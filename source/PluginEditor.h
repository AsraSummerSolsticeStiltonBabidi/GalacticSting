#pragma once

#include "PluginProcessor.h"
#include "ScaleKeyboard.h"
#include "NoteNames.h"
#include "PartialAmpEdit.h"
#include "IntervalTokens.h"
#include "GalacticPalette.h"
#include "dsp/Dissonance.h"
#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <vector>
//==============================================================================
// THE TITLES OF THE NATIVE DIALOGS, in ONE place.
//
// A NativeMessageBox is an OS window, not a juce::Component: it is outside the editor the
// snapshot rig photographs and outside the tree walkTextFits descends, so a title has no
// gate of any kind on it. Six of them went out in English under a body that changed
// language properly - invisible from inside the plugin, obvious the moment a Catalan user
// presses the button.
//
// Both the call sites and the probe read THESE pairs, so the check cannot go stale by
// copying: a title that drifts here drifts in the report on the same build.
//
// EVERY ACCENT IN HERE IS AN ESCAPE, and the two that were not are why this paragraph
// exists. The two "afinacio" titles carried their accented o as raw source bytes, which is
// correct today only because CMakeLists passes /utf-8: without it MSVC reads a BOM-less
// file in the system codepage and one character becomes two. That is exactly the accident
// documented further down this file - a name drawing with an extra capital A-tilde in all
// 42 main-editor snapshots - and a title bar is the one place in this plugin where nobody
// could photograph it, because Windows draws it. Latent, not live; latent in the one
// string no gate can see.
namespace fgdlg
{
    struct Pair { const char* en; const char* ca; };

    inline constexpr Pair loadScale  { "Could not load scale",  "No s'ha pogut carregar l'escala" };
    inline constexpr Pair loadTimbre { "Could not load timbre", "No s'ha pogut carregar el timbre" };
    inline constexpr Pair saveScale  { "Save scale",            "Desar escala" };
    inline constexpr Pair rhythmHelp { "Spectrum vs Rhythm",    "Espectre vs ritme" };
    inline constexpr Pair mpeOut     { "MPE out required",      "Cal activar MPE out" };

    // The FILE CHOOSER titles are the same hole one ring out: a chooser is an OS window
    // too, and three of these five were English while the other two already used tr.
    inline constexpr Pair openTuning { "Load a tuning (.tun / .scl / .ascl)",
                                       "Carrega una afinaci\xc3\xb3 (.tun / .scl / .ascl)" };
    inline constexpr Pair openBank   { "Load a scale into the tuning bank (.tun / .scl / .ascl)",
                                       "Carrega una escala al banc d'afinacions (.tun / .scl / .ascl)" };
    inline constexpr Pair openTimbre { "Load a scale file as timbre (.tun / .scl / .ascl)",
                                       "Carrega un fitxer d'escala com a timbre (.tun / .scl / .ascl)" };

    // These two were already correct. They join the table anyway: the gate is only worth
    // anything if it holds the whole set, and a title that is right today is exactly the
    // one nobody re-reads.
    inline constexpr Pair saveTuning { "Save tuning", "Desa l'afinaci\xc3\xb3" };
    inline constexpr Pair saveGen    { "Save the generated scale (.scl)",
                                       "Desa l'escala generada (.scl)" };

    inline constexpr Pair all[] = { loadScale, loadTimbre, saveScale, rhythmHelp, mpeOut,
                                    openTuning, openBank, openTimbre, saveTuning, saveGen };
}

//==============================================================================
// THE PROCESSOR'S REFUSALS, IN THE USER'S LANGUAGE.
//
// tr() is an EDITOR member, so the processor cannot call it - and the processor is where
// every parser and loader lives. The result is a Catalan window whose failures answer in
// English: a translated title over an English body, which is the half-finished sentence
// this project's own rules forbid.
//
// The audit's fix is to make every refusal a typed code the editor turns into words. That
// is the right end state and it is 74 call sites, many of them composing the message with
// a runtime value ("Key " + n, "File declares " + count), so it cannot be done as a table
// at all. Counted here rather than guessed: 61 messages, of which 30 are composed.
//
// THE OTHER 31 ARE FIXED STRINGS, and those need no design change: the editor already has
// them in its hand by the time it shows them, so it can simply look them up. That is this
// table. It is deliberately NOT a translation layer for everything - a composed message
// falls through unchanged, exactly as today, so nothing can regress and this can stop at
// any point without leaving the product in a worse state than it started.
//
// The gate is the important half. RhythmVerify reads THE SOURCE for every `errorOut = "…";`
// literal and asserts each one is in this table - the same trick fgdlg above uses for
// window titles, and for the same reason: a second copy of a string goes stale the moment
// somebody rewords the first. A message added tomorrow fails the suite until it is here.
namespace fgerr
{
    struct Pair { const char* en; const char* ca; };

    inline constexpr Pair table[] =
    {
        { "A scale needs at least one degree per period.",
          "Una escala necessita com a m\xc3\xadnim un grau per per\xc3\xado" "de." },
        { "Could not write the file.",
          "No s'ha pogut escriure el fitxer." },
        { "File not found.",
          "No s'ha trobat el fitxer." },
        // The fallback reason tonight's restore / bank-add refusals use when a parser said
        // nothing at all - it has to translate like every other reason (night review).
        { "Invalid value.",
          "Valor no v\xc3\xa0lid." },
        { "The copy of this preset saved inside the file is damaged.",
          "La c\xc3\xb2pia d'aquest preset desada dins del fitxer est\xc3\xa0 malmesa." },
        { "Invalid period.",
          "Per\xc3\xado" "de inv\xc3\xa0lid." },
        // These two carry escaped quotes and are still WHOLE messages - the examples are
        // part of the sentence, not a value pasted in. The audit's first classifier called
        // them composed for the backslashes alone and would have left them English for ever.
        { "Invalid value. Use cents with a decimal point (\"702.0\") or a ratio (\"3/2\").",
          "Valor inv\xc3\xa0lid. Fes servir cents amb punt decimal (\"702.0\") o un ratio (\"3/2\")." },
        { "Invalid period. Use a ratio (\"2\", \"3/2\") or cents (\"1200c\").",
          "Per\xc3\xado" "de inv\xc3\xa0lid. Fes servir un ratio (\"2\", \"3/2\") o cents (\"1200c\")." },
        { "Invalid pitch.",
          "To inv\xc3\xa0lid." },
        { "Invalid scale period.",
          "Per\xc3\xado" "de d'escala inv\xc3\xa0lid." },
        { "No generated scale to save yet.",
          "Encara no hi ha cap escala generada per desar." },
        { "No such chord.",
          "Aquest acord no existeix." },
        { "No target notes to adapt to.",
          "No hi ha notes objectiu a les quals adaptar-se." },
        { "Not a valid .scl file (too short).",
          "No \xc3\xa9s un fitxer .scl v\xc3\xa0lid (massa curt)." },
        { "Not a valid .tun file: no [Exact Tuning] or [Tuning] notes found.",
          "No \xc3\xa9s un fitxer .tun v\xc3\xa0lid: no s'hi ha trobat cap nota [Exact Tuning] ni [Tuning]." },
        { "Nothing adapted yet.",
          "Encara no s'ha adaptat res." },
        { "Nothing to restore.",
          "No hi ha res a restaurar." },
        { "That file is not a Fibl\xc3\xb3 preset.",
          "Aquest fitxer no \xc3\xa9s un preset de Fibl\xc3\xb3." },
        { "That pitch is already a degree of the scale.",
          "Aquest to ja \xc3\xa9s un grau de l'escala." },
        { "That preset could not be read.",
          "No s'ha pogut llegir aquest preset." },
        { "That preset no longer exists.",
          "Aquest preset ja no existeix." },
        { "That tuning is already the active one.",
          "Aquesta afinaci\xc3\xb3 ja \xc3\xa9s l'activa." },
        { "The current settings produce no partials - nothing to analyse.",
          "Els ajustos actuals no produeixen cap parcial - no hi ha res a analitzar." },
        { "The current tuning has no usable pitch for MIDI note 0.",
          "L'afinaci\xc3\xb3 actual no t\xc3\xa9 cap to utilitzable per a la nota MIDI 0." },
        { "The tonic and the period cannot be deleted - delete any degree between them.",
          "La t\xc3\xb2nica i el per\xc3\xado" "de no es poden esborrar - esborra qualsevol grau que hi hagi entre mig." },
        { "The tonic is fixed at 1/1 - edit any other degree.",
          "La t\xc3\xb2nica \xc3\xa9s fixa a 1/1 - edita qualsevol altre grau." },
        { "This .tun has an unusable basefreq.",
          "Aquest .tun t\xc3\xa9 una basefreq inservible." },
        { "This preset carries no tuning of its own.",
          "Aquest preset no porta cap afinaci\xc3\xb3 pr\xc3\xb2pia." },
        { "This spectrum does not come from a tuning.",
          "Aquest espectre no prov\xc3\xa9 de cap afinaci\xc3\xb3." },

        // The long ones. They are wrapped across several source lines for the column rule,
        // which is not the same thing as being composed - the audit's first pass counted
        // nineteen of these as impossible to translate for that reason alone.
        { "This .tun does not repeat at any interval, so it states no degrees for the Custom Engine to use. Load it as a TUNING instead - it will sound exactly as written.",
          "Aquest .tun no es repeteix a cap interval, o sigui que no declara cap grau que el Motor propi pugui fer servir. Carrega'l com a AFINACI\xc3\x93 - sonar\xc3\xa0 exactament com est\xc3\xa0 escrit." },
        { "Type at least one interval - \"1\" on its own is a sine, and \"1 3/2 2\" is a fundamental, a fifth and an octave period.",
          "Escriu com a m\xc3\xadnim un interval - \"1\" tot sol \xc3\xa9s un sinus, i \"1 3/2 2\" s\xc3\xb3n un fonamental, una quinta i un per\xc3\xado" "de d'octava." },
        { "This tuning is a 128-key table (a .tun that does not repeat), not a scale, so its degrees cannot be edited - an edit would have to throw the table away. Save it as .tun and edit the keys there.",
          "Aquesta afinaci\xc3\xb3 \xc3\xa9s una taula de 128 tecles (un .tun que no es repeteix), no una escala, o sigui que els seus graus no es poden editar - editar-los obligaria a llen\xc3\xa7""ar la taula. Desa-la com a .tun i edita-hi les tecles." },
        { "Temper mode is on, so the harmonic engine's partials are snapped to the tuning and a Custom-Engine spectrum is bypassed entirely - the adapted spectrum would be written and never heard. Switch temper off first.",
          "El mode temperat est\xc3\xa0 actiu, o sigui que els parcials del motor harm\xc3\xb2nic s'ajusten a l'afinaci\xc3\xb3 i un espectre del Motor propi queda completament de banda - l'espectre adaptat s'escriuria i no se sentiria mai. Apaga primer el temperat." },
        { "This tuning is a 128-key table (a .tun that does not repeat), and a .scl can only hold degrees repeating at a period. Save it as .tun instead - that keeps every key exactly.",
          "Aquesta afinaci\xc3\xb3 \xc3\xa9s una taula de 128 tecles (un .tun que no es repeteix), i un .scl nom\xc3\xa9s pot contenir graus que es repeteixin a un per\xc3\xado" "de. Desa-la com a .tun - aix\xc3\xad es mant\xc3\xa9 cada tecla exactament." },
        { "This preset was saved with temper mode on, which builds its spectrum from the tuning - an adapted spectrum would never be heard.",
          "Aquest preset es va desar amb el mode temperat actiu, que constru\xc3\xaf""x el seu espectre a partir de l'afinaci\xc3\xb3 - un espectre adaptat no se sentiria mai." },
        { "This tuning has no repeating scale behind it (an MTS-ESP master can supply an arbitrary 128-key table, and a descending or period-1 scale repeats at nothing), so it cannot be written as a scale. Nothing was saved.",
          "Aquesta afinaci\xc3\xb3 no t\xc3\xa9 cap escala repetitiva al darrere (un mestre MTS-ESP pot donar una taula arbitr\xc3\xa0ria de 128 tecles, i una escala descendent o de per\xc3\xado" "de 1 no es repeteix mai), o sigui que no es pot escriure com a escala. No s'ha desat res." },
        { "The period can only be set by hand for a 128-key tuning table; a scale's period comes from the scale itself.",
          "El per\xc3\xado" "de nom\xc3\xa9s es pot posar a m\xc3\xa0 en una taula d'afinaci\xc3\xb3 de 128 tecles; el per\xc3\xado" "de d'una escala surt de l'escala mateixa." },
        { "This .tun does not repeat at any interval, so it cannot be held as a scale (degrees + period).",
          "Aquest .tun no es repeteix a cap interval, o sigui que no es pot guardar com a escala (graus + per\xc3\xado" "de)." },
    };

    // ...AND THE ONES THAT CARRY A VALUE, AS TEMPLATES.
    //
    // A composed refusal is a fixed sentence with the user's own text or a number dropped
    // into it - `"Bad ratio: \"" + whatTheyTyped + "\""`. There is no whole string to look
    // up, but there IS a template: every string literal in the statement is a fixed chunk
    // and every gap between them is a value, so joining the literals with '%' reconstructs
    // it exactly. RhythmVerify builds these templates FROM THE SOURCE and asserts each is
    // here, so this list cannot drift from the code and cannot quietly miss a new one.
    //
    // The Catalan side uses the same number of '%' in the order the sentence needs them -
    // which is the reason this is a template and not two half-sentences glued together:
    // Catalan does not put the value where English does, and a concatenation cannot move it.
    inline constexpr Pair patterns[] =
    {
        { "Bad cents value: \"%\"",
          "Valor en cents incorrecte: \"%\"" },
        { "Bad ratio: \"%\"",
          "Ratio incorrecte: \"%\"" },
        { "Not an interval: \"%\"",
          "Aix\xc3\xb2 no \xc3\xa9s un interval: \"%\"" },
        // THE WHOLE SENTENCE, not a prefix of it. These two held only their opening
        // clause - "...inside a " and "...% partials" - and trErr matches a template
        // against the COMPLETE message, so neither could ever fire: the Catalan reader got
        // the English sentence, and the gate that counts untranslated refusals reported
        // none, because a pattern that never matches is not a pattern that failed.
        // TWO OF THEM, and they are two messages. The realtime follower's version stops
        // at the period; the one the user asks for by hand goes on to explain what to do
        // about it. Only the long one was in the table, so the short one - which is the
        // one a REALTIME corner produces, the more frequent of the two - reached a Catalan
        // reader in English.
        { "Those % partials cannot all keep % cents of air inside a %-cent period.",
          "Aquests % parcials no poden mantenir tots % cents d'aire dins d'un per\xc3\xado" "de "
          "de % cents." },
        { "Those % partials cannot all keep % cents of air inside a %-cent period (the period "
          "itself counts: the spectrum repeats there). Use fewer partials, a smaller smallest "
          "interval, or a wider period.",
          "Aquests % parcials no poden mantenir tots % cents d'aire dins d'un per\xc3\xado" "de "
          "de % cents (el per\xc3\xado" "de mateix hi compta: l'espectre s'hi repeteix). Fes "
          "servir menys parcials, un interval m\xc3\xadnim m\xc3\xa9s petit, o un per\xc3\xado" "de "
          "m\xc3\xa9s ample." },
        // TWO ENTRIES FOR ONE MESSAGE, because the scope is a LITERAL chosen at runtime
        // (", 0 vs all" or ", all vs all"), not a value. Captured as a value it would be
        // re-emitted verbatim - an English fragment inside a Catalan sentence - so each
        // branch is its own template with its own translation.
        { "This search is % notes x % partials, 0 vs all, % registers - past the hard cap "
          "(about % minutes of computation; on this machine an estimated %). Fewer partials, "
          "fewer registers, or the \"0 vs all\" scope will bring it inside.",
          "Aquesta cerca \xc3\xa9s de % notes x % parcials, 0 contra tots, % registres - per "
          "sobre del l\xc3\xadmit dur (uns % minuts de c\xc3\xa0lcul; en aquesta m\xc3\xa0quina, "
          "uns %). Amb menys parcials, menys registres, o l'abast \"0 contra tots\" hi cabr\xc3\xa0." },
        { "This search is % notes x % partials, all vs all, % registers - past the hard cap "
          "(about % minutes of computation; on this machine an estimated %). Fewer partials, "
          "fewer registers, or the \"0 vs all\" scope will bring it inside.",
          "Aquesta cerca \xc3\xa9s de % notes x % parcials, tots contra tots, % registres - per "
          "sobre del l\xc3\xadmit dur (uns % minuts de c\xc3\xa0lcul; en aquesta m\xc3\xa0quina, "
          "uns %). Amb menys parcials, menys registres, o l'abast \"0 contra tots\" hi cabr\xc3\xa0." },
        { "The adaptation came back with % partials instead of the number asked for. Nothing was written.",
          "L'adaptaci\xc3\xb3 ha tornat amb % parcials en comptes dels demanats. No s'ha escrit res." },
        { "This corner's adaptation is estimated at % minutes per search - it cannot follow anything at that price.",
          "L'adaptaci\xc3\xb3 d'aquesta cantonada s'estima en % minuts per cerca - a aquest preu no pot seguir res." },
        { "Key % of this scale is not a usable frequency - its degrees or its period run off the end of what a number can hold. Nothing was loaded.",
          "La tecla % d'aquesta escala no \xc3\xa9s una freq\xc3\xbc\xc3\xa8ncia utilitzable - els seus graus o el seu per\xc3\xado" "de se surten del que un n\xc3\xbamero pot contenir. No s'ha carregat res." },
        { "Key % of this tuning table is not a usable frequency. Nothing was loaded.",
          "La tecla % d'aquesta taula d'afinaci\xc3\xb3 no \xc3\xa9s una freq\xc3\xbc\xc3\xa8ncia utilitzable. No s'ha carregat res." },
        { "Bad basefreq in .tun: \"%\".",
          "basefreq incorrecta al .tun: \"%\"." },
        // saveCurrentTuningTun, in PluginProcessor.h - a file the refusal walk did not
        // read until 2026-09-09, so this one had no translation and nothing said so.
        { "The tuning bank is full (% slots). Nothing was stored and nothing was overwritten - remove a tuning from the bank with its X to make room.",
          "El banc d'afinacions \xc3\xa9s ple (% ranures). No s'ha desat res i no s'ha sobreescrit res - treu una afinaci\xc3\xb3 del banc amb la seva X per fer lloc." },
        { "The tuning bank is full (% slots), so this tuning did not join the bank. It IS loaded and sounding; remove a tuning from the bank with its X to make room for it.",
          "El banc d'afinacions \xc3\xa9s ple (% ranures), o sigui que aquesta afinaci\xc3\xb3 no s'ha afegit al banc. S\xc3\xad que est\xc3\xa0 carregada i sonant; treu una afinaci\xc3\xb3 del banc amb la seva X per fer-li lloc." },
        { "Could not write %",
          "No s'ha pogut escriure %" },
        { "Bad value for note % in .tun: \"%\".",
          "Valor incorrecte per a la nota % del .tun: \"%\"." },
        { "Note % in this .tun is not a finite pitch.",
          "La nota % d'aquest .tun no \xc3\xa9s un to finit." },
        { "Note % in this .tun is not a usable frequency.",
          "La nota % d'aquest .tun no \xc3\xa9s una freq\xc3\xbc\xc3\xa8ncia utilitzable." },
        { "This .tun only specifies % of the 128 MIDI notes. Nothing was loaded - the rest would have to be invented.",
          "Aquest .tun nom\xc3\xa9s especifica % de les 128 notes MIDI. No s'ha carregat res - la resta s'hauria d'inventar." },
        { "Invalid note count (%).",
          "Nombre de notes inv\xc3\xa0lid (%)." },
        { "File declares % notes but has fewer.",
          "El fitxer declara % notes per\xc3\xb2 en t\xc3\xa9 menys." },
        { "Bad pitch on line %: \"%\"",
          "To incorrecte a la l\xc3\xadnia %: \"%\"" },
    };
}


//==============================================================================
// HOW MUCH ROOM A CONTROL LEAVES FOR ITS OWN WORD, AND HOW WIDE IT MUST BE TO HOLD IT.
//
// "haz un repaso de todos los textos que quedan cortados como 'not in this spectrum' y
//  arreglalos"
//
// Three different places need these numbers and they have to agree: the LookAndFeel that
// PAINTS the control, resized() which SIZES it, and the text sweep which CHECKS it. They
// did not agree - the tuning buttons were sized with a plain 12 pt font while the
// LookAndFeel drew them in ~15 pt bold, and every caption was sized as if a juce::Label
// drew into its whole width when JUCE reserves 5 px a side - so "Afegeix al banc" was
// 24 px too narrow and drawFittedText silently squashed it. This is a bilingual product:
// Catalan runs 20-40% longer than English, so a width that is a hard-coded number is a
// truncation waiting for a language switch.
//
// Each function mirrors exactly the painter named beside it. Nothing here guesses.
// THE "NEAREST JUST INTERVAL" NAMER, out here where a test can call it.
//
// It used to live in an anonymous namespace inside PluginEditor.cpp, which is why it had
// no test and why both of these shipped:
//
//   * 1000 cents was named "~ 7/4", which is 31.2 cents away, while 9/5 is 17.6 and 16/9
//     is 3.9. The near-field pick claimed in its own comment to be "the closest thing"
//     and was in fact the SIMPLEST within 35 cents, which is a different question.
//   * 100 cents was named "~ 14/13" at -28 cents. 17/16 is 5.0 away and 18/17 is 1.0,
//     and BOTH were excluded by a `simp <= 200` cap - while the comment beside it claimed
//     the last tier "catches 17/16 (100c is only ~1c away from it)". It does not catch
//     it, and ~1c is 18/17, not 17/16. Two wrong statements in one sentence.
//
// The rule now: a ratio may be CLAIMED as an approximation when its error fits a window
// that shrinks with its complexity - 90/sqrt(p*q), capped at 18 cents - and among those
// the SIMPLEST wins, so 700c is 3/2 and not some closer obscurity. Separately, the
// nearest neighbour is exactly that: smallest error, ties to the simpler ratio, which is
// what its comment always said it was.
namespace fgjust
{
    struct JustHint
    {
        bool      has     = false;  long long p  = 1, q  = 1; double err     = 0.0;
        bool      hasNear = false;  long long np = 1, nq = 1; double nearErr = 0.0;
    };
    inline JustHint nearestJust (double cents)
    {
        JustHint h;
        if (! std::isfinite (cents)) return h;
        const double ratio = std::exp2 (cents / 1200.0);
        if (! (ratio > 0.0) || ! std::isfinite (ratio)) return h;

        double bestSimp = 1.0e18, nearBest = 1.0e18, nearSimp = 1.0e18;
        for (long long q2 = 1; q2 <= 17; ++q2)
        {
            const long long p2 = (long long) std::llround (ratio * (double) q2);
            if (p2 <= 0 || std::gcd (p2, q2) != 1) continue;
            const double err  = cents - 1200.0 * std::log2 ((double) p2 / (double) q2);
            const double simp = (double) (p2 * q2);
            const double aerr = std::abs (err);
            // CLAIMED: close enough for how complex it is, and of those the simplest.
            // TWO WAYS TO EARN THE NAME, because one law cannot do both jobs. A single
            // window of the form K/sqrt(p*q) has to pass 8/5 at 13.7 cents (800c, a
            // documented case), and that forces K >= 87 - which then hands every complex
            // ratio a window wide enough to cover 83% of the octave. Measured: the "*"
            // stopped meaning anything. So:
            //   * a SIMPLE ratio may be named loosely - it is the one a player would name
            //     anyway, and 12-EDO's thirds and sixths are 14 cents off theirs;
            //   * a complex one only when it is close in ABSOLUTE terms, where "17/16"
            //     is a real statement about a 5-cent interval and not a shrug.
            // Among everything that qualifies, the simplest wins, so 700c stays 3/2.
            const double tol = juce::jmin (18.0, 90.0 / std::sqrt (simp));
            const bool   ok  = (simp <= 100.0 && aerr <= tol)      // simple, loosely
                            || (simp <= 306.0 && aerr <= 6.0);     // complex, but really there
            if (ok && simp < bestSimp)
            { bestSimp = simp; h.has = true; h.p = p2; h.q = q2; h.err = err; }
            // NEAREST: the closest, full stop - ties broken towards the simpler ratio.
            // Capped at 300 so it names something a musician recognises: at 1000c that is
            // 16/9 at -3.9c, where a cap of 400 let in 25/14 at -3.8c, which is nearer by
            // a tenth of a cent and tells nobody anything.
            if (simp <= 300.0 && aerr <= 45.0
                 && (aerr < nearBest - 1.0e-9 || (aerr < nearBest + 1.0e-9 && simp < nearSimp)))
            { nearBest = aerr; nearSimp = simp; h.hasNear = true; h.np = p2; h.nq = q2; h.nearErr = err; }
        }
        return h;
    }
}

namespace fgfit
{
    // GalacticLnF::getTextButtonFont.
    inline juce::Font textButtonFont (int h)
    { return juce::Font (juce::FontOptions (juce::jmin (16.0f, (float) h * 0.62f)).withStyle ("Bold")); }
    // The drawn tuning-fork glyph plus its gap (GalacticLnF::drawButtonText).
    inline float textButtonForkWidth (int h)
    {
        const float fh = juce::jmin (13.0f, (float) h - 4.0f);
        return (float) ((int) std::ceil (fh * 0.62f) + 3) + 2.0f;
    }
    // GalacticLnF::drawButtonText reduces the bounds by 4 px each side.
    inline float textButtonRoom (int w, int h, bool fork)
    { return (float) w - 8.0f - (fork ? textButtonForkWidth (h) : 0.0f); }
    inline int textButtonWidth (const juce::String& text, int h, bool fork)
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (textButtonFont (h), text))
             + 8 + (fork ? (int) std::ceil (textButtonForkWidth (h)) : 0) + 2; // +2: rounding
    }
    inline int textButtonWidth (const juce::Button& b)
    { return textButtonWidth (b.getButtonText(), b.getHeight(), b.getProperties().contains ("fork")); }

    // GalacticLnF::drawToggleButton: ONE tick box size and ONE gap to the word, for every
    // ordinary toggle in the window.
    //
    // These three numbers replace LookAndFeel_V4's formula, which derived the box from the
    // BUTTON'S HEIGHT - jmin (15, h * 0.75) * 1.1, with the text at roundToInt (that) + 10.
    // A block 215 px wide inside PRIME CONTROL therefore drew three different tick boxes
    // (15x15, 18x18 and a NON-SQUARE 18x16, that last one because a fractional height puts
    // the outline between pixels) and two different label gaps, purely because its three
    // toggles were laid out in rows of three different heights. A tick box is a glyph, not
    // a scaled part of its row.
    //
    // The painter reads these, and so do toggleRoom / toggleWidth below - which is what
    // stops the sweep that checks a word fits from measuring a geometry nothing draws.
    inline constexpr int kTickX   = 4;   // the box's left inset (V4's own, kept)
    inline constexpr int kTickBox = 16;  // the box itself, square, at every button height
    inline constexpr int kTickGap = 8;   // box right edge -> the word's cell
    // The BINAURAL compartment stacks its painted title over its fader (see the ecBin
    // block in resized(), and paintedSubLabels()). Same 13 px the essentials cells use
    // when they stack, in ONE place, because two of them have to agree about where the
    // word is drawn and where the room for it was reserved.
    inline constexpr int kBinTitleH = 13;
    inline juce::Font toggleFont (int h)
    { return juce::Font (juce::FontOptions (juce::jmin (15.0f, (float) h * 0.75f))); }
    inline float toggleRoom (int w, int h)
    { juce::ignoreUnused (h); return (float) w - (float) (kTickX + kTickBox + kTickGap) - 2.0f; }
    inline int toggleWidth (const juce::String& text, int h)
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (toggleFont (h), text))
             + kTickX + kTickBox + kTickGap + 2 + 2; // + the painter's right margin, + rounding
    }
    inline int toggleWidth (const juce::ToggleButton& b)
    { return toggleWidth (b.getButtonText(), b.getHeight()); }
    // ...AND THE TICK BOX ON ITS OWN, before a single letter of text.
    //
    // drawToggleButton puts the box at x = 4 with a side of jmin(15, h*0.75) * 1.1, so a
    // ToggleButton needs 4 + that much width just to CONTAIN its own tick. The nine prime
    // checkboxes have no button text at all - their numbers are separate Labels under
    // them - so every width measurement in this file reads them as empty strings and none
    // of the three text checks could ever fire on them. At 690x550 they are laid out
    // 6 px wide against a 20.5 px tick: what is drawn is a 2 px sliver of a box, nothing
    // can be aimed at, and the state of the whole prime filter is unreadable. This is the
    // number that says so.
    inline int toggleTickWidth (int h) noexcept
    { return 4 + juce::roundToInt (juce::jmin (15.0f, (float) h * 0.75f) * 1.1f); }

    // GalacticLnF::drawToggleButton's "onled" pill: a state LED and the word ON / OFF,
    // the pair centred in the WHOLE button, bold, at h * 0.62. The pill has no
    // juce::Button text at all (the painted sub-label above it is its name), so every
    // other measurement in this file reads it as an empty string and would happily size
    // it to nothing.
    inline juce::Font onLedFont (int h)
    { return juce::Font (juce::FontOptions ((float) h * 0.62f).withStyle ("Bold")); }
    // THE LED ITSELF - a filled disc when on, a hollow ring when off.
    //
    // A SECOND CUE THAT IS NOT A HUE. The pill is a saturated green when on and a
    // saturated red when off, and green-vs-red is the one distinction a deuteranope or a
    // protanope does not get: to about one man in twelve those two fills are the same
    // muddy ochre. The word inside it has always said ON or OFF, so the pill was never
    // colour-ALONE - but a three-letter word at h * 0.62 (15 px in the rhythm panel,
    // 13 in the SPECTRUM one) has to be READ, and solid-versus-hollow is told apart at a
    // glance, out of the corner of an eye, and in a thumbnail. Redundant on purpose: hue,
    // word and shape all say the same thing, and any one of the three is enough.
    //
    // Floored at 5 px because a ring needs an inside: below that the 1.6 px pen closes up
    // and the hollow one is a filled dot in a lighter colour, which is no cue at all.
    //
    // TURNED OFF BY THE USER, 2026-09-08: "aparecio un circulo en el boton on/off del
    // lapis para editar la amplitud de parciales, quitalo." Their instrument, their call -
    // and the reasoning above is left standing rather than deleted, because it is the
    // argument for putting it back and it should not have to be re-derived. Flipping this
    // one flag restores the disc everywhere, at the right width, in every measurement:
    // the painter, the fit sweep and the snug widths all read onLedLead() below, so
    // nothing has a hand copy of the geometry to go stale. It was on EVERY ON/OFF pill,
    // not only the pencil's, so it goes from all of them - one pill with a dot and three
    // without is a worse answer than either.
    inline constexpr bool kOnLedDotShown = false;
    inline float onLedDot (int h)
    { return kOnLedDotShown ? juce::jmax (5.0f, std::round ((float) h * 0.26f)) : 0.0f; }
    inline constexpr float kOnLedGap = 4.0f;   // the disc's right edge -> the word
    // The whole lead-in the word sits behind: the disc and its gap, or nothing at all.
    // ONE expression, so "no disc" cannot mean "no disc but still paying for its gap".
    inline float onLedLead (int h)
    { return kOnLedDotShown ? onLedDot (h) + kOnLedGap : 0.0f; }
    // What the painter really lays down, so the fit sweep and the snug widths measure the
    // pill that is drawn rather than the one it used to draw.
    //
    // ...AND A PIXEL OF SLACK ON THE WORD, because the painter and this function measure
    // it by two different routes. drawToggleButton hands drawText a rect exactly this
    // wide, and juce::Graphics::drawText curtails against its OWN measurement of the same
    // string - GlyphArrangement::addCurtailedLineOfText, not getStringWidth - so a float
    // difference of a fraction of a pixel is enough to drop the last glyph of "OFF". A
    // switch reading "OF" is worse than one reading nothing, because it looks deliberate.
    inline float onLedInkWidth (int h, const juce::String& word)
    { return onLedLead (h)
           + juce::GlyphArrangement::getStringWidth (onLedFont (h), word) + 1.0f; }
    inline int onLedWidth (int h)  // "OFF" is the longer of the two words
    { return (int) std::ceil (onLedInkWidth (h, "OFF")) + 2; }

    // THE GUTTER THE CHEVRON REALLY OCCUPIES, taken off the very path that draws it.
    //
    // GalacticLnF::drawComboBox strokes the chevron centred at x = width - h*0.55, with a
    // half-width of h*0.15 and a 1.6 px round cap, so its LEFT ink edge sits at
    // width - h*0.70 - 0.8. Everything below used to reserve h/2 instead - h*0.20 + 0.8
    // px SHORT of it - and the difference was not theoretical: at h = 24 the label's text
    // area ended 17 px from the right edge while the chevron's ink began at 16.8, so the
    // two ranges overlapped by 0.2 px. Photographed at 1200x650, the final "g" of "Find
    // tunings based on beating" TOUCHED the chevron with zero gap, in English and in
    // Catalan, and "Whitelist" did the same at 690x550 - while the chevron had 9.6 px of
    // clear pill to its RIGHT and none at all to its left. Nothing went red, because the
    // probe that checks the fit read this same understated formula.
    //
    // + 3 px, so the word and the arrow are separated rather than merely not overlapping.
    // TRIED AND REJECTED: moving the chevron right instead (cx = width - h*0.45) would
    // give 6 of those 9 px back to the text, but the pill's right end is a semicircle of
    // radius h/2 and the glyph then sits inside the curve, which reads as a chevron
    // falling off the box. The gutter is the honest 21 px at h = 24, and snugComboWidth
    // below asks for it, so a box sized to its own words simply gets 9 px wider.
    inline int comboArrowGutter (int boxHeight) noexcept
    { return (int) std::ceil ((float) boxHeight * 0.70f + 0.8f) + 3; }

    // GalacticLnF::positionComboBoxText: the label is inset by height/3 on the left and
    // has to stop clear of the chevron (comboArrowGutter above). Three places need this
    // number and they have to agree: the painter, the resized() that sizes a box to the
    // words it holds, and the probe that checks it. A ComboBox neither clips visibly nor
    // resizes - it draws as much of the item as the cell happens to hold - so a formula
    // copied out by hand let "Sync Project Tempo" appear on screen as "Sync Project Te"
    // with every check in the build still green.
    inline int comboTextWidth (int boxWidth, int boxHeight) noexcept
    { return boxWidth - boxHeight / 3 - comboArrowGutter (boxHeight); }
    // ...and the inverse: the box width a word of `textWidth` needs.
    inline int comboWidthForText (int textWidth, int boxHeight) noexcept
    { return textWidth + boxHeight / 3 + comboArrowGutter (boxHeight) + 2; } // +2: the label's own rounding

    // ...and two more things stand between that room and the ITEM the user reads, both
    // of which a width check has to know about or it certifies boxes that truncate:
    //   - juce::Label keeps its own {1,5,1,5} border off the bounds it is given, so ten
    //     more pixels never hold text;
    //   - the item is drawn in GalacticLnF::getLabelFont's face, which is BOLD with a
    //     12.5 px floor - not in the lighter face LookAndFeel_V4::getComboBoxFont sizes
    //     the label with.
    inline constexpr int kComboLabelBorder = 10;      // juce::Label's default 5 px a side

    // A COMBO WHOSE ITEM READS BELOW THE TITLE ABOVE IT, opted into by NAME - the same
    // idiom GalacticLnF already uses to turn a ToggleButton into an ON/OFF pill
    // ("onled"). Every other box in this window is a VALUE under a painted sub-label and
    // is meant to be the louder of the two; the TRANSITION box holds what used to BE two
    // painted sub-labels ("haz que ... esos nombres sean mas pequenos"), so its item has
    // to sit a step under the 11.5 px title rather than 1 px over it, which is where the
    // ordinary rule's 12.5 px floor would put it.
    inline constexpr const char* kSmallComboName = "smallcombo";
    inline juce::Font comboItemFont (int h, bool small)
    {
        if (small)   // 10..11 px: under the 11.5 px painted sub-label at every box height
            return juce::Font (juce::FontOptions (juce::jmax (10.0f, juce::jmin (11.0f, (float) h * 0.62f)))
                                   .withStyle ("Bold"));
        return juce::Font (juce::FontOptions (juce::jmax (12.5f, juce::jmin (16.0f, (float) h * 0.85f)))
                               .withStyle ("Bold"));
    }
    inline juce::Font comboItemFont (int h) { return comboItemFont (h, false); }
    // The SAME question asked of a box rather than a number, so the painter, the resized()
    // that sizes the box to its words and the probe that checks they fit cannot disagree
    // about which of the two faces this box is drawn in.
    inline juce::Font comboItemFont (const juce::ComboBox& c, int h)
    { return comboItemFont (h, c.getName() == kSmallComboName); }
    inline int comboItemRoom (int boxWidth, int boxHeight) noexcept
    { return comboTextWidth (boxWidth, boxHeight) - kComboLabelBorder; }
    inline int comboWidthForItem (int itemWidth, int boxHeight) noexcept
    { return comboWidthForText (itemWidth + kComboLabelBorder, boxHeight); }

    // THE ONE PADDING RULE for a control sized to the words it actually holds.
    //
    // "hay varios botones y desplegables ahora mismo que son absurdamente anchos,
    //  reduce su anchura". Six controls were taking a share of their row - a half, a
    //  third, whatever was left - so a pill with the word "OFF" on it was 275 px wide
    //  and "Recent..." was 300. Each is now sized to its own text, and they all add the
    //  SAME breathing room on top of the painter's bare minimum so the panel reads as
    //  one system rather than six separate decisions about what looks about right.
    //
    // The minimum each function above returns is the painter's own: exactly enough for
    // the glyphs plus the insets that painter reserves. kSnugPad is the air around them.
    inline constexpr int kSnugPad = 12;
    inline int snugTextButtonWidth (const juce::TextButton& b, int h)
    { return textButtonWidth (b.getButtonText(), h, b.getProperties().contains ("fork")) + kSnugPad; }
    inline int snugToggleWidth (const juce::ToggleButton& b, int h)
    { return (b.getName() == "onled" ? onLedWidth (h) : toggleWidth (b.getButtonText(), h)) + kSnugPad; }
    // A TAB CHIP.
    //
    // "haz que los botones harmonic/custom tengan el mismo tamano que los de los
    //  sub-menus dentro del menu modulator (pitch, filter, custom, etc)"
    //
    // The engine chips (HARMONIC / CUSTOM) and the modulator sub-tabs (AMP / PITCH /
    // FILTER / CUSTOM) are the same control doing the same job - a row of exclusive
    // chips that titles the box under it and recolours it - so they are one size, one
    // band height, one inset, one font. They were 104 x 16 and 58 x 19: the engine pair
    // was wider AND shorter, and its text a whole point smaller, because each row
    // carried its own literals.
    //
    // The corner radius needs no number here: GalacticLnF::drawButtonBackground fills
    // every button that is not named "round" as a square rect, so both rows already
    // share it and a radius stated here would be a second, drifting copy of nothing.
    namespace chip
    {
        inline constexpr int kHeight = 19;  // the band a chip row occupies, chips filling it
        // A CHIP STRIP STANDS IN A CONTROL ROW, not in a band of its own height.
        //
        // The strip and the panel beside it start on the same y, and a 19 px chip next to
        // a 24 px combo puts their two words 2.5 px apart - a line across the window that
        // is very nearly straight, which reads worse than one that obviously is not. The
        // chip keeps its own 19 px (it is shared with the engine strip, and growing it
        // would move that too); it is CENTRED in a row's height instead, which lands both
        // baselines on the same pixel.
        inline constexpr int kRowHeight = 24;
        inline juce::Rectangle<int> centreInRow (juce::Rectangle<int> row)
        { return row.withSizeKeepingCentre (row.getWidth(), kHeight); }
        inline constexpr int kInsetX = 1;   // air each chip keeps either side of itself
        // A chip's own width: its word in the font that height gives it, the painter's
        // own 4 px a side, and the same kSnugPad every other control in this window is
        // padded with. Never a fixed constant: "HARMÒNIC" is not "HARMONIC".
        inline int width (const juce::TextButton& b)
        { return textButtonWidth (b.getButtonText(), kHeight, false) + kSnugPad + 2 * kInsetX; }
    }

    // The WIDEST item a combo can show, never the selected one: the box does not resize
    // when the user picks another item, so a slot sized to today's choice draws
    // tomorrow's cut off - and nothing on screen says it was cut.
    inline int snugComboWidth (const juce::ComboBox& c, int h)
    {
        const auto f = comboItemFont (c, h);   // from the TARGET height, not the current one
        int w = 0;
        for (int i = 0; i < c.getNumItems(); ++i)
            w = juce::jmax (w, juce::GlyphArrangement::getStringWidthInt (f, c.getItemText (i)));
        return comboWidthForItem (w, h) + kSnugPad;
    }

    // ...AND THE SAME QUESTION ASKED OF THE SELECTED ITEM ALONE.
    //
    // snugComboWidth above measures the WIDEST item, so the box does not resize under the
    // user's hand as they change modes - that is comfort, and it is the right default.
    // But a box is a control that must be READ, and at a size where the row cannot hold
    // both the widest item and the control beside it, showing what is CURRENTLY selected
    // is the part that carries meaning. Same font, same padding, same rule - one item
    // instead of all of them - so the two numbers can never disagree about anything else.
    inline int snugComboWidthForSelection (const juce::ComboBox& c, int h)
    {
        const auto f = comboItemFont (c, h);
        return comboWidthForItem (juce::GlyphArrangement::getStringWidthInt (f, c.getText()), h)
             + kSnugPad;
    }



    // A VERTICAL fader is never narrower than the value it reads out.
    //
    // Slider::resized() clamps the text box to the slider's own width, so a 46 px box
    // asked for in configFader becomes 34 - and 24 after juce::Label's own {1,5,1,5}
    // border. "120.0 ms" needs 39 there, so the glide fader's read-out was drawn
    // condensed to 70%: not clipped, not out of bounds, just quietly unreadable, which is
    // exactly the failure a bounds check cannot see.
    //
    // Measured from the slider's OWN text ACROSS its range, in the 10 px face
    // GalacticLnF::getLabelFont gives a slider's child label - so renaming a suffix or
    // widening a range moves the column with it, instead of leaving a magic 34 behind.
    //
    // Across the range, not just at the ends: the widest string is not always at an
    // endpoint - one more decimal can cost more than one more digit saves. Measured, the
    // glide fader's widest IS its maximum ("5000.0 ms"), so today this sampling changes
    // no column; it is here so that the next fader whose middle reads wider than its ends
    // does not have to be found on the glass. Stated plainly because it earns nothing
    // right now: removing it and measuring the ends alone leaves the rig at zero.
    // THE BORDER A SLIDER'S VALUE BOX REALLY KEEPS, in ONE place.
    //
    // juce::Label defaults to BorderSize (1, 5, 1, 5) and this function used to add a
    // hard-coded 10 for it. GalacticLnF::createSliderTextBox now sets 1 px a side - the
    // boxes are drawn with no fill and no outline, so 5 was buying nothing and "Centred"
    // needed the room - and the moment it did, this 10 was a SECOND COPY OF THE GEOMETRY
    // AND IT WAS WRONG. It demanded 8 px more width than the box needs, and the sizing
    // sweep in resized() reads it to choose between TextBoxBelow and NoTextBox: any fader
    // whose column sat within those 8 px lost its number ENTIRELY. "ahora no aparece el
    // numero de base bajo el control de base." Both sides read this constant now.
    inline constexpr int kSliderValueBorderX = 1;
    inline int faderValueWidth (const juce::Slider& s)
    {
        const juce::Font f (juce::FontOptions (10.0f));
        auto& sl = const_cast<juce::Slider&> (s);
        float w = 0.0f;
        for (int i = 0; i <= 32; ++i)
            w = juce::jmax (w, juce::GlyphArrangement::getStringWidth (
                                   f, sl.getTextFromValue (sl.proportionOfLengthToValue ((double) i / 32.0))));
        return (int) std::ceil (w) + 2 * kSliderValueBorderX;
    }
    // juce::Label: drawLabel works inside getBorderSize(), in getLabelFont()'s face.
    inline juce::Font labelFont (const juce::Label& l)
    { return l.getLookAndFeel().getLabelFont (const_cast<juce::Label&> (l)); }
    inline int labelWidth (const juce::Label& l)
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (labelFont (l), l.getText()))
             + l.getBorderSize().getLeftAndRight() + 3;
    }
    // ...and, for the ones that are meant to WRAP, the height that text takes at a
    // given box width. A fixed height on a bilingual paragraph is a cut last line.
    inline int labelHeight (const juce::Label& l, int boxWidth)
    {
        juce::AttributedString as;
        as.setText (l.getText());
        as.setFont (labelFont (l));
        as.setJustification (l.getJustificationType());
        as.setWordWrap (juce::AttributedString::byWord);
        juce::TextLayout tl;
        tl.createLayout (as, (float) juce::jmax (1, boxWidth - l.getBorderSize().getLeftAndRight()));
        return (int) std::ceil (tl.getHeight()) + l.getBorderSize().getTopAndBottom() + 2;
    }

    // THE ROOM AND THE FACE A CLOSED COMBO'S ITEM IS REALLY DRAWN IN, read off the child
    // juce::Label the box draws it through rather than reconstructed from a model of it.
    //
    // Two probes ask this question - the fit sweep (walkTextFits) and the layout probe's
    // combo() - and while each carried its own answer they disagreed. The sweep measured
    // comboTextWidth in getComboBoxFont's LIGHTER face and never took off the Label's own
    // {1,5,1,5} border, so it certified 10 px more room than the word gets AND measured
    // the word in a narrower face than GalacticLnF::getLabelFont draws it in. Both misses
    // ran the same way - towards "it fits" - which is how a combo whose last glyph
    // touches its chevron was reported as having ZERO fit problems in three separate
    // scenarios. The layout probe had already been corrected, twice, by hand; this is
    // that correction with one body instead of two.
    //
    // The label's bounds come from positionComboBoxText, which is where the arrow gutter
    // is taken off, so a change to comboArrowGutter moves the painter and BOTH probes in
    // one step and they cannot drift apart again. The fallbacks are for a box that has
    // not been laid out yet (no label): the same numbers, modelled.
    inline const juce::Label* comboItemLabel (const juce::ComboBox& c)
    {
        for (int i = 0; i < c.getNumChildComponents(); ++i)
            if (auto* l = dynamic_cast<const juce::Label*> (c.getChildComponent (i))) return l;
        return nullptr;
    }
    inline juce::Font comboDrawnFont (const juce::ComboBox& c)
    {
        if (auto* l = comboItemLabel (c)) return labelFont (*l);
        return comboItemFont (c, c.getHeight());
    }
    inline int comboDrawnRoom (const juce::ComboBox& c)
    {
        if (auto* l = comboItemLabel (c))
            return l->getBorderSize().subtractedFrom (l->getLocalBounds()).getWidth();
        return comboItemRoom (c.getWidth(), c.getHeight());
    }
    inline int comboDrawnRoomH (const juce::ComboBox& c)
    {
        if (auto* l = comboItemLabel (c))
            return l->getBorderSize().subtractedFrom (l->getLocalBounds()).getHeight();
        return juce::jmax (1, c.getHeight() - 4);
    }

    // A SINGLE-LINE juce::TextEditor's text room, mirroring TextEditor::getMaximumTextWidth
    // (viewport width - leftIndent - the 2 px it keeps for the caret at the right edge),
    // with its border taken off first.
    //
    // Here because NOTHING in this build measured a TextEditor's text at all. A Label
    // condenses and a ComboBox draws what fits, but a TextEditor SCROLLS: when setText
    // leaves the caret at the end, a value too wide for the field is truncated from the
    // LEFT with no ellipsis, and there is no wrong-looking pixel to notice. That is how
    // the reference-pitch field displayed "55653" for 261.6255653 Hz at 690x550 - a
    // number that is not cut, it is a different, perfectly plausible number.
    inline constexpr int kTextEditorCaretSpace = 2;   // TextEditor's own rightEdgeSpace
    inline int textEditorRoom (const juce::TextEditor& te)
    {
        return juce::jmax (1, te.getBorder().subtractedFrom (te.getLocalBounds()).getWidth()
                                - te.getLeftIndent() - kTextEditorCaretSpace);
    }
}

class FractalAdditiveAudioProcessorEditor : public juce::AudioProcessorEditor,
                                            private juce::Timer
{
public:
    explicit FractalAdditiveAudioProcessorEditor (FractalAdditiveAudioProcessor&);
    ~FractalAdditiveAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    // The playground lock's band: over the children, because the controls it crosses
    // out are children and JUCE draws those on top of paint(). See the definition.
    void paintOverChildren (juce::Graphics&) override;
    // A knob too narrow for its number shows no number - see the definition.
    void fitKnobValueBox (juce::Slider& k);
    void layoutPlaygroundBandButton();
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override; // reclaim keyboard focus so play works after a window switch
    void broughtToFront() override;                      // ditto when the host re-activates our window
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;

    // Test hook (RhythmVerify): the macro boxes and every control laid out inside the
    // SPECTRUM panel and Essentials, as name -> bounds. The verifiers build the processor
    // only, so until this existed a layout change could not be checked by ANYTHING except
    // opening the plugin and looking - and a box whose content overflows it, or two boxes
    // that overlap, are exactly what a fixed 1200x650 rearrangement gets wrong.
    // Nothing in the plugin itself calls this.
    struct LayoutProbe
    {
        // engineBox is the ONE shared box both engines live in (the HARMONIC / CUSTOM
        // chips are its title; the selected chip decides whose controls are inside).
        juce::Rectangle<int> spectrum, engineBox, bothEngines,
                             essentials, envelopes, tuning, engineChips,
                             specColumn, // the full-height Spectrum-vs-Tuning box on the right
                             titleBar;   // the strip the preset cluster + playground button live in
        // THE TOP-RIGHT CORNER of every macro panel, as how far INSIDE its own corner the
        // drawn border passes: 0.5 px when that corner is square, 2.5 px for the 9 px
        // radius. Sampled off the very path paint() strokes (see macroFramePath).
        //
        // Measured this way because nothing else in the probe can see it. The wall lines
        // already agree (an arc leaves the border's x exactly where it was, it just stops
        // short of the top), the rects already touch, and every bounds check passes - while
        // on the glass two arcs pinch the shared wall at its top end and it reads as a seam
        // instead of one line.
        std::vector<std::pair<juce::String, float>> panelCorners;
        // The colour the WHOLE SPECTRUM panel is wearing right now (the selected
        // engine's). Probed because "it must not be the neutral steel again, and it
        // must not collide with the panels around it" is a measurable claim about a
        // hue, and nothing else in the build could check it.
        juce::Colour panelColour;
        // The SLOPE (AMP DECAY) fader's thumb colour, and whether it is frozen right now.
        // Probed because that fader is the ONE control in the panel allowed to leave the
        // panel colour, and only in one direction: the inert grey while manual per-partial
        // amplitudes are on. It went MINT after a pencil on/off cycle - a colour this
        // panel stopped using - and no geometric check in the build could see it.
        juce::Colour slopeColour;
        bool         slopeFrozen = false;
        std::vector<std::pair<juce::String, juce::Rectangle<int>>> controls; // "harm.baseN" etc.
        // The SUB-CELLS the boxes are divided into. They are rectangles only - the
        // dividers between them and their inline sub-labels are drawn in paint() from
        // exactly these - so a cell that escaped its box, or two cells that grew into
        // each other, draws a divider across a neighbour's controls and nothing in the
        // build could see it. Named "eng.base", "both.slope", "ess.ref", ...
        std::vector<std::pair<juce::String, juce::Rectangle<int>>> cells;
        // Every PAINTED label: the exact rect paint() draws it into, and how wide the
        // string actually is in the font paint() uses. A painted label has no component
        // to squash, so it fails by CLIPPING - silently, in one language only ("FROZEN -
        // DRAWN BY HAND" clipped at "DRA..."). Probing the measured width is the only way
        // a build can tell.
        struct PaintedLabel
        {
            juce::String name, text;
            juce::Rectangle<int> cell;  // the rect the text is drawn into
            float textWidth = 0.0f;     // measured in the very font paint() sets
            // The INK. "The whole panel is one colour" is a claim about exactly this,
            // and it is invisible to every geometric check in the build: the old
            // mint/magenta scheme of the Fractal-Morph-Range compartment laid out
            // perfectly and simply looked like a different box.
            juce::Colour colour;
            // WHICH WAY THIS ONE FAILS when it does not fit, because the two are not the
            // same defect and only one of them is a cut. paint() draws a `fitted` label
            // with drawFittedText (condensed to 80%, all of it there) and every other one
            // with drawText and useEllipsis = false (the tail simply gone). The rig's
            // structural floor refuses the second kind - a word that is not all there is
            // not a cramped word - so it has to be able to tell them apart. Defaulted, so
            // the componentLabels entries below, which are juce::Labels and condense, keep
            // their five-element construction.
            bool fitted = false;
        };
        std::vector<PaintedLabel> paintedLabels;
        // The same measurement for every juce::Label COMPONENT laid out in those boxes,
        // in its own font. A Label does not clip - it squashes to 70% - so it fails by
        // going unreadably condensed instead of disappearing, which no bounds check can
        // see either. Same struct: cell = the label's bounds, textWidth = what it needs.
        std::vector<PaintedLabel> componentLabels;
        // The read-outs that WRAP. A juce::Label lays its text out in as many lines as its
        // height allows, and modern JUCE will SHRINK THE FONT to make more of them fit - so
        // a sentence that "fits" can be rendering two points smaller than the one beside
        // it, and a single-line width check would reject every wrapping label out of hand.
        // Measured the only way that answers the real question: word-wrap this text, in
        // this font, at this width, and see how tall it comes out.
        struct WrappedLabel
        {
            juce::String name, text;
            juce::Rectangle<int> cell;   // the TEXT area - the Label's own border removed
            float needWidth = 0.0f, needHeight = 0.0f;
        };
        std::vector<WrappedLabel> wrappedLabels;
        // ...and every OTHER sentence the adaptation's read-out can print, measured against
        // that same label's text area with the widest numbers each can hold. The label only
        // ever shows one state; probing the live one would measure whichever state the
        // verifier happened to reach, and the state that clips is always the other one.
        std::vector<WrappedLabel> readoutVariants;
        // Every combo whose ITEM TEXTS are rewritten by applyLanguage(), as
        // name -> selected id. Probed because re-wording a ComboBox is the one operation
        // that can silently DESELECT it (getSelectedId() compares the visible label
        // against the item's text), and a mode box that reads 0 gates its whole panel off.
        std::vector<std::pair<juce::String, int>> comboIds;
        // ...and, for the combos whose WORDS are the feature, whether the selected item
        // really FITS the box it was given. A ComboBox does not clip visibly and does not
        // resize: it just draws as much of the word as the cell holds, so every geometric
        // check in the build passes while the screen reads "Sync Project Te". `avail` is
        // the text area LookAndFeel_V4 leaves after the arrow; `need` is what the word
        // measures in the box's own font. This is the check that bites when a label is
        // renamed to something longer than its slot - the Shepard rate box was.
        struct ComboText
        {
            juce::String name, text;
            float need = 0.0f;
            int   avail = 0;
        };
        std::vector<ComboText> comboTexts;
        // ...and the same measurement for every BUTTON that is sized to its own word.
        // A juce::TextButton does not condense and does not resize: drawFittedText
        // squashes the string to 70% and then clips it, so a button narrowed to fit
        // English shows a Catalan word cut in half with every bounds check still green.
        // Narrowing controls is exactly how that appears, so the six controls of this
        // pass - and every other button in the window - are measured here. `avail` is
        // the room the painter really leaves (fgfit::textButtonRoom / toggleRoom, the
        // fork glyph and the tick box already taken off); `need` is the string's width
        // in the painter's own font.
        std::vector<ComboText> buttonFits;
        // The BUTTONS whose word is the whole feature: the custom view's one button that
        // is "Load" or "Remove" depending on what is loaded, and the tuning bank's add
        // button. A wrong word on the first of those means a green Load that deletes the
        // user's spectrum, and no bounds check in the build could ever see it.
        std::vector<std::pair<juce::String, juce::String>> buttonTexts;
        // THE SHARED WALL, as the x of the LINE each of the three boxes actually draws
        // there - not the x of its rectangle. Every border is a stroke whose centre sits
        // a fixed distance inside the box, so two rects that merely touch draw TWO lines
        // back to back; comparing the rects would call that a shared wall while the
        // screen shows a doubled border. These are what paint() puts on the glass.
        float wallSpectrumRight = 0.0f, wallTuningRight = 0.0f, wallColumnLeft = 0.0f;
        // The Periods field's HOVER, in the language this probe was taken in. The average /
        // worst-case selector and the sentence under it are gone (minimax could not change
        // the result - see adaptRegPeriodsEdit), and the one thing that must not go with them
        // is the answer to "so how ARE those registers combined?". It lives in this tooltip
        // now, and a tooltip is invisible to every other check in this build - including to
        // the bilingual sweep, which restores the language before it returns, so it has to
        // be read HERE or not at all.
        juce::String regPeriodsTip;
        // EVERY DIVIDER LINE the SPECTRUM panel strokes, named, exactly as
        // spectrumDividers() returns them to paint(). A divider is a bare drawLine with
        // no component behind it, so a wall that stops short of the panel floor is
        // invisible to every rect check in the build - which is how the prime column's
        // left wall shipped with a 23 px hole under it.
        std::vector<std::pair<juce::String, juce::Line<float>>> dividers;
        // THE KEYBOARD, as two rectangles that are not the same claim.
        //
        // "el teclado ahora mismo no aparece centrado en el sinte, haz que asi sea"
        //
        // `keyboardBox` is the component's own bounds in the editor. `keysDrawn` is the
        // span the KEYS really occupy inside it, taken from ScaleKeyboard::cellRect -
        // the function paint() fills from. The two differ whenever the tuning has fewer
        // playable keys than the window has cells (a wide period puts only a handful
        // between 40 Hz and 4186 Hz), and that is the case that was off centre: the
        // component filled the editor while its keys hugged the left edge. Checking the
        // component alone would have called that centred.
        juce::Rectangle<int> keyboardBox, keysDrawn;
        int keysInWindow = 0, keysPlayable = 0;   // cells available vs cells with a key
    };
    LayoutProbe layoutProbe() const;
    // THE DIVIDER LINES INSIDE THE SPECTRUM PANEL, from the cell rects. paint() strokes
    // exactly what this returns and layoutProbe() reports exactly what this returns;
    // there is no second place deciding where a wall starts or ends. Same rule as
    // macroFramePath, for the same reason: a probe that rebuilt the lines from its own
    // copy would keep passing after paint() changed them.
    std::vector<std::pair<juce::String, juce::Line<float>>> spectrumDividers() const;
    // THE OUTLINE A MACRO PANEL IS DRAWN WITH, from its box rect - fill and stroke both.
    // paint() strokes exactly what this returns and layoutProbe() measures exactly what
    // this returns; there is no second place deciding which corners are round.
    //
    // That is not tidiness, it is what makes the corner check bite. The probe used to
    // rebuild the path from its own copy of the rule, so sabotaging paint() into rounding
    // the shared corner again left every check passing - the probe was measuring its own
    // assumption. With one function, a hand that squares or rounds the wrong end changes
    // the number the probe reads.
    //
    // The SHARED-WALL panels are SPECTRUM and TUNING: their right border is the same drawn
    // line as the Spectrum-vs-Tuning column's left border (see the wall block in resized()),
    // so that end is square and every corner facing open page keeps its radius.
    juce::Path macroFramePath (const juce::Rectangle<int>& box) const;
    // Test hook (RhythmVerify): probe the layout in the OTHER language. Every painted
    // sub-label is bilingual and Catalan is the longer language nearly everywhere, so a
    // label that fits in English can still clip in Catalan - which is precisely how the
    // "FROZEN - DRAWN BY HAND" clip survived review. Restores the language it found.
    LayoutProbe layoutProbeInLanguage (bool useCatalan);
    // Test hook (RhythmVerify): re-read engineInForce() and redress the panel for it,
    // exactly as the 30 Hz timer does. The verifier has no message loop, so without this
    // the panel would still be wearing the colour it was constructed with and "selecting
    // an engine recolours the whole panel" could not be measured at all.
    void refreshEngineDressing() { applyEngineState(); }
    // Test hook (RhythmVerify): run ONE tick of the panel timer. The verifier has no
    // message loop, so anything that only appears because the timer noticed a change -
    // the Shepard's rise-interval dropdown when the tuning's period stops being an
    // octave, say - is otherwise unreachable, and "it appears when the tuning changes"
    // could not be measured at all. It is the timer's own layout signature that is
    // under test, so calling resized() directly here would test nothing.
    // The label the KEYBOARD paints under a degree - the exact ratio when the scale really
    // holds one, else its cents with a trailing "*" when a nearby just interval exists.
    // Exposed because that rule is a promise to the player ("the hover will name it") and
    // nothing measured it: the asterisk is a single glyph on a 63 px key, which is exactly
    // the kind of thing a screenshot cannot settle and a check can.
    juce::String degreeKeyLabelForTests (int degree) const
    { return keyboard.getDegreeLabeller() ? keyboard.getDegreeLabeller() (degree) : juce::String(); }
    void tickTimerForTests() { timerCallback(); }
    // Test hook (RhythmVerify): re-read "partialManual" and redress the SLOPE fader,
    // exactly as the 30 Hz timer does. The verifier has no message loop, so a pencil
    // on/off cycle is otherwise unreachable - and that cycle is the bug.
    void refreshSlopeFreezeForTests() { refreshSlopeFreeze(); }
    // Test hooks (RhythmVerify run 50): what the TRANSITION box shows is a claim about the
    // SCREEN, so the verifier drives the same call the timer makes and then reads the same
    // three things the user would see. Nothing here re-derives the answer from the
    // parameters - that would be the test asserting its own arithmetic instead of the
    // panel's - it reports what the components are actually holding.
    void applyTransitionModeForTests() { applyTransitionMode(); }
    // 1 = Off, 2 = Morph, 3 = Dry/Wet (the box's own item ids), 0 = nothing selected.
    int  transitionItemShownForTests() const { return transitionBox.getSelectedId(); }
    // Which of the two Time faders is the one on screen, and whether it can be moved.
    // (Off shows the Dry/Wet one, greyed: see applyTransitionMode.)
    bool morphTimeFaderShownForTests() const { return morphTimeFader.isVisible(); }
    bool transitionTimeFaderLiveForTests() const
    { return (morphTimeFader.isVisible() ? morphTimeFader : dryWetTimeFader).isEnabled(); }
    // THE BINAURAL FADER'S USABLE TRAVEL, in pixels, or -1 when the cell was too small
    // and resized() hid the control instead of squashing it.
    //
    // Not its width. Every geometric check in the build was green over a version of this
    // fader whose track was 8 px long under a 24 px star: the bounds were inside the
    // cell, nothing had collapsed to zero, the value box read cleanly - and the thing
    // could not be dragged. The distance between where 0.0 sits and where 1.0 sits is
    // the only number that says whether a fader can be aimed, so that is what this
    // returns and what RhythmVerify holds a floor under.
    int binauralTravelForTests() const
    {
        if (! binauralFader.isVisible()) return -1;
        return (int) std::lround (binauralFader.getPositionOfValue (1.0)
                                - binauralFader.getPositionOfValue (0.0));
    }
    // Test hook (RhythmVerify): click an engine chip the way a hand does. Exists for
    // the one behaviour a probe of engineInForce cannot reach: with NO scale loaded,
    // clicking CUSTOM must still open that view (it is the only place the Load button
    // and the intervals editor live), while the sound honestly stays harmonic.
    void clickEngineChipForTests (int i) { if (engineChip[i & 1].onClick) engineChip[i & 1].onClick(); }
    // Test hooks (RhythmVerify): the custom view's two buttons, through their own
    // callbacks. The Load/Remove one is a SINGLE button with two opposite jobs, and
    // which job it has depends on state the verifier can set - so "the green Load never
    // deletes anything and the red Remove always does" is only checkable from here.
    // INSTRUMENTATION (RhythmVerify): drag a fader the way a hand does - press, move,
    // release - and report what happened around it. Written for "mover controles con
    // notas apretadas sigue siendo buggy, a veces no deja mover el control": the claim
    // is about a DRAG, and a drag is the one thing no probe in this build could see.
    // Everything that could plausibly refuse one is recorded next to the result, so a
    // failure says WHICH of them was true at the time instead of just "it did not move".
    struct DragProbe
    {
        bool   found = false, enabled = false, visible = false;
        double before = 0.0, afterDrag = 0.0, afterSettle = 0.0;
        // The state the drag happened in.
        int          heldNotes = 0;       // computer-keyboard keys this editor is sounding
        juce::String focusOwner;          // the component holding keyboard focus, by class
        bool         editorHasFocus = false;
        int          msSinceFocused = 0;  // the pollComputerKeyboard debounce clock
    };
    // `name`: "slope", "baseN", "fractal", "morphTime", "harmonics".  `dy`: pixels to
    // drag downwards.  `settleTicks`: timer ticks to run afterwards, to catch a value
    // that moves and is then written back.
    DragProbe dragFaderForTests (const juce::String& name, int dy, int settleTicks);

    // TESTS: WHERE A FADER'S VALUE BOX ENDED UP, after every pass that has an opinion.
    //
    // A box in the wrong PLACE is not something any existing check can state. The fit
    // sweep asks whether a control's text fits the room it was given, and a number moved
    // under a horizontal track in a 24 px row is given room - about 9 px of it - and fits.
    // The snapshot rig photographs the result, and a picture is only evidence if somebody
    // looks. This is the fact in a form a build can fail on: the position juce::Slider
    // really holds, plus the height left for the track under it.
    struct FaderBoxProbe { bool found = false; int pos = -1, boxW = 0, boxH = 0, rowH = 0; };
    FaderBoxProbe faderBoxProbeForTests (const juce::String& name) const;
    void clickCustomLoadRemoveForTests() { if (loadTimbreButton.onClick) loadTimbreButton.onClick(); }
    void clickRandomSpectrumForTests()   { if (randomSpectrumBtn.onClick) randomSpectrumBtn.onClick(); }
    // Test hook (RhythmVerify): double-click a row of the chord-search results, which is
    // the ONLY way into the "Tuning info" view - it loads the chord as the scale and
    // flips the list over to that scale's own degrees. Through the real callback, so the
    // view flip and the resized() that follows it are the ones that ship.
    void loadChordResultForTests (int row) { if (chordList.onLoad) chordList.onLoad (row); }
    // TESTS: what the chord-recommendation list ("Tuning info") is SHOWING right now -
    // read off the rows it will paint, not off the worker's output. Every chip is a
    // degree INDEX, so `maxDegree` must always be inside 0..degreesPerPeriod-1 of the
    // scale that is loaded: "los resultados de tuning info aparecen con numeros que no
    // corresponden a los numeros de tunings que no son 12edo, tipo en una pentatonica me
    // dice 7 y 11". Nothing else in the build can see a stale row.
    struct NadShownProbe
    {
        int  rows = 0;
        int  maxDegree = -1;          // the biggest chip number on screen
        int  degreesPerPeriod = 0;    // what the LOADED scale actually has
        bool equalStep = false;       // rows shown as "any +n" offsets rather than degrees
    };
    NadShownProbe nadShownProbeForTests() const;
    // ...and whether that list is ON SCREEN at all. The recompute behind it is gated on
    // exactly this ("nadView.isVisible()"), so a test that reads the rows without
    // reading this cannot tell a list that is right from a list nobody can see.
    bool degreeListShownForTests() const { return nadView.isVisible(); }

    // Test hooks (RhythmVerify): drive the Spectrum-vs-Tuning panel the way a hand does -
    // pick a mode, pick the adaptation's target, press REALTIME - and read back what the
    // panel then says. The wiring these check ("REALTIME follows the notes you PLAY,
    // whatever the target box was left on" and "each mode keeps its own Period") lives
    // entirely inside these callbacks, and the verifier has neither a mouse nor a message
    // loop to reach them with.
    struct PanelProbe
    {
        int  mode = 0;                 // specModeBox
        int  adaptTarget = 0;          // adaptTargetBox (1 = loaded tuning, 2 = held chord)
        bool adaptTargetEnabled = true;
        juce::String period;           // the shared Period field, as typed
    };
    PanelProbe panelProbeForTests() const;
    // Test hook (RhythmVerify): the keyboard's degree-0 keys, with the period number each
    // one prints and the rects and fonts paint() really uses (see ScaleKeyboard). Those
    // numbers are how the register range's ANCHOR is named back to the user ("3:0"), so
    // they have to be legible at the key width the keyboard actually uses, in every tuning -
    // and there is no other way in this build to see a number that drawFittedText silently
    // shrank to fit.
    std::vector<ScaleKeyboard::PeriodTag> keyboardPeriodTagsForTests() const
    { return keyboard.periodTagProbe(); }

    // Test hooks (RhythmVerify) for the NOTE-NAME option. The keyboard is asked what it
    // is really drawing; the editor is asked what it would tell the user. A check that
    // built its own names, or its own reason, would be testing itself.
    struct NoteNameProbe
    {
        bool         optionOn    = false;  // the user's preference, as persisted
        int          edo         = 0;      // equal divisions of the octave, 0 = not an EDO
        int          namedCount  = 0;      // names the editor has cached for this tuning
        bool         showing     = false;  // is the KEYBOARD drawing names right now
        bool         fits        = false;  // ...and would they fit the narrowest key
        float        fontAtMinKey = 0.0f;  // the height they get at kCellW
        juce::String widest;               // the widest name on this keyboard
        juce::String reason;               // why numbers, in the current language ("" = names)
        // The very face the keyboard draws names in. Carried so a check can run the
        // layout for EDOs this tuning is not - the keyboard only ever holds one at a
        // time, and the widest name of them all belongs to one nobody loaded.
        juce::Typeface::Ptr face;
    };
    NoteNameProbe noteNameProbeForTests() const;
    // The same probe, and the period tags, taken in the OTHER language and restoring the
    // one they found - exactly as layoutProbeInLanguage does. The names themselves are
    // language-free, but the REASON the keyboard gives for showing numbers is not, and
    // neither is the interval label the degree band shares its key with.
    NoteNameProbe noteNameProbeInLanguage (bool useCatalan);
    std::vector<ScaleKeyboard::PeriodTag> keyboardPeriodTagsInLanguage (bool useCatalan);
    // Flip the option exactly as the menu item does (preference write included), so a
    // check exercises the real path rather than a private copy of it.
    void setNoteNamesForTests (bool on);
    // The same flip WITHOUT writing the preference, for the snapshot rig. Deliberately not
    // setNoteNamesForTests: the rig runs on the user's own machine, out of the shipped
    // standalone, and a run that photographs this feature must not leave it switched on
    // for them afterwards. It is also what pins every OTHER scenario to the shipped
    // default - otherwise the 82 pictures would differ depending on a setting belonging to
    // whoever took them, which is not a baseline a redesign can be built on.
    void setNoteNamesForSnapshot (bool on);
    // Test hooks (RhythmVerify): the adaptation's register range - the step count the field
    // shows, the anchor it is counted from, the keys it resolves to and the Hz the search
    // is really given. A range that reads one width and searches another is the bug this
    // probe exists to catch, and it now has one more way to happen than it did: the anchor
    // moves with the hands.
    struct RegisterProbe { int anchorKey = 0;
                           double periods = 0.0, fitDown = 0.0, fitUp = 0.0;
                           double loHz = 0.0, hiHz = 0.0; juce::String periodsText, spanText; };
    RegisterProbe registerProbeForTests() const;
    void setAdaptRegisterPeriodsForTests (const juce::String& n);
    // Test hook (RhythmVerify): what the panel would print on the Adapt button, and what
    // one run / one realtime update is predicted to cost with the settings as they stand.
    juce::String adaptButtonTextForTests() const { return adaptGoBtn.getButtonText(); }
    double       adaptCostMsForTests (bool realtime) const { return adaptCostMs (realtime); }
    juce::String adaptReadoutForTests() const { return adaptReadoutText(); }
    void setSpecModeForTests (int mode);
    void setAdaptTargetForTests (int id);
    // The two selectors that ADD rows to the right column (the free chord search's
    // Find/REALTIME pair, and the adaptation's "between lo hi" row). Both exist only in
    // one branch of resized(), so a layout sweep cannot reach them any other way.
    void setTuneSourceForTests (int id);
    void setChordNotesForTests (int n);
    void setAdaptGoalForTests (int id);
    void setPeriodTextForTests (const juce::String& t);
    void pressAdaptRealtimeForTests();

private:
    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;

    // Every inline sub-label the editor PAINTS (BASE / HARMONICS / AMP DECAY / FROZEN /
    // PHASE / ...). Built in ONE place and used by both paint() (which draws them) and
    // layoutProbe() (which measures them), because a painted label has no component to
    // squash and fails only by clipping - so the drawn rect and the measured rect must
    // be the SAME rect or the check is measuring a label that is not on screen.
    struct PaintedSub
    {
        juce::String name, text;
        juce::Rectangle<int> cell;
        juce::Colour col;
        float fontHeight = 11.5f;
        bool  fitted = false;  // drawFittedText (shrinks to fit) vs drawText (clips)
    };
    std::vector<PaintedSub> paintedSubLabels() const;

    void configGroup (juce::GroupComponent&, juce::Colour);
    void configFader (juce::Slider&, const juce::String& paramID,
                      std::unique_ptr<SliderAttachment>&, juce::Colour, const juce::String& tooltip);
    // Every fader configFader touches is watched: whenever an envelope/LFO/host
    // automation is actually driving its parameter right now, the LookAndFeel draws
    // a second marker at that LIVE position (see GalacticLnF's "modLive" property
    // read in drawLinearSlider/drawRotarySlider) alongside the thumb's own base
    // value - so picking e.g. the right sustain point under modulation is visual,
    // not guesswork. Polled on the existing 30 Hz timer; see timerCallback().
    std::vector<juce::Slider*> modWatchSliders;
    // Every fader configFader() built, so their value boxes can be sized to the text they
    // really show - which is only knowable after the suffixes are set. See applyLanguage().
    std::vector<juce::Slider*> configuredFaders;
    // ...AND THE BOX EACH OF THEM WAS BUILT WITH, because the sizing pass must widen a box
    // without MOVING it.
    //
    // configFader gives every fader a 46 px box BELOW the track and then pushes it onto
    // configuredFaders - and eight callers immediately turn their fader horizontal with the
    // value to its RIGHT, in a row 22-24 px tall where a box below has nowhere to go. The
    // sizing pass then wrote TextBoxBelow back over all of them, and nothing re-set it: the
    // Morph and Dry/Wet times, Harmonics, Prime limit and Reps all drew their number under
    // a 9 px sliver of track. The snapshot rig had been saying so for a while without
    // anybody reading it as this - at the 690x550 floor the Dry/Wet value label is what
    // pins the window's minimum HEIGHT, with "-1.0 tall" of room.
    //
    // So the width the CALLER chose is the floor, not a global 46, and the position and
    // height are read live rather than replaced: phaseFader's box is switched between
    // TextBoxRight and NoTextBox by resized() according to how much room its cell has, and
    // a language pass must not undo that decision either.
    struct FaderBoxHome { juce::Slider::TextEntryBoxPosition pos = juce::Slider::TextBoxBelow; int w = 46; };
    std::vector<FaderBoxHome> faderBoxHome;   // parallel to configuredFaders, filled once
    // ESSENTIALS could not fit its sub-label beside its control, so the label is drawn
    // above it instead. Set by resized(), read by paintedSubLabels() - the two must agree.
    bool essStacked = false;
    // The nine prime whitelist ticks could not be given a tick box anyone can aim at, so
    // resized() hid the whole row. Read by paintedSubLabels(), which then paints the tag
    // that says the row is missing and what brings it back - the same "one flag, two
    // places that must agree" arrangement as essStacked above. It is the ONLY thing on
    // screen that says so, which is why it is a member and not a local: a row that
    // vanishes with nothing beside it is exactly the silent hiding this build refuses.
    bool primeTicksHidden = false;
    // TEMPER TO TUNING shares the HARMONICS title row, to the right of the word itself,
    // so paintedSubLabels() has to give that word a SHORTER cell - exactly as the prime
    // title does for its RESIZE tag. Set by resized() to the toggle's width plus its gap
    // (0 when the toggle did not fit and is hidden), read by paintedSubLabels(): the same
    // "one flag, two places that must agree" arrangement as the two flags above. Without
    // it the fit sweep measures HARMONICS against a cell the toggle is sitting in, and
    // the first thing that goes wrong is a window floor measured on a lie.
    int harmTemperW = 0;
    // ...and whether resized() HID it, because a control that vanishes with nothing beside
    // it is the silent hiding this build refuses. Exactly the primeTicksHidden arrangement
    // above: paintedSubLabels() reads this and paints the tag that says the toggle is off
    // screen and what brings it back.
    bool harmTemperHidden = false;
    // ONE COMMIT AT A TIME. The blur handler runs onReturnKey, and onReturnKey ends in
    // refocus() -> grabKeyboardFocus(), which is itself a focus change: if the box still
    // holds focus at that instant the whole handler re-enters, and a spectrum the parser
    // refuses (so the text never matches what the processor holds) would re-enter forever.
    // A commit is not a thing that can be inside itself.
    bool intervalCommitting = false;
    void refreshModWatch();
    void configCombo (juce::ComboBox&, const juce::String& paramID,
                      std::unique_ptr<ComboBoxAttachment>&, juce::Colour, const juce::String& tooltip);
    void makeLabel (juce::Label&, juce::Colour);
    void refocus();
    void reclaimFocusUnlessTyping(); // regrab keyboard focus after a window switch (not while typing)
    // Is any mouse button down anywhere right now? Desktop's own answer, and it is
    // already false again by the time a mouseUp is dispatched - see refocus().
    static bool mouseButtonHeld();
    // THE RULE for taking the keyboard back, as data, so the drag tests can ask it
    // directly instead of inferring it from where the focus happened to land.
public:
    struct FocusReclaimProbe
    {
        bool typing = false;      // a text field owns the letters
        bool mouseHeld = false;   // a press is in progress: a focus change could end it
        bool alreadyOurs = false;
        bool tooSoon = false;     // another attempt inside the debounce window
        bool hostOwnsKeys = false;// a plugin: the pointer crossing us is no reason to take them
        bool wouldGrabIfFollowing = false;  // every test but the host one says yes (the Standalone's answer)
        bool wouldGrab = false;
    };
    FocusReclaimProbe focusReclaimProbeForTests() const { return focusReclaimState(); }
private:
    FocusReclaimProbe focusReclaimState() const;
    void pollComputerKeyboard(); // reconcile held keys against the real key state
    bool playKeysLive() const;   // may the QWERTY mapping ring notes right now?
    // The line under the on-screen keys, for either wrapper. ONE source, read by
    // applyLanguage and by layoutProbe - see its definition for why both.
    juce::String keyboardHelpText (bool standalone) const;
public:
    // TESTS: the two claims the wrapper gate makes - "these keys are not ours here" and
    // "the window says so". Both read the live objects, not a copy.
    bool playKeysLiveForTests() const { return playKeysLive(); }
    bool editorHasFocusForTests() const { return hasKeyboardFocus (true); }
    juce::String keyboardHelpTextForTests (bool standalone) const { return keyboardHelpText (standalone); }
private:
    // Repaint the Custom Engine's interval list with one colour per token (see
    // IntervalTokens.h). Called on every edit and after every programmatic setText.
    void recolourIntervalTokens();
    void setIntervalText (const juce::String& text); // setText + recolour, never one without the other
    // The text the box was last painted from. It is the re-entry guard, and it has to be
    // the TEXT and not a bool: TextEditor::textChanged() POSTS the onTextChange callback
    // instead of calling it, so a scoped flag is already down by the time our own inserts
    // come back as messages - each of which would rebuild the text and post more, which
    // multiplies rather than settles. Comparing the text ends it after one pass.
    juce::String lastColouredIntervals;
    void openScalaChooser();
    void openPartialAmpWindow();   // the pencil in the SLOPE cell
    void openPlaygroundWindow();   // "Open Preset Playground", under the preset selector
    // Build-and-wire WITHOUT showing, split out of the two open functions so the
    // ownership probe below exercises the exact construction a real click runs.
    void ensurePartialWindow();
    void ensurePlaygroundWindow();
    // THE one mechanism both floating windows use to stay above the editor: OS-level
    // ownership (see FloatingWindowOwner.h). Called on every open, not just the first,
    // because a host may re-dock the editor under a different top-level window.
    void attachFloatingWindow (juce::TopLevelWindow&);

public:
    // Test hooks (RhythmVerify): the playground panel is in a SEPARATE desktop window,
    // so nothing the main layout probe measures ever touches it - and the verifier has
    // no mouse to open it with. These lay it out at a given size, run its paint(), and
    // drive the pad the way a drag does, so the component's own code (its layout, its
    // pixel-to-parameter mapping and the gesture pair the host records from) is exercised
    // by the build rather than only by someone opening the plugin and looking.
    struct PlaygroundProbe
    {
        juce::Rectangle<int> panel, pad, padArea, box[4], clear[4], lbl[4];
        // The per-corner EDIT button, its real (localised) label, and whether it is
        // enabled - an Edit button on an empty corner has nothing to edit and must not
        // be clickable. Bounds so the verifier can prove the new button did not shove
        // the corner's name box under its neighbours.
        juce::Rectangle<int> edit[4];
        juce::String editText[4];
        bool editEnabled[4] = { false, false, false, false };
        // The corner names AS DRAWN, so a check can see the "*" a modified corner must
        // carry (and, just as importantly, see it disappear when the corner is saved).
        juce::String cornerName[4];
        // The MORPH/MIX mode pair: bounds and the REAL (localised) label on each, so
        // the verifier can assert the Catalan "MESCLA" fits the button it is drawn in.
        juce::Rectangle<int> mode[2];
        juce::String modeText[2];
        // The MIX voice-budget row (label + control) and the label's real text.
        juce::Rectangle<int> voicesL, voicesC;
        juce::String voicesText;
        juce::String status;
    };
    // Probes in the given language and RESTORES the one it found, exactly as
    // layoutProbeInLanguage does - a probe that leaves the editor in Catalan would make
    // every later check in the run measure the wrong strings.
    PlaygroundProbe playgroundProbeForTests (int w, int h, bool useCatalan);
    // Press, move to the normalised point, release - the whole gesture, through the very
    // handlers a mouse reaches.
    void playgroundDragForTests (float nx, float ny);
    // TESTS: put a preset name on a corner through the corner box's REAL onChange - the
    // one path a user has, and the one that used to drop the processor's refusal on the
    // floor. A name the folder does not hold is the case life produces: the box lists a
    // snapshot of the preset folder and the file can be gone by the time it is picked.
    void playgroundAssignForTests (int corner, const juce::String& name)
    { playgroundPanel.assignForTests (corner, name); }
    // TESTS: the playground window's X button on a corner (its real onClick), the panel's own
    // poll, and what a corner box shows.
    void playgroundClearForTests (int corner)
    { if (playgroundPanel.cornerClear[corner & 3].onClick) playgroundPanel.cornerClear[corner & 3].onClick(); }
    void playgroundRefreshForTests() { playgroundPanel.refresh (false); }
    juce::String playgroundCornerBoxTextForTests (int corner) const { return playgroundPanel.cornerBox[corner & 3].getText(); }
    // The floating windows' OS ownership, read BACK from the OS. The Z-order behaviour
    // itself (clicking the editor no longer buries the floating window) is enforced by
    // Windows from this one recorded relationship, so the honest automated check is
    // "the OS has the editor's top-level root on record as the owner" - the raise
    // behaviour can only be confirmed by eye. The caller must have put the editor on
    // the desktop first (hidden is fine); peers are created hidden, nothing flashes.
    struct FloatingOwnershipProbe { bool windowOnDesktop = false, ownerIsEditorRoot = false; };
    FloatingOwnershipProbe floatingOwnershipProbeForTests (bool playground);

    // TESTS: what the preset drop-down's list REALLY shows for a typed query. Builds the
    // actual panel and types into its actual field, so a check can never pass against a
    // second copy of the filter that the drop-down itself has stopped using.
    static juce::StringArray presetSearchProbeForTests (const juce::StringArray& names,
                                                        const juce::String& typed);
    // TESTS: how many times a RefreshingComboBox whose drop-down is TAKEN OVER (the
    // presets box) can actually be opened. The user's report was "it opens once, I load
    // a preset, and it never opens again" - ComboBox's private menuActive flag was left
    // set forever because no PopupMenu ever finished to clear it. This drives the same
    // public entry point a click does (showPopupIfNotActive) `attempts` times and
    // returns how many of those reached onShowPopup. Anything less than `attempts` is
    // the bug. (showPopup() is dispatched asynchronously; the probe delivers that one
    // callback by hand, since the verifier has no dispatch loop - see the body.)
    static int comboReopenProbeForTests (int attempts);
    // TESTS: how much clear space the CUSTOM RATIOS box really leaves around the numbers
    // typed into it. "adapta la caja de custom ratios para que los numeros no toquen su
    // borde." Two separate ways that went wrong, neither visible to any rect check:
    //   - a TextEditor's clearances are asymmetric (setIndents has no bottom or right
    //     twin, and the default border is {1,1,1,3}), so the bottom row sat 1 px from
    //     the outline;
    //   - this LookAndFeel strokes a TextEditor's outline as a PILL of radius height/2,
    //     which on the 70 px tall ratios box is a 35 px arc biting deep into the first
    //     and last rows - the corners of the text area were OUTSIDE the drawn shape.
    // `clip` is the border inset, i.e. the only region the editor can put ink in at all
    // (the viewport clips there), read back out of JUCE rather than restated here.
    struct RatiosInkProbe
    {
        juce::Rectangle<int> box;   // the editor itself
        juce::Rectangle<int> clip;  // where any glyph can land: nothing outside reaches the glass
        juce::Rectangle<int> ink;   // bounding box of the glyphs currently laid out
        float radius = 0.0f;        // the corner radius the outline is stroked with
        float minGap = 0.0f;        // clear px between the outline's inner edge and `clip`
        bool  clipInsideOutline = true; // all four corners of `clip` inside the drawn shape
        bool  inkInsideClip = true;
    };
    RatiosInkProbe ratiosInkProbeForTests (const juce::String& typed);

    // TESTS: DOES THE CUSTOM ENGINE'S INTERVAL BOX STILL DRIVE THE SPECTRUM?
    //
    // "el custom spectrum no funciona bien ahora, solo funciona al modo random, no
    //  reacciona a los intervalos que se escriben o borran en la caja de texto."
    //
    // Nothing in the build asserted that TYPING into that box changes the sound, which is
    // exactly how it could stop doing so unnoticed. This drives the box the way a user
    // does - one KeyPress per character, backspaces to delete, then Return - so it can
    // see a break anywhere on the path, not just in the handler.
    //
    // The two POSTED callbacks are delivered BY HAND, because there is no message loop in
    // the verifier and, more to the point, because trusting them would test nothing:
    // TextEditor::textChanged() and TextEditor::returnPressed() both go through
    // postCommandMessage(), so `onTextChange` and `onReturnKey` arrive later or never.
    // (This is the same rule presetSearchProbeForTests states for its own filter.)
    struct IntervalBoxProbe
    {
        juce::String boxText;        // what the box holds when Return is pressed
        juce::String timbreSource;   // what the processor believes was typed, afterwards
        bool  visible = false;       // the box is on screen in the custom view...
        bool  enabled = false;       // ...and can be typed into at all
        bool  readOnly = true;
        juce::Rectangle<int> bounds; // where it was laid out
        int   timbreNotes = 0;       // degrees the Custom Engine holds after the Return
        bool  hadFocus = false;      // the box really held the keyboard while typing
    };
    // `tickTimer` runs THIS editor's own 30 Hz timerCallback() after every keystroke,
    // which is what really happens while a human types: a person is nowhere near 30
    // characters a second, so every single key lands between two ticks. That timer
    // re-seeds this very box from the processor, so "does typing survive a tick" is a
    // question only this mode can ask - and the product's build has
    // JUCE_MODAL_LOOPS_PERMITTED=0, so there is no dispatch loop to borrow instead.
    // If the caller has put the editor on a real (off-screen) peer, the keys are injected
    // there and the box holds REAL keyboard focus; otherwise they go straight to it.
    IntervalBoxProbe typeIntervalsProbeForTests (const juce::String& typed, int backspaces,
                                                 bool tickTimer = false);

    // TESTS: TYPED, THEN CLICKED AWAY - never a Return anywhere.
    //
    // The other half of "escribo algo en la caja de texto de intervalos y no se aplica":
    // most people type a list and then reach for a fader, and until now that threw the
    // list away without a word. Returns what the PROCESSOR ended up holding, because the
    // only interesting question is whether the sound followed the typing. Drives the real
    // focusLost path (juce::TextEditor::focusLost is what fires onFocusLost), not the
    // callback by hand: a probe that calls the handler cannot see a handler that is never
    // reached.
    juce::String typeThenBlurForTests (const juce::String& typed);

    // TESTS: WHAT THE NOTICE OVERLAY IS SAYING RIGHT NOW.
    //
    // The overlay is this window's ONE way of telling the user that something failed
    // where no dialog could be raised - a project whose scale no longer reads, a bank
    // slot that would not load. Both messages are parked on the PROCESSOR by code running
    // without an editor, and picked up by the next timer tick; so "the failure is
    // recorded" and "the failure is on screen" are two different claims, and until this
    // probe existed only the first of them was ever checked. The bank-slot message was
    // recorded for a whole commit with nothing anywhere reading it.
    // `queued` is how many notices wait behind the one shown; `fits` is whether the overlay
    // is tall enough for every line of its title and body at its width.
    // `heightPx` is the overlay's height, `needPx` what its whole text needs at its width.
    struct NoticeProbe { bool visible = false; juce::String title, body; int queued = 0; bool fits = true; juce::String more;
                         int heightPx = 0, needPx = 0;
                         // Measured on the PAINTED geometry, not on heightFor (which also sized
                         // the overlay, so asking it again could never disagree with itself):
                         int fullNeedPx = 0;        // what the WHOLE text needs, even while it is folded
                         bool bodyShown = true;     // the body's own layout fits the rectangle paint clips it to
                         bool actionClear = true;   // the action button is inside the notice and below the body
                         juce::Rectangle<int> bounds; };
    NoticeProbe noticeProbeForTests() const
    {
        const int need = noticeOverlay.heightFor (noticeOverlay.getWidth());
        NoticeProbe p { noticeOverlay.isVisible(), noticeOverlay.title, noticeOverlay.body, (int) noticeQueue.size(),
                        ! noticeOverlay.isVisible() || noticeOverlay.getHeight() >= need,
                        noticeOverlay.moreText, noticeOverlay.getHeight(), need };
        p.bounds = noticeOverlay.getBounds();
        p.fullNeedPx = noticeOverlay.heightForFull (noticeOverlay.getWidth());
        if (noticeOverlay.isVisible())
        {
            const auto body = noticeOverlay.bodyArea();
            p.bodyShown = noticeOverlay.bodyLayout (body.getWidth()).getHeight() <= (float) body.getHeight() + 0.5f;
            const auto& ab = noticeOverlay.actionBtn;
            p.actionClear = ! ab.isVisible()
                         || (noticeOverlay.getLocalBounds().contains (ab.getBounds())
                             && body.getBottom() + (noticeOverlay.moreText.isNotEmpty() ? NoticeOverlay::kMoreH : 0) <= ab.getY());
        }
        return p;
    }
    // TESTS: which notice is up (its key), and its action button's text.
    juce::String noticeKeyForTests() const { return noticeOverlay.isVisible() ? noticeKey : juce::String(); }
    // TESTS: the fold - whether this notice can fold, whether it is folded now, the button's
    // word, and the text actually on screen (which must be the start of the whole text).
    bool noticeCanExpandForTests() const { return noticeOverlay.canExpand(); }
    bool noticeExpandedForTests() const  { return noticeOverlay.expanded; }
    juce::String noticeExpandTextForTests() const
    { return noticeOverlay.expandBtn.isVisible() ? noticeOverlay.expandBtn.getButtonText() : juce::String(); }
    juce::String noticeShownBodyForTests() const { return noticeOverlay.shownBody(); }
    void clickNoticeExpandForTests() { noticeOverlay.expandBtn.onClick(); }
    juce::String noticeActionForTests() const
    { return noticeOverlay.isVisible() && noticeOverlay.actionBtn.isVisible() ? noticeOverlay.actionBtn.getButtonText() : juce::String(); }
    void clickNoticeActionForTests() { noticeOverlay.actionBtn.onClick(); }
    // TESTS: the hover / host-activation focus reclaim, called the way mouseEnter calls it.
    void reclaimFocusForTests() { reclaimFocusUnlessTyping(); }
    // TESTS: a mouse press/release in this window just happened (GestureWatch), or none ever did.
    void noteGestureForTests()   { lastGestureMs = juce::Time::getMillisecondCounter(); }
    void forgetGestureForTests() { lastGestureMs = juce::Time::getMillisecondCounter() - 10 * kGestureMs; }
    bool gestureRecentForTests() const { return juce::Time::getMillisecondCounter() - lastGestureMs < kGestureMs; }
    bool openFocusGrabQueuedForTests() const { return openFocusGrabQueued; }
    // TESTS: the main Menu's items as built for a click (label + ticked), and the freeze item's toggle.
    juce::StringArray mainMenuItemsForTests()
    {
        juce::StringArray out;
        auto m = buildMainMenu();
        for (juce::PopupMenu::MenuItemIterator it (m); it.next();)
            out.add (juce::String (it.getItem().itemID) + (it.getItem().isTicked ? " [x] " : " [ ] ") + it.getItem().text);
        return out;
    }
    void toggleWarnNearFreezeForTests() { toggleWarnNearFreeze(); }
    // TESTS: the choice a click on item r makes, through the same function the menu calls.
    // NEVER 5 from a test: it opens the preset folder in Explorer.
    void mainMenuChoiceForTests (int r) { jassert (r != 5); if (r != 5) handleMainMenuChoice (r); }
    // TESTS: one step of the load watch with an EXACT elapsed time (the timer hands it the
    // wall clock), and what the episode has seen.
    void refreshLoadWarningForTests (double dtSeconds) { refreshLoadWarning (dtSeconds); }
    // THE SNAPSHOT RIG: after staging the warning, stop the watch, so the rig's own slow ticks
    // (a busy machine photographing 150 views) cannot start a real episode and change - or,
    // with the option off, withdraw - the notice between the English and Catalan pictures.
    void holdLoadWatchForSnapshot() { loadWatchHeld = true; }
    const fibload::Policy& loadPolicyForTests() const { return loadPolicy; }
    // TESTS: a RESET - hides the notice and forgets anything queued behind it, so a check
    // starts from a clean overlay. NOT what the X does: see clickNoticeCloseForTests.
    void dismissNoticeForTests() { noticeQueue.clear(); noticeKey.clear(); noticeOverlay.setVisible (false); }
    // TESTS: exactly what pressing the overlay's X does - hide it and show the next one.
    void clickNoticeCloseForTests() { noticeOverlay.closeBtn.onClick(); }
    // TESTS: raise / withdraw a KEYED notice through the real functions (title doubles as body).
    void raiseKeyedNoticeForTests (const juce::String& key, const juce::String& title)
    { raiseNotice ([this, title] { noticeOverlay.title = title; noticeOverlay.body = title; }, key); }
    void withdrawNoticeForTests (const juce::String& key) { withdrawNotice (key); }
    // TESTS: the first VISIBLE child laid above a showing notice that OVERLAPS it - its name, or
    // its class - or empty when nothing covers it. "Is it the last child" was the first version,
    // and it failed on children that sit above without covering anything.
    juce::String noticeBuriedByForTests() const
    {
        if (! noticeOverlay.isVisible()) return {};
        const int me = getIndexOfChildComponent (&noticeOverlay);
        for (int i = me + 1; i < getNumChildComponents(); ++i)
            if (auto* c = getChildComponent (i); c != nullptr && c->isVisible()
                                                  && c->getBounds().intersects (noticeOverlay.getBounds()))
                return c->getName().isNotEmpty() ? c->getName() : juce::String (typeid (*c).name());
        return {};
    }
    // TESTS: take the snapshot rig's preset list back off the editor.
    void clearSnapshotPresetPanelForTests() { snapshotPresetPanel.reset(); }
    // TESTS: the Delete button's words on the snapshot rig's copy of the preset list.
    juce::String snapshotPanelDeleteTextForTests() const
    { return snapshotPresetPanel != nullptr ? snapshotPresetPanel->deleteBtn.getButtonText() : juce::String(); }
    // TESTS: what this window would SHOW for a refusal the processor handed it. The two
    // feedback-loop refusals are English sentences built in PluginProcessor.cpp, which has
    // no tr(); this is where they become the window's language, and a check that feeds it
    // the processor's OWN literals is what stops a rewording there from quietly shipping
    // English here.
    juce::String realtimeRefusalTextForTests (const juce::String& raw) const
    { return realtimeRefusalText (raw); }

    // TESTS: WHAT HAPPENS TO AN EDIT THAT HAS NOT BEEN APPLIED YET.
    //
    // Typing into the box does not change the sound until Return; until then the box is
    // holding a draft. The 30 Hz timer also RE-SEEDS this box from the processor. Whether
    // those two can collide - whether a draft can be thrown away without the user doing
    // anything - is what this measures. Types `edit` on the end of whatever the box holds,
    // then does what refocus() does (hands the keyboard back to the editor), then ticks
    // the timer, and reports what is left in the box.
    struct IntervalDraftProbe
    {
        juce::String seeded;    // what the box held before the edit
        juce::String afterEdit; // ...and after typing the edit, before focus moved
        juce::String afterTick; // ...and after the keyboard left and the timer ticked
        bool survived = false;  // afterTick == afterEdit
    };
    IntervalDraftProbe intervalDraftProbeForTests (const juce::String& edit);

    //==========================================================================
    // TESTS: ONE PENCIL DRAG IS ONE Ctrl+Z, HOWEVER LONG THE DRAG TOOK.
    //
    // Driven through the grid's OWN callbacks (onGestureStart / onEdited / onGestureEnd),
    // which is the whole point: those three are what a mouse really calls, so a fix that
    // only worked in a path the test invented would not be measured here. The processor's
    // 60 Hz timer is run `ticks` times between the edit and the mouse-up, which is how a
    // slow drag is reproduced offline - at 60 ticks the ~1 s undo cut used to fire in the
    // middle of the gesture and make it two steps, the second of which leaves manual mode
    // armed with no edits (see FractalAdditiveAudioProcessor::beginUndoGesture).
    struct PencilUndoProbe
    {
        bool armed = false;          // this gesture had to switch manual mode on
        int  cells = 0;              // knobs the grid was showing (0 = nothing was dragged)
        int  stepsBefore = 0;        // undo steps on the stack before the gesture...
        int  stepsAfterFlush = 0;    // ...once the manual-mode flip has been written...
        int  stepsAfterTicks = 0;    // ...after the drag's timer ticks...
        int  stepsAfter  = 0;        // ...and after it: the difference is what Ctrl+Z costs
        bool undid = false;          // the one Ctrl+Z did something
        bool manualAfterUndo = true; // ...and afterwards, is manual STILL armed?
        int  editsAfterUndo = -1;    // ...with how many hand-drawn amplitudes left
    };
    PencilUndoProbe pencilGestureUndoProbeForTests (int cell, float dB, int ticksMidDrag);

    //==========================================================================
    // EVERY DRAWN STRING IN THE WHOLE EDITOR, MEASURED AGAINST THE RECT IT IS DRAWN INTO.
    //
    // "haz un repaso de todos los textos que quedan cortados como 'not in this spectrum'
    //  y arreglalos"
    //
    // The existing checks name their controls one by one, so they can only ever see the
    // text somebody remembered to register - and a truncation is precisely what nobody
    // remembered. This one WALKS THE TREE instead: every visible Label, Button and
    // ComboBox under the editor, whatever panel it lives in, named by its path so the
    // report says where to go. Nothing to keep in step, so a control added tomorrow is
    // measured tomorrow.
    //
    // `need` is what the string measures in the very font that control draws it in;
    // `avail` is the width that control really leaves for text (a Label's border, a
    // TextButton's corner indents, a ToggleButton's tick box, a ComboBox's arrow), each
    // taken from the LookAndFeel that actually paints it. Multi-line labels are
    // word-wrapped instead and report `needHeight` against the box.
    struct TextFit
    {
        juce::String path;      // "rhythmPanel/slot0Lbl", ...
        juce::String kind;      // Label / TextButton / ToggleButton / ComboBox
        juce::String text;
        juce::Rectangle<int> bounds;   // the control, in editor coordinates
        float need = 0.0f,  avail = 0.0f;   // single-line width
        float needH = 0.0f, availH = 0.0f;  // wrapped height, for the ones that wrap
        bool  wraps = false;
        float over() const { return wraps ? needH - availH : need - avail; }
    };
    // Lays the editor out in the given language, walks it, and puts the language back.
    std::vector<TextFit> textFitProbeForTests (bool useCatalan);

    //==========================================================================
    // THE SELF-SNAPSHOT RIG'S HANDS (see source/SnapshotRig.cpp).
    //
    // "no lo hagas a ciegas, usa screenshots"
    //
    // Nobody can redesign a layout they cannot see, and nothing in this build could see
    // it: all four verifiers construct the PROCESSOR and never an editor window. The rig
    // photographs the editor with Component::createComponentSnapshot - no OS compositing,
    // nothing outside our own bounds - and to do that it must first PUT the editor in the
    // state being photographed.
    //
    // Every one of these is a SETTER that STAYS SET, which is exactly what separates them
    // from the ...ForTests probes above: layoutProbeInLanguage() and
    // playgroundProbeForTests() deliberately restore the language they found, because a
    // probe that left the editor in Catalan would make every later check in a verifier run
    // measure the wrong strings. A photograph is the opposite job - the pixels have to BE
    // Catalan when the shutter fires - so the rig needs a language switch that does not
    // undo itself. Nothing in the plugin calls these; the interactive path never reaches
    // them (see fgsnap::runIfRequested).
    void setLanguageForSnapshot (bool useCatalan)
    {
        catalan = useCatalan;
        applyLanguage();                        // Label COMPONENTS' text is written here...
        if (getWidth() > 0) resized();          // ...and some rows are sized from their own text
        // The preset list writes its words when it is BUILT (the real one is built each
        // time it opens, in the language of the moment), so the rig's copy is rebuilt here
        // or the Catalan picture would show the English panel.
        if (snapshotPresetPanel != nullptr) showPresetSearchForSnapshot (snapshotPresetOnCorner);
    }
    // The modulator sub-tab strip (AMP / PITCH / FILTER / CUSTOM) and, inside CUSTOM, which
    // of the six modulators is bound. Through the real callbacks, so the snapshot shows the
    // layout a click produces rather than a second arrangement only the rig can reach.
    void clickEnvTabForSnapshot (int k) { if (envTabB[k & 3].onClick) envTabB[k & 3].onClick(); }
    void setModSelForSnapshot (int id)  { modSelBox.setSelectedId (id, juce::sendNotificationSync); }
    // The Spectrum-to-Rhythm panel, which is an in-editor overlay and so lands in the
    // editor's own snapshot. Same body as rhythmBtn.onClick, minus the focus grab: a
    // snapshot run has no keyboard to take back.
    void setRhythmPanelForSnapshot (bool show)
    {
        if (show)
        {
            rhythmPanel.numRows = processorRef.getTemplatePartialCount();
            rhythmPanel.list.updateContent();
            rhythmPanel.refreshSlots();
            rhythmPanel.refreshLive();
        }
        rhythmScrim.setVisible (show);
        rhythmPanel.setVisible (show);
        if (getWidth() > 0) resized();
        if (show) { rhythmScrim.toFront (false); rhythmPanel.toFront (false); }
        if (noticeOverlay.isVisible()) noticeOverlay.toFront (false);   // a notice that is up stays on top
    }
    // The two FLOATING panels, laid out at the size their own window opens at and handed
    // back for the rig to photograph. They are not children of this editor (they live in
    // their own top-level windows), so the editor's snapshot can never contain them - and
    // no window is created here at all, so nothing is ever shown on a screen.
    juce::Component& floatingPanelForSnapshot (bool playground, int w, int h)
    {
        if (playground)
        {
            playgroundPanel.applyLanguage();
            playgroundPanel.setSize (w, h);
            playgroundPanel.refresh (true);
            playgroundPanel.setVisible (true);
            return playgroundPanel;
        }
        partialPanel.applyLanguage();
        partialPanel.resync();          // never photograph a stale partial list
        partialPanel.setSize (w, h);
        partialPanel.setVisible (true);
        return partialPanel;
    }
private:
    // Recursive half of textFitProbeForTests: every visible Label / Button / ComboBox
    // under `parent`, measured against the room its own LookAndFeel leaves it.
    void walkTextFits (const juce::Component& parent, const juce::String& path,
                       std::vector<TextFit>& out) const;
public:
    // TESTS: what the MAIN window says about whose patch its controls are. The preset
    // field is frozen (and disabled) while a playground corner is being edited; nothing
    // else in the build can see that, and getting it wrong is how a user loads a preset
    // onto somebody's corner.
    struct CornerEditUiProbe
    {
        int  corner = -1;
        juce::String presetText, saveText;
        bool presetEnabled = true, dirtyMarkVisible = false;
    };
    CornerEditUiProbe cornerEditUiProbeForTests()
    {
        refreshCornerEditUi();
        CornerEditUiProbe pr;
        pr.corner           = processorRef.getPlaygroundEditCorner();
        pr.presetText       = presetBox.getText();
        pr.saveText         = savePresetButton.getButtonText();
        pr.presetEnabled    = presetBox.isEnabled();
        pr.dirtyMarkVisible = presetDirtyMark.isVisible();
        return pr;
    }

private:
    void refreshSlopeFreeze();     // shows that SLOPE is inert while manual mode is on
    void openTimbreChooser();
    void openTuningBankChooser();
    void refreshTuningBank(); // sync the bank combo items + selection with the processor
    void loadRecent (int index);
    void applyLanguage();
    void refreshPresetList();
    void savePresetFlow();
    // The presets box's own drop-down: a search field over a live-filtered list (see
    // PresetSearchPanel). Returns false when there is nothing to show, so the ordinary
    // ComboBox popup still gets its turn and an empty folder is not an empty mystery box.
    bool openPresetSearch();
    // Ask for a name and save the playground corner currently in edit mode under it.
    void saveCornerFlow (int corner);
    // Keep the preset field / Save button honest about WHOSE patch the controls are.
    // Called whenever edit mode may have changed, and once per UI tick (automation and
    // the processor itself can end it - a corner cleared, the playground disengaging).
    void refreshCornerEditUi();
    // The SPECTRUM panel while the playground drives the sound: see the comment at the
    // definition. pgDrivingUi is read by paint() (the band) and by the layout.
    void refreshPlaygroundDrivingUi();
    void setSpectrumControlsEnabled (bool on);
    // The band's own way out: one click enters EDIT on the corner the pad is nearest to,
    // so the user does not have to open another window to act on what the band says.
    juce::TextButton pgEditNearestBtn;
    int  pgNearestCorner = 0;
    bool pgDrivingUi = false;
    int  lastPgDrivingSeen = -1;
public:
    // TESTS: is the SPECTRUM panel currently telling the user it cannot be heard? Reads
    // the same flag paint() draws from, so the check cannot pass over a band that is not
    // on the glass.
    bool playgroundBandShownForTests() const { return pgDrivingUi; }
    // TESTS: the band's own way out - is it offered, and does pressing it act?
    bool playgroundBandButtonShownForTests() const
    { return pgEditNearestBtn.isVisible() && pgEditNearestBtn.getWidth() > 0; }
    int  playgroundBandButtonCornerForTests() const { return pgNearestCorner; }
    void clickPlaygroundBandButtonForTests() { if (pgEditNearestBtn.onClick) pgEditNearestBtn.onClick(); }
private:
    void updateWarning (int templatePartials);
    // The longest sentence in the plugin, in one place so the probe can measure it
    // without the dissonance search that raises it. See the definition.
    juce::String fewerDegreesBody (int requested, int found) const;
    void runDissonanceTune();
    void saveDissonanceScl();
    // BOTH sides are decoded as UTF-8, because both sides ARE UTF-8.
    //
    // This used to be `catalan ? String::fromUTF8 (ca) : String (en)`, and
    // juce::String(const char*) reads each BYTE as a code point (CharPointer_ASCII). The
    // Catalan half was therefore safe and the English half was not, so any English string
    // carrying a non-ASCII character shipped as mojibake: "Fibló" inside the Periods
    // tooltip drew as "FiblÃ³", and the bend box's "±48 st (MPE)" - which skipped tr()
    // altogether precisely BECAUSE it is the same in both languages, and so got no
    // protection at all - drew as "Â±48 st (MPE)" in all 42 main-editor snapshots.
    // The whole file is compiled with /utf-8 (see CMakeLists.txt), so a literal is UTF-8
    // bytes whichever argument it is passed as; there was never a reason to treat the two
    // differently. Note that this is the ONLY decoder: juce::String::operator<< (const
    // char*) has the same Latin-1 behaviour, so a non-ASCII fragment streamed into a
    // String still needs String::fromUTF8 of its own.
    juce::String tr (const char* en, const char* ca) const
    { return juce::String::fromUTF8 (catalan ? ca : en); }

    // A REFUSAL THE PROCESSOR WROTE, PUT INTO THE READER'S LANGUAGE.
    //
    // The processor cannot call tr() - tr is an editor member and every parser lives on
    // the other side - so its `err` strings arrive in English and used to be shown that
    // way, under a title this window had carefully translated. This looks each one up in
    // fgerr::table on the way to the screen.
    //
    // A message that is NOT in the table falls through UNCHANGED, which is exactly today's
    // behaviour: the thirty composed messages ("Key " + n, "File declares " + count) have
    // no fixed literal to match and need the typed-code design the audit describes. So
    // this can never make anything worse, and it stops cleanly wherever it has got to.
    // Compared as UTF-8 against the table's English side, so the one accented message in
    // it ("...not a Fibló preset") matches like any other.
    juce::String trErr (const juce::String& fromProcessor) const
    {
        if (! catalan || fromProcessor.isEmpty()) return fromProcessor;
        for (const auto& p : fgerr::table)
            if (fromProcessor == juce::String::fromUTF8 (p.en))
                return juce::String::fromUTF8 (p.ca);

        // ...THEN THE ONES THAT CARRY A VALUE. The English template's literal chunks are
        // consumed in order and whatever sits between them is a value; if every chunk is
        // found, the Catalan template is rebuilt with those values put back. The chunks
        // must be consumed IN ORDER and the first must sit at the very start, or a short
        // stub like `"Bad ratio: \""` would match halfway through an unrelated sentence.
        for (const auto& p : fgerr::patterns)
        {
            const auto en = juce::StringArray::fromTokens (juce::String::fromUTF8 (p.en), "%", "");
            juce::StringArray vals;
            int at = 0; bool ok = true;
            for (int i = 0; i < en.size() && ok; ++i)
            {
                if (en[i].isEmpty()) continue;                 // a value at either end
                const int f = fromProcessor.indexOf (at, en[i]);
                if (f < 0 || (i == 0 && f != 0)) { ok = false; break; }
                if (i > 0) vals.add (fromProcessor.substring (at, f));
                at = f + en[i].length();
            }
            // A trailing value (the template ends with '%') is whatever is left over.
            if (ok && juce::String::fromUTF8 (p.en).endsWithChar ('%')) vals.add (fromProcessor.substring (at));
            else if (ok && at != fromProcessor.length())        ok = false;
            if (! ok) continue;

            const auto ca = juce::StringArray::fromTokens (juce::String::fromUTF8 (p.ca), "%", "");
            juce::String out;
            for (int i = 0; i < ca.size(); ++i)
            {
                out += ca[i];
                if (i < ca.size() - 1 && i < vals.size()) out += vals[i];
            }
            return out;
        }

        // ...AND "<name> \xe2\x80\x94 <sentence>". The processor names WHAT failed in front of the
        // parser's own sentence (a scale, a bank file), and neither lookup above can match a
        // string with a user's file name glued to its start - so the scale notice showed a
        // Catalan body ending in an English sentence the table has translated all along
        // (seen in rig scenario 57). The name is kept exactly as written; only the sentence
        // after the LAST dash is looked up, and only a sentence that actually translates
        // replaces anything.
        {
            const auto dash = juce::String::fromUTF8 (" \xe2\x80\x94 ");
            const int at = fromProcessor.lastIndexOf (dash);
            if (at > 0)
            {
                const auto tail = fromProcessor.substring (at + dash.length());
                const auto translated = trErr (tail);
                if (translated != tail)
                    return fromProcessor.substring (0, at + dash.length()) + translated;
            }
        }
        return fromProcessor;
    }
    void timerCallback() override;

    // ComboBox that refreshes its item list just before its popup opens (so the
    // preset list always reflects the folder contents).
    struct RefreshingComboBox : juce::ComboBox
    {
        std::function<void()> onBeforePopup;
        // Optional: take the drop-down over entirely (the presets box does - it opens a
        // SEARCHABLE list instead of a plain menu). Returning true means "handled".
        std::function<bool()> onShowPopup;
        void showPopup() override
        {
            if (onBeforePopup) onBeforePopup();
            if (onShowPopup && onShowPopup())
            {
                // THE BOX OPENED EXACTLY ONCE PER EDITOR, THEN DIED FOR GOOD.
                //
                // ComboBox::mouseDown -> showPopupIfNotActive() sets the private
                // menuActive flag and THEN calls showPopup() asynchronously. The only
                // thing in JUCE that ever clears menuActive again is hidePopup(), and
                // the only caller of that is comboBoxPopupMenuFinishedCallback - the
                // modal callback of the PopupMenu that juce::ComboBox::showPopup()
                // launches. When we take the drop-down over there IS no PopupMenu, so
                // returning here left menuActive stuck true: every later mouseDown,
                // mouseUp, Return key and scroll-wheel tick was swallowed by the
                // `if (! menuActive)` guard and the presets box never opened again.
                //
                // hidePopup() is public and does exactly the one thing needed: clear
                // the flag (plus PopupMenu::dismissAllActiveMenus(), which walks only
                // PopupMenu's own MenuWindow list and so cannot touch the CallOutBox
                // onShowPopup just launched).
                hidePopup();
                return;
            }
            juce::ComboBox::showPopup();
        }
    };

    //==========================================================================
    // CASE- AND DIACRITIC-INSENSITIVE FOLDING, for the preset search box.
    //
    // Preset names are the user's own words in the user's own language, and this is a
    // bilingual product: "Can\xc3\xa7\xc3\xb3 llunyana", "Melod\xc3\xada", "R\xc3\xa0dio"
    // are exactly the names it invites. A search that only matched byte-for-byte would
    // be a trap for those names specifically - you would have to know which accent you
    // typed a year ago to find your own preset. juce::String has no folding of its own,
    // so the Latin letters this project's two languages (and their neighbours) use are
    // mapped here BY CODE POINT - never by a literal accented character, which would
    // depend on how this file happens to be encoded.
    //
    // Anything not in the table falls through unchanged, so a Cyrillic or CJK name still
    // matches itself exactly. U+00B7 (Catalan's l\xc2\xb7l) folds to NOTHING, so
    // "col\xc2\xb7legi" is found by typing "collegi".
    // ORDER MATTERS: the accent fold happens FIRST, on the raw code point, and covers
    // BOTH cases of every letter. Lowercasing first would not work - JUCE's toLowerCase
    // goes through towlower, which is locale-dependent and in the "C" locale a plugin
    // starts in leaves U+00C7 ('\xc3\x87') exactly where it was. Folding after that would
    // silently miss every CAPITALISED accented name, which is most of the names people
    // actually give presets. Only once nothing matched does toLowerCase get its turn, on
    // ASCII, where it is reliable.
    static juce::juce_wchar foldSearchChar (juce::juce_wchar cIn)
    {
        const int  c  = (int) cIn;
        const auto in = [c] (int lo, int hi) { return c >= lo && c <= hi; };
        if (c == 0xb7) return 0;   // U+00B7, Catalan's l-geminada: punctuation, not a letter
        if (in (0xc0, 0xc5) || in (0xe0, 0xe5) || in (0x100, 0x105))            return 'a';
        if (c == 0xc7 || c == 0xe7 || in (0x106, 0x10d))                        return 'c';
        if (in (0x10e, 0x111))                                                  return 'd';
        if (in (0xc8, 0xcb) || in (0xe8, 0xeb) || in (0x112, 0x11b))            return 'e';
        if (in (0x11c, 0x123))                                                  return 'g';
        if (in (0x124, 0x127))                                                  return 'h';
        if (in (0xcc, 0xcf) || in (0xec, 0xef) || in (0x128, 0x131))            return 'i';
        // 0x13F/0x140 are Catalan's PRECOMPOSED L-with-middle-dot, so both spellings of
        // "col-legi" - the two-character one and the one-character one - fold to "collegi".
        if (in (0x139, 0x142))                                                  return 'l';
        if (c == 0xd1 || c == 0xf1 || in (0x143, 0x148))                        return 'n';
        if (in (0xd2, 0xd6) || c == 0xd8 || in (0xf2, 0xf6) || c == 0xf8
            || in (0x14c, 0x151))                                               return 'o';
        if (in (0x154, 0x159))                                                  return 'r';
        if (c == 0xdf || in (0x15a, 0x161))                                     return 's';
        if (in (0x162, 0x167))                                                  return 't';
        if (in (0xd9, 0xdc) || in (0xf9, 0xfc) || in (0x168, 0x173))            return 'u';
        if (c == 0xdd || c == 0xfd || c == 0xff || in (0x176, 0x178))           return 'y';
        if (in (0x179, 0x17e))                                                  return 'z';
        return juce::CharacterFunctions::toLowerCase (cIn);
    }
    static juce::String foldForSearch (const juce::String& s)
    {
        juce::String out;
        out.preallocateBytes ((size_t) s.getNumBytesAsUTF8() + 1);
        for (auto p = s.getCharPointer(); ! p.isEmpty();)
            if (const auto f = foldSearchChar (p.getAndAdvance()); f != 0)
                out += juce::String::charToString (f);
        return out;
    }

    //==========================================================================
    // THE PRESET DROP-DOWN, WITH A SEARCH BOX ("haz que el desplegable de presets tenga
    // un buscador"). A plain ComboBox popup is a PopupMenu, and a PopupMenu owns the
    // keyboard for its own navigation - a text field inside one cannot reliably be typed
    // into. So the presets box opens THIS instead: a search field over a live-filtered
    // list, in a CallOutBox (which handles click-outside and Escape by itself).
    //
    // The one thing it must not break is PLAYING. The editor's computer-keyboard mapping
    // is disabled whenever a juce::TextEditor holds focus (see pollComputerKeyboard), so
    // letters typed here can never fire notes - and the instant this closes, focus goes
    // back to the editor and the same letters play again. That is why the search field is
    // a real TextEditor and not a hand-rolled key handler.
    //
    // DELETING ("haz que sean borrables con boton izquierdo y clic en un boton de delete
    // que se abra al dar ese clic"). A LEFT CLICK on a row loads that preset AND LEAVES
    // THE LIST OPEN, with a Delete button under it acting on the row just clicked - so a
    // long list can be auditioned and thinned out without reopening it for every preset.
    // Double-click or Enter is the old gesture: load and close. The strip under the button
    // always says what the button will do, to which preset, and where the file goes;
    // after a delete it says what happened, and a refusal says why. That strip is a
    // wrapped TextLayout the panel grows to fit, never a Label that could squash or
    // ellipsise the one sentence that matters.
    struct PresetSearchPanel : juce::Component,
                               juce::ListBoxModel
    {
        juce::TextEditor search;
        juce::ListBox    list { "presets", this };
        juce::TextButton deleteBtn;
        juce::StringArray all, shown;
        juce::String      current;
        // The preset the Delete button acts on: the one last CLICKED, whatever the
        // filter or the scroll position has done since. Empty = no button.
        juce::String      armed;
        juce::String      status;           // the last delete's outcome; empty = none
        bool              statusIsError = false;
        int               listHeight = 60;
        std::function<void (const juce::String&)> onPick;
        std::function<void()> onDismissed;
        // Asked after every pick, so the gold "this is loaded" colour follows what the
        // processor actually LOADED - a preset that failed to load must not look loaded.
        std::function<juce::String()> loadedName;
        struct DeleteOutcome { bool removeFromList = false, isError = true; juce::String message; };
        std::function<DeleteOutcome (const juce::String&)> onDelete;
        // The sentences (the panel has no tr): what an armed button will do, and the
        // hint shown before anything is clicked.
        std::function<juce::String (const juce::String&)> armedText;
        juce::String hintText;

        PresetSearchPanel (juce::StringArray items, juce::String cur, juce::String placeholder)
            : all (std::move (items)), current (std::move (cur))
        {
            deleteBtn.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xffa83a3a));
            deleteBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            // Never focusable: Enter or Space typed into the search field must not be able
            // to land on this button.
            deleteBtn.setWantsKeyboardFocus (false);
            deleteBtn.setMouseClickGrabsKeyboardFocus (false);
            deleteBtn.onClick = [this] { deleteArmed(); };
            addChildComponent (deleteBtn);

            search.setTextToShowWhenEmpty (placeholder, juce::Colour (0xff6a7285));
            search.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff141a24));
            search.setColour (juce::TextEditor::outlineColourId,    juce::Colour (0xff55557a));
            search.setColour (juce::TextEditor::textColourId,       juce::Colours::white);
            search.setFont (juce::Font (juce::FontOptions (14.0f)));
            search.onTextChange = [this] { refilter(); };
            // Enter takes the highlighted row (the first match when nothing is selected),
            // so a name can be found and loaded without the mouse ever moving.
            search.onReturnKey  = [this] { pick (juce::jmax (0, list.getSelectedRow())); };
            search.onEscapeKey  = [this] { dismiss(); };
            addAndMakeVisible (search);

            list.setRowHeight (20);
            list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff0e1118));
            list.setOutlineThickness (0);
            // THE KEYBOARD STAYS IN THE SEARCH FIELD. A click on a row used to hand it to the
            // ListBox, and with the list now staying open that meant typed letters stopped
            // filtering - and in the Standalone, where no text field held focus any more, they
            // PLAYED NOTES (night review). Rows are clicked, never typed into.
            list.setWantsKeyboardFocus (false);
            addAndMakeVisible (list);
            refilter();
        }
        ~PresetSearchPanel() override { if (onDismissed) onDismissed(); }
        // Load for a click that keeps the list open. Returns what went wrong with that load,
        // in the user's language (empty when nothing did) - see makePresetSearchPanel.
        std::function<juce::String (const juce::String&)> onAudition;

        void refilter()
        {
            const auto q = foldForSearch (search.getText().trim());
            shown.clear();
            for (const auto& n : all)
                if (q.isEmpty() || foldForSearch (n).contains (q))
                    shown.add (n);
            list.updateContent();
            // Select the loaded preset when it is still on screen, otherwise the first
            // match - so Enter always has an obvious meaning and never nothing.
            const int cur = shown.indexOf (current);
            list.selectRow (cur >= 0 ? cur : (shown.isEmpty() ? -1 : 0), true, true);
            list.repaint();
        }
        // LOAD AND CLOSE: double-click, Enter.
        void pick (int row)
        {
            if (! juce::isPositiveAndBelow (row, shown.size())) return;
            const auto name = shown[row];
            dismiss();
            if (onPick) onPick (name);
        }
        // LOAD AND STAY: a single click. Arms Delete on that row.
        void audition (int row)
        {
            if (! juce::isPositiveAndBelow (row, shown.size())) return;
            const auto name = shown[row];
            // A click has already selected this row; saying so here keeps every other way
            // in (the snapshot rig, a future keyboard gesture) showing the same highlight.
            list.selectRow (row, true, true);
            juce::String problems;
            if (onAudition)  problems = onAudition (name);
            else if (onPick) onPick (name);
            if (loadedName) current = loadedName();
            // Armed even when the load failed: a preset that will not load is exactly the one
            // somebody wants to delete - and the strip says it did not load, so nothing claims
            // it did.
            armed = name;
            deleteBtn.setVisible (true);
            setStatus (problems, problems.isNotEmpty());
            list.repaint();
            // The editor's own refocus() runs inside that load; give the keyboard back here.
            search.grabKeyboardFocus();
        }
        void deleteArmed()
        {
            if (armed.isEmpty() || ! onDelete) return;
            const auto name = armed;
            const auto out  = onDelete (name);
            if (out.removeFromList)
            {
                all.removeString (name);   // exact spelling: it came out of this very list
                armed.clear();
                deleteBtn.setVisible (false);
            }
            if (loadedName) current = loadedName();
            refilter();
            setStatus (out.message, out.isError);
        }
        void dismiss()
        {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
            else                                                            setVisible (false);
        }

        // The strip's sentence: an outcome if there is one, else what the armed button
        // will do, else how to use the list.
        juce::String statusText() const
        {
            if (status.isNotEmpty())                return status;
            if (armed.isNotEmpty() && armedText)    return armedText (armed);
            return hintText;
        }
        juce::TextLayout statusLayout (int width) const
        {
            juce::AttributedString as;
            as.setWordWrap (juce::AttributedString::byWord);
            as.append (statusText(), juce::Font (juce::FontOptions (12.0f)),
                       status.isEmpty() ? juce::Colour (0xffb8c0d0)
                                        : (statusIsError ? juce::Colour (0xffffa05c)
                                                         : juce::Colour (0xff8fd18f)));
            juce::TextLayout tl;
            tl.createLayout (as, (float) juce::jmax (40, width));
            return tl;
        }
        void setStatus (const juce::String& s, bool isError)
        {
            status = s;
            statusIsError = isError;
            fitHeight();
            repaint();
        }
        // search 22 + list + button 24 + the WHOLE sentence, with 4 px between them.
        void fitHeight()
        {
            if (getWidth() <= 0) return;
            const int textH = (int) std::ceil (statusLayout (getWidth() - 8).getHeight());
            setSize (getWidth(), 4 + 22 + 4 + listHeight + 4 + 24 + 4 + textH + 6);
        }
        void setWidthAndListHeight (int width, int listH)
        {
            listHeight = listH;
            setSize (width, 100);
            fitHeight();
        }

        int getNumRows() override { return shown.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (! juce::isPositiveAndBelow (row, shown.size())) return;
            if (selected) { g.setColour (juce::Colour (0xff2d6da8)); g.fillRect (0, 0, w, h); }
            g.setColour (shown[row] == current ? juce::Colour (0xffffc95c) : juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (13.0f)));
            g.drawText (shown[row], 6, 0, w - 10, h, juce::Justification::centredLeft, true);
        }
        // The panel is sized to the longest name (see makePresetSearchPanel); a name
        // longer than the whole window can still be cut, and ONLY THEN does the row get a
        // tooltip, carrying the whole of it. A tooltip repeating a name already fully on
        // screen is noise - and the snapshot rig's language gate rightly reads a preset
        // name as an untranslated string.
        juce::String getTooltipForRow (int row) override
        {
            if (! juce::isPositiveAndBelow (row, shown.size())) return {};
            const auto w = juce::GlyphArrangement::getStringWidth (juce::Font (juce::FontOptions (13.0f)), shown[row]);
            return w > (float) (list.getVisibleRowWidth() - 10) ? shown[row] : juce::String();
        }
        void listBoxItemClicked (int row, const juce::MouseEvent&) override       { audition (row); }
        void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { pick (row); }
        void returnKeyPressed (int row) override { pick (row); }

        juce::Rectangle<int> statusArea;
        void resized() override
        {
            auto r = getLocalBounds().reduced (4);
            search.setBounds (r.removeFromTop (22));
            r.removeFromTop (4);
            list.setBounds (r.removeFromTop (listHeight));
            r.removeFromTop (4);
            auto bar = r.removeFromTop (24);
            deleteBtn.setBounds (bar.removeFromLeft (juce::jmin (bar.getWidth(), 110)));
            r.removeFromTop (4);
            statusArea = r;
        }
        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff0e1118));
            statusLayout (statusArea.getWidth()).draw (g, statusArea.toFloat());
        }
    };
    // The panel with every callback wired to this editor - the ONE builder, so the check
    // that clicks it drives exactly what the drop-down opens.
    std::unique_ptr<PresetSearchPanel> makePresetSearchPanel (const juce::StringArray& names);
    juce::String recycleBinName() const;
public:
    // TESTS: the wired panel, not in a CallOutBox (a CallOutBox needs a message loop to
    // launch), over the current preset folder. dismiss() then hides it instead.
    std::unique_ptr<PresetSearchPanel> presetSearchPanelForTests()
    { return makePresetSearchPanel (processorRef.getPresetNames()); }
    // TESTS: what the preset combo's field says right now.
    juce::String presetBoxTextForTests() const { return presetBox.getText(); }
    // TESTS: the items the preset combo itself holds right now.
    juce::StringArray presetBoxItemsForTests() const
    {
        juce::StringArray out;
        for (int i = 0; i < presetBox.getNumItems(); ++i) out.add (presetBox.getItemText (i));
        return out;
    }
    // SNAPSHOT: the wired preset list laid over the editor where the CallOutBox puts it
    // (just under the preset box), with its second row CLICKED - so Delete is armed and
    // the strip names the preset - and, with `refuse`, that preset put on playground
    // corner A and Delete pressed, so the strip carries the refusal. The drop-down itself
    // is a CallOutBox, which needs a message loop to launch; this is the same panel, built
    // by the same makePresetSearchPanel, so the picture is of what a click opens.
    void showPresetSearchForSnapshot (bool onCorner)
    {
        snapshotPresetOnCorner = onCorner;
        snapshotPresetPanel = makePresetSearchPanel (processorRef.getPresetNames());
        auto& pnl = *snapshotPresetPanel;
        pnl.onDismissed = nullptr;   // it dies with the editor: no refocus into a window mid-destruction
        if (onCorner && snapshotDeletedName.isNotEmpty())
        {
            // A LANGUAGE REBUILD OF A PICTURE THAT ALREADY DELETED: the outcome is re-said in the
            // new language, never re-done - pressing Delete again would take ANOTHER preset.
            pnl.setStatus (presetDeletedMessage (snapshotDeletedName, lastDeleteWentToBin, lastDeleteCornerMask), false);
        }
        else
        {
            if (pnl.shown.size() > 0)
                pnl.audition (juce::jmin (1, pnl.shown.size() - 1));
            if (onCorner && pnl.armed.isNotEmpty())
            {
                // The preset on corner A, then Delete: the file goes, the corner keeps its copy.
                juce::String err;
                processorRef.assignPlaygroundCorner (0, pnl.armed, err);
                snapshotDeletedName = pnl.armed;
                pnl.deleteArmed();
            }
        }
        addAndMakeVisible (pnl);
        const auto under = getLocalArea (&presetBox, presetBox.getLocalBounds());
        pnl.setTopLeftPosition (under.getX(), under.getBottom() + 12);
        pnl.toFront (false);
    }
private:
    std::unique_ptr<PresetSearchPanel> snapshotPresetPanel;   // only ever set by the snapshot rig
    bool snapshotPresetOnCorner = false;
    juce::String snapshotDeletedName;                         // the rig's one real delete, re-said on a language switch
    // The last delete's outcome, for re-saying it (see presetDeletedMessage).
    bool lastDeleteWentToBin  = false;
    int  lastDeleteCornerMask = 0;
public:
    // The sentence for a preset that was deleted: where the file went, and - when it was on
    // playground corners - that EVERY one of them (bit i = corner i) keeps its own copy. One
    // builder, used by the list's Delete and by the rig's language rebuild, so the two can
    // never say different things.
    juce::String presetDeletedMessage (const juce::String& name, bool wentToBin, int keptCornerMask) const;
private:

    // TextEditor with a focus hook. It used to stretch to the window edge while
    // typing (the intervals box was one 24 px line); now that box is a multi-line
    // area filling most of the shared engine box, so the hook only resets the view
    // to the text's start on blur - but the hook itself stays: without it a long
    // list was left scrolled to its tail and read as if it began mid-way.
    struct ExpandingEditor : juce::TextEditor
    {
        std::function<void (bool)> onFocusChanged;
        void focusGained (FocusChangeType t) override { juce::TextEditor::focusGained (t); if (onFocusChanged) onFocusChanged (true); }
        void focusLost   (FocusChangeType t) override { juce::TextEditor::focusLost (t);   if (onFocusChanged) onFocusChanged (false); }
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

    // Continuous fader with magnetic integer detents (Ctrl = free fine drag) and
    // tick marks painted at each whole level. Used by the fractal-level fader.
    struct MagneticSlider : SteppedSlider
    {
        // How close to a whole value the drag must come before it snaps onto it.
        // Per-slider because the axes differ in how much room they have: a 0..3
        // axis (Harmonics) needs a NARROWER window than a 1..4 one, or most of the
        // blend between the named points is simply unreachable.
        double snapWindow = 0.12;
        double snapValue (double v, juce::Slider::DragMode dm) override
        {
            if (dm == juce::Slider::notDragging) return v;
            if (juce::ModifierKeys::currentModifiers.isCtrlDown()) return v;
            const double n = std::round (v);
            return (std::abs (v - n) < snapWindow && n >= getMinimum() && n <= getMaximum()) ? n : v;
        }
        void paint (juce::Graphics& g) override
        {
            juce::Slider::paint (g);
            const auto r = getLookAndFeel().getSliderLayout (*this).sliderBounds.toFloat();
            if (r.isEmpty()) return;
            g.setColour (findColour (juce::Slider::trackColourId).withAlpha (0.9f));
            const int lo = (int) std::ceil (getMinimum()), hi = (int) std::floor (getMaximum());
            const bool vertical = isVertical();
            for (int i = lo; i <= hi; ++i)
            {
                const float p = (float) valueToProportionOfLength ((double) i);
                if (vertical) // proportion 0 = bottom of the track
                {
                    const float y = r.getBottom() - p * r.getHeight();
                    g.fillRect (r.getX(), y - 0.5f, 4.0f, 1.0f);
                    g.fillRect (r.getRight() - 4.0f, y - 0.5f, 4.0f, 1.0f);
                }
                else
                {
                    const float x = r.getX() + p * r.getWidth();
                    g.fillRect (x - 0.5f, r.getY(), 1.0f, 4.0f);
                    g.fillRect (x - 0.5f, r.getBottom() - 4.0f, 1.0f, 4.0f);
                }
            }
        }
    };

    // Live list of the ACTIVE scale's degrees: colour chip + interval label +
    // Sethares dissonance (green->yellow->red). Fixed row height so it can live in
    // a Viewport and scroll when a scale has more steps than fit. Rows carry their
    // sort keys (step, ratio, height) so the header can re-order them.
    struct DissDegreeList : juce::Component, juce::TooltipClient
    {
        struct Row { int step; double ratio, height; juce::String num, label, value;
                     juce::Colour chip, valueCol; bool isPeriod; };
        std::vector<Row> rows;
        juce::String tip;
        // WHY THE LAST ROW IS MISSING. This list always ended on the PERIOD - degree 0
        // one period up, in a red box - and for a tuning that states no ascending period
        // that row was degree 0 times periodRatio()'s 2.0 display fallback: an octave the
        // user's file never contained, scored and coloured like a fact. The row is gone
        // in that case (see computeActiveScaleDissonance) and this line says so, because
        // a list that is quietly one row shorter is the same defect one layer along.
        juce::String noteText;
        static constexpr int kRowH = 19;
        // Hovering a row SOUNDS that dyad (0 + this degree); only one sounds at a time.
        std::function<void (int)> onHover;
        int hoverRow = -1;
        void mouseMove (const juce::MouseEvent& e) override
        {
            const int r0 = e.y / kRowH;
            const int r = juce::isPositiveAndBelow (r0, (int) rows.size()) ? r0 : -1;
            if (r != hoverRow) { hoverRow = r; if (onHover) onHover (r); repaint(); }
        }
        void mouseExit (const juce::MouseEvent&) override
        { if (hoverRow != -1) { hoverRow = -1; if (onHover) onHover (-1); repaint(); } }
        juce::String getTooltip() override { return tip; }
        int contentHeight() const
        { return (int) rows.size() * kRowH + (noteText.isNotEmpty() ? kRowH : 0); }
        void paint (juce::Graphics& g) override
        {
            // Every row is the DYAD "0 + that degree": a red "0" chip (the root key,
            // drawn like the keyboard's period boundary) always precedes the degree
            // chip, so the pairing is explicit at a glance.
            const int rowH   = kRowH;
            const int W      = getWidth();
            const int chipW  = rowH - 2;
            const int labelX = 2 * chipW + 6, labelW = 50;
            const int valueX = labelX + labelW + 2;
            int y = 0;
            for (const auto& r : rows)
            {
                auto zeroR = juce::Rectangle<int> (0, y + 1, chipW, chipW);
                auto chipR = juce::Rectangle<int> (chipW + 2, y + 1, chipW, chipW);
                if (r.isPeriod)
                {
                    // The period: one red box covering the "0" AND the interval, white text.
                    g.setColour (juce::Colour (0xffe0453f));
                    g.fillRoundedRectangle (juce::Rectangle<int> (0, y + 1, valueX - 3, rowH - 2).toFloat(), 3.0f);
                    g.setColour (juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions ((float) rowH * 0.6f).withStyle ("Bold")));
                    g.drawText (r.num, zeroR, juce::Justification::centred, false);
                    g.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
                    g.drawText (r.label, labelX, y, labelW, rowH, juce::Justification::centredLeft, false);
                }
                else
                {
                    g.setColour (juce::Colour (0xffe0453f)); // red root chip "0"
                    g.fillRoundedRectangle (zeroR.toFloat(), 3.0f);
                    g.setColour (juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions ((float) rowH * 0.6f).withStyle ("Bold")));
                    g.drawText ("0", zeroR, juce::Justification::centred, false);
                    g.setColour (r.chip);
                    g.fillRoundedRectangle (chipR.toFloat(), 3.0f);
                    g.setColour (r.chip.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white);
                    g.drawText (r.num, chipR, juce::Justification::centred, false);
                    g.setColour (juce::Colour (0xffb9c0d0));
                    g.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
                    g.drawText (r.label, labelX, y, labelW, rowH, juce::Justification::centredLeft, false);
                }
                g.setColour (r.valueCol);
                g.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
                g.drawText (r.value, valueX, y, W - valueX, rowH, juce::Justification::centredLeft, false);
                y += rowH;
            }
            if (noteText.isNotEmpty())   // same voice as the n-ad list's status line
            {
                g.setColour (juce::Colour (0xff8f96a8));
                g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Italic")));
                g.drawText (noteText, 2, y, W - 4, rowH, juce::Justification::centredLeft, false);
            }
        }
    };

    // n-AD list: rows of degree chips (keyboard hues) + total-beating value.
    // The ADAPTED SPECTRUM, one row per partial: the interval it landed on and the
    // amplitude the search gave it, as a bar. Read-only - it is a read-out of what was
    // written into the Custom Engine, and the pencil/amplitude window still owns editing.
    struct AdaptList : juce::Component, juce::TooltipClient
    {
        struct Row { juce::String name; float level = 1.0f; }; // level = amp / loudest amp
        std::vector<Row> rows;
        juce::String tip, emptyText;
        static constexpr int kRowH = 18;
        int contentHeight() const { return (int) rows.size() * kRowH + (emptyText.isNotEmpty() ? 2 * kRowH : 0); }
        juce::String getTooltip() override { return tip; }
        void paint (juce::Graphics& g) override
        {
            int y = 0, r = 0;
            for (const auto& row : rows)
            {
                if (r % 2 == 1) { g.setColour (juce::Colour (0x14ffffff)); g.fillRect (0, y, getWidth(), kRowH); }
                g.setColour (juce::Colour (0xffd8dbe6));
                g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
                g.drawText (row.name, 4, y, 62, kRowH, juce::Justification::centredLeft, false);
                const int bx = 70, bw = juce::jmax (10, getWidth() - bx - 6);
                g.setColour (juce::Colour (0x22ffffff));
                g.fillRect (bx, y + 5, bw, kRowH - 10);
                g.setColour (juce::Colour (0xff8f7ad6));
                g.fillRect (bx, y + 5, (int) std::round ((float) bw * juce::jlimit (0.0f, 1.0f, row.level)),
                            kRowH - 10);
                y += kRowH; ++r;
            }
            if (emptyText.isNotEmpty())
            {
                g.setColour (juce::Colour (0xff8f96a8));
                g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Italic")));
                g.drawFittedText (emptyText, 4, y, getWidth() - 8, 2 * kRowH,
                                  juce::Justification::topLeft, 3);
            }
        }
    };

    struct NadList : juce::Component, juce::TooltipClient
    {
        struct Row { std::vector<int> degs; juce::String value; juce::Colour valCol; };
        std::vector<Row> rows;
        int degreesPerPeriod = 12;
        // Set when every degree of the tuning is one equal step from the last. Then a
        // shape found on degree 0 has the same intervals started from ANY degree, so
        // the row is shown as "any +10 +18" in neutral grey rather than as coloured
        // key numbers - the colours would claim specific keys the result is not tied to.
        bool equalStep = false;
        juce::String tip, emptyText; // status/coverage line (computing / over budget / partial)
        static constexpr int kRowH = 21;
        // Hovering a row SOUNDS that combination; only one sounds at a time.
        std::function<void (int)> onHover;
        int hoverRow = -1;
        void mouseMove (const juce::MouseEvent& e) override
        {
            const int r0 = e.y / kRowH;
            const int r = juce::isPositiveAndBelow (r0, (int) rows.size()) ? r0 : -1;
            if (r != hoverRow) { hoverRow = r; if (onHover) onHover (r); repaint(); }
        }
        void mouseExit (const juce::MouseEvent&) override
        { if (hoverRow != -1) { hoverRow = -1; if (onHover) onHover (-1); repaint(); } }
        int contentHeight() const { return (int) rows.size() * kRowH + (emptyText.isNotEmpty() ? kRowH : 0); }
        juce::String getTooltip() override { return tip; }
        // THE SCORE IS A COLUMN, NOT WHATEVER IS LEFT AFTER THE CHIPS.
        //
        // It used to be drawn at `x + 4`, where x was wherever the row's variable-width
        // degree chips happened to end - so the one number in this list a user actually
        // COMPARES down the column started at 1134 on a row of single-digit chips and at
        // 1139 on a row containing a "+10", and ended at 1164 against 1170. Every row was
        // internally consistent and the column was ragged, which is the worst of both: the
        // eye cannot scan it and nothing looks broken enough to report.
        //
        // The width is measured from the WIDEST value in the list, in the font it is drawn
        // in, so the column is as narrow as it can be while still holding every row - and
        // it is RIGHT-aligned, because these are magnitudes and the digits that matter line
        // up under each other.
        //
        // AND ITS LEFT EDGE CLEARS THE WIDEST ROW OF CHIPS, not just this row's. Pinning
        // the column to the right edge alone would be a column that a long combination
        // draws THROUGH - the chips are laid out left to right with no bound at all - so
        // the x is the further right of "flush with the list's right margin" and "past the
        // longest chip run in the list". Nothing is squashed or dropped either way: when
        // the chips are wider than the list, the value moves out with them exactly as it
        // did before, and the list is still the thing that has to be made wider.
        static juce::Font chipFont() { return juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")); }
        static juce::Font valueFont() { return juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")); }
        // The chip a degree is drawn in, as a width - the ONE copy of this arithmetic, so
        // the reservation and the drawing cannot disagree about where a row ends.
        static juce::String chipText (const Row& row, size_t k, bool equal)
        {
            return equal ? (k == 0 ? juce::String ("any")
                                   : "+" + juce::String (row.degs[k] - row.degs[0]))
                         : juce::String (row.degs[k]);
        }
        static int chipWidth (const juce::String& txt)
        {
            return juce::jmax (18, (int) std::ceil (
                       juce::GlyphArrangement::getStringWidth (chipFont(), txt)) + 8);
        }
        // THE SCORE COLUMN, PLUS THE PIXEL drawText NEEDS TO FINISH A GLYPH.
        //
        // This returned ceil(measured) - the column was exactly as wide as its widest
        // string and not one pixel wider - and drawText into a box that tight clips the
        // last glyph's right side bearing. Every row of every result list read "47.2" for
        // 47.28, "47.28" for 47.283: a digit short, in the one number this whole feature
        // exists to rank by, at every window size. Photographed at 1200x650 and confirmed
        // at 2x; no gate saw it, because the box a painted string is measured against is
        // the box it was given, and the string does fit it - by definition, since the box
        // was measured FROM the string.
        //
        // 3 px, which is the same allowance juce::Label's own border makes and enough for
        // any face at this size. The column is right-aligned, so the pixels land on the
        // side the clipping was on.
        int valueColumnWidth() const
        {
            float w = 0.0f;
            const auto f = valueFont();
            for (const auto& row : rows)
                w = juce::jmax (w, juce::GlyphArrangement::getStringWidth (f, row.value));
            return (int) std::ceil (w) + 3;
        }
        int widestChipRun() const
        {
            int widest = 0;
            for (const auto& row : rows)
            {
                int x = 4;
                for (size_t k = 0; k < row.degs.size(); ++k)
                    x += chipWidth (chipText (row, k, equalStep)) + 2;
                widest = juce::jmax (widest, x);
            }
            return widest;
        }

        void paint (juce::Graphics& g) override
        {
            int y = 0, r = 0;
            const int D = juce::jmax (1, degreesPerPeriod);
            // One column for the whole list, decided before the first row is drawn.
            const int valW = valueColumnWidth();
            const int valX = juce::jmax (widestChipRun() + 4, getWidth() - 6 - valW);
            for (const auto& row : rows)
            {
                if (r % 2 == 1) { g.setColour (juce::Colour (0x14ffffff)); g.fillRect (0, y, getWidth(), kRowH); }
                if (r == hoverRow) { g.setColour (juce::Colour (0x332fb155)); g.fillRect (0, y, getWidth(), kRowH); }
                int x = 4;   // no rank number: the beating value already orders them
                for (size_t k = 0; k < row.degs.size(); ++k)
                {
                    const int d = row.degs[k];
                    // Equal-step tuning: the shape is rootless, so the first chip says
                    // "any" and the rest are OFFSETS from it, all in neutral grey.
                    // Otherwise each chip is the actual degree, in its keyboard colour.
                    const bool grey = equalStep;
                    const juce::String txt = chipText (row, k, grey);
                    const auto col = grey
                        ? juce::Colour (0xff6b6b78)
                        : juce::Colour::fromHSV (0.82f * (float) (d % D) / (float) D, 0.62f, 0.9f, 1.0f);
                    const int w = chipWidth (txt);
                    auto chip = juce::Rectangle<int> (x, y + 2, w, kRowH - 4);
                    g.setColour (col);
                    g.fillRoundedRectangle (chip.toFloat(), 3.0f);
                    g.setColour (col.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white);
                    g.setFont (chipFont());
                    g.drawText (txt, chip, juce::Justification::centred, false);
                    x += w + 2;
                }
                g.setColour (row.valCol);
                g.setFont (valueFont());
                g.drawText (row.value, valX, y, valW, kRowH, juce::Justification::centredRight, false);
                y += kRowH; ++r;
            }
            if (emptyText.isNotEmpty()) // status line: computing / over budget / coverage
            {
                g.setColour (juce::Colour (0xff8f96a8));
                g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Italic")));
                g.drawText (emptyText, 4, y, getWidth() - 8, kRowH, juce::Justification::centredLeft, false);
            }
        }
    };

    // Two-column header over the dissonance list: "Step" and "Sethares dissonance"
    // (two lines). The list is always ordered by dissonance (most consonant first);
    // hovering explains that the dissonance tracks the synth's timbre live.
    struct DissHeader : juce::Component, juce::TooltipClient
    {
        // The EN/CA hook every self-painting component in this file needs, for the reason
        // d8aeeaf documents: the translated gate walks juce::Components, so a word drawn
        // straight onto a Graphics is invisible to it and stays in one language forever.
        std::function<juce::String (const char*, const char*)> trF;
        juce::String tip;
        int  splitX = 92;
        juce::String getTooltip() override { return tip; }
        void paint (juce::Graphics& g) override
        {
            const juce::Colour c (0xff8f96a8);
            g.setColour (c);
            g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
            g.drawText (trF ? trF ("Step", "Grau") : juce::String ("Step"),
                        juce::Rectangle<int> (0, 0, splitX, getHeight()),
                        juce::Justification::centredLeft, false);
            auto rr = juce::Rectangle<int> (splitX, 0, getWidth() - splitX, getHeight());
            g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
            g.drawText (trF ? trF ("beating", "batec") : juce::String ("beating"),
                        rr, juce::Justification::centredLeft, false);
        }
    };

    // Chord-search results: "#rank  [play]  note note note..." - one colour PER NOTE
    // INDEX; the play glyph previews (all together, then an ascending strum);
    // double-clicking a row loads the chord as the active .scl scale.
    struct ChordList : juce::Component, juce::TooltipClient
    {
        struct Row { std::vector<juce::String> notes; double value = 0.0; };
        std::vector<Row> rows;
        juce::String emptyText { "Press Find to search.\nhover a row to hear it\ndouble-click loads it as the scale" };
        std::function<void (int)> onPlay, onLoad, onHover; // onHover(-1) = pointer left
        // No play column any more (hover sounds it), so the notes get that space.
        static constexpr int kRowH = 22, kRankW = 2, kGapW = 7;
        int contentHeight() const { return (int) rows.size() * kRowH; }
        static juce::Font rowFont() { return juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")); }
        // Columns are only as wide as the widest text they actually hold, so the
        // intervals start hard left and sit as close together as they can while the
        // columns still line up down the list. Nothing is ever squeezed or hidden:
        // if the widest row still overflows, the Viewport scrolls sideways.
        std::vector<int> columnWidths() const
        {
            size_t maxN = 0;
            for (const auto& r : rows) maxN = std::max (maxN, r.notes.size());
            std::vector<int> w (maxN, 0);
            const auto f = rowFont();
            for (const auto& r : rows)
                for (size_t k = 0; k < r.notes.size(); ++k)
                    w[k] = juce::jmax (w[k], (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, r.notes[k])));
            return w;
        }
        int contentWidth() const
        {
            int total = kRankW;
            for (const int w : columnWidths()) total += w + kGapW;
            return total + 2;
        }
        // THE HOVER OVER A FOUND TUNING SAYS TWO THINGS. "AL PASAR EL RATON POR ENCIMA
        // DE UN TUNING ENCONTRADO QUE SOLO PONGA DOBLE CLIC PARA CARGAR Y EL VALOR DE
        // DISONANCIA TOTAL DEL CONJUNTO, NO TODO LO DEMAS."
        //
        // The list used to answer with the panel's paragraph - how the search works, what
        // Sethares is, what Play does - which is the right text for the button that
        // STARTS a search and the wrong one for the twelfth row of its results. Over a
        // row it is now what you can do with that row and what that row scored; off the
        // rows (the empty area under a short list) it is still the panel's explanation,
        // because there the question really is "what is this list".
        juce::String tip;
        juce::String rowTipLoad, rowTipValue;   // written by applyLanguage()
        juce::String getTooltip() override
        {
            const auto p = getMouseXYRelative();
            const int row = p.y / kRowH;
            if (! getLocalBounds().contains (p) || row < 0 || row >= (int) rows.size())
                return tip;
            // Four decimals: the values this search separates differ in the third and
            // fourth, and a rounded read-out would show two neighbouring results as the
            // same number - which is exactly the comparison the hover exists to serve.
            return rowTipLoad + "\n" + rowTipValue + " "
                 + juce::String (rows[(size_t) row].value, 4);
        }
        static juce::Colour noteColour (int k)
        {
            static const juce::uint32 pal[] = { 0xffe0453f, 0xffffc95c, 0xff4fe09a, 0xff4fe6ff,
                                                0xff9a7bff, 0xffff77c8, 0xffff9a3c, 0xffc4ccda };
            return juce::Colour (pal[(size_t) (k % 8)]);
        }
        void paint (juce::Graphics& g) override
        {
            if (rows.empty())
            {
                g.setColour (juce::Colour (0xff8f96a8));
                g.setFont (juce::Font (juce::FontOptions (12.5f)));
                g.drawFittedText (emptyText,
                                  getLocalBounds().reduced (8), juce::Justification::centred, 4, 0.8f);
                return;
            }
            const auto cols = columnWidths();
            int y = 0, row = 0;
            for (const auto& r : rows)
            {
                if (row % 2 == 1) { g.setColour (juce::Colour (0x14ffffff)); g.fillRect (0, y, getWidth(), kRowH); }
                if (row == hoverRow) // the row you are hearing
                { g.setColour (juce::Colour (0x332fb155)); g.fillRect (0, y, getWidth(), kRowH); }
                // the chord's notes, one colour per note index, packed left with just
                // enough room for the widest entry of each column
                int x = kRankW;
                g.setFont (rowFont());
                for (int k = 0; k < (int) r.notes.size(); ++k)
                {
                    const int cw = k < (int) cols.size() ? cols[(size_t) k] : 0;
                    g.setColour (noteColour (k));
                    g.drawFittedText (r.notes[(size_t) k], juce::Rectangle<int> (x, y, cw, kRowH),
                                      juce::Justification::centredLeft, 1, 0.5f);
                    x += cw + kGapW;
                }
                y += kRowH; ++row;
            }
        }
        // HOVER plays it - there is no play button any more, and only one chord ever
        // sounds: moving to another row (or off the list) stops the previous one.
        int hoverRow = -1;
        void mouseMove (const juce::MouseEvent& e) override
        {
            const int row = e.y / kRowH;
            const int r = juce::isPositiveAndBelow (row, (int) rows.size()) ? row : -1;
            if (r != hoverRow) { hoverRow = r; if (onHover) onHover (r); repaint(); }
        }
        void mouseExit (const juce::MouseEvent&) override
        { if (hoverRow != -1) { hoverRow = -1; if (onHover) onHover (-1); repaint(); } }
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            const int row = e.y / kRowH;
            if (juce::isPositiveAndBelow (row, (int) rows.size()) && onLoad) onLoad (row);
        }
    };

    // "From spectrum's segment" partial picker: one row per partial of the current
    // spectrum. Click toggles selection; Shift-click selects the span to the anchor;
    // all selected rows share one colour; the tonic (lowest selected = C3) is starred.
    struct SegmentList : juce::Component, juce::TooltipClient
    {
        FractalAdditiveAudioProcessor* proc = nullptr;
        std::function<void()> onChange;
        std::function<juce::String (double)> ratioName;
        juce::String tip;
        int numRows = 0;
        static constexpr int kRowH = 18;
        juce::String getTooltip() override { return tip; }
        int contentHeight() const { return numRows * kRowH; }
        void paint (juce::Graphics& g) override
        {
            if (proc == nullptr) return;
            const auto* t = proc->getLiveTemplate();
            const int n = t != nullptr ? (int) t->partials.size() : 0;
            const int tonic = proc->segmentTonicIndex();
            for (int i = 0; i < n; ++i)
            {
                const int y = i * kRowH;
                const bool sel = proc->segmentSelected (i);
                if (sel)
                {
                    g.setColour (juce::Colour (0xff2fb0c8)); // one shared colour for all selected
                    g.fillRoundedRectangle (juce::Rectangle<int> (0, y + 1, getWidth(), kRowH - 2).toFloat(), 3.0f);
                }
                const bool isTonic = (i == tonic);
                // Tonic (lowest selected = C3) stands out in gold; others white/grey.
                g.setColour (isTonic ? juce::Colour (0xffffc95c) : (sel ? juce::Colours::white : juce::Colour (0xffb9c0d0)));
                g.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
                g.drawText ("P" + juce::String (i + 1) + (isTonic ? " C3" : juce::String()),
                            4, y, 64, kRowH, juce::Justification::centredLeft, false);
                if (ratioName)
                    g.drawText (ratioName ((double) t->partials[(size_t) i].ratio),
                                70, y, getWidth() - 72, kRowH, juce::Justification::centredLeft, false);
            }
        }
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (proc == nullptr) return;
            const auto* t = proc->getLiveTemplate();
            const int n = t != nullptr ? (int) t->partials.size() : 0;
            const int i = e.y / kRowH;
            if (i >= 0 && i < n) { proc->segmentToggle (i, e.mods.isShiftDown()); if (onChange) onChange(); }
        }
    };

    // Closable floating notice (e.g. "only N minima exist"). Click X to dismiss.
    // Floating "Recent" window: every scale file loaded in this plugin, newest first,
    // scrollable, click one to load it.
    struct RecentOverlay : juce::Component
    {
        // The EN/CA hook every self-painting component in this file needs, for the reason
        // d8aeeaf documents: the translated gate walks juce::Components, so a word drawn
        // straight onto a Graphics is invisible to it and stays in one language forever.
        std::function<juce::String (const char*, const char*)> trF;
        struct List : juce::Component
        {
            // Its own hook: this inner list paints the empty-state line and cannot see
            // the enclosing overlay's member.
            std::function<juce::String (const char*, const char*)> trF;
            juce::StringArray paths;
            std::function<void (int)> onPick;
            static constexpr int kRowH = 22;
            int contentHeight() const { return juce::jmax (kRowH, paths.size() * kRowH); }
            void paint (juce::Graphics& g) override
            {
                int y = 0;
                for (int i = 0; i < paths.size(); ++i)
                {
                    if (i % 2 == 1) { g.setColour (juce::Colour (0x14ffffff)); g.fillRect (0, y, getWidth(), kRowH); }
                    if (i == hoverRow) { g.setColour (juce::Colour (0x33b27bff)); g.fillRect (0, y, getWidth(), kRowH); }
                    const juce::File f (paths[i]);
                    g.setColour (juce::Colour (0xff8f96a8));
                    g.setFont (juce::Font (juce::FontOptions (10.5f).withStyle ("Bold")));
                    g.drawText ("#" + juce::String (i + 1), 4, y, 26, kRowH, juce::Justification::centredLeft, false);
                    g.setColour (juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
                    g.drawText (f.getFileNameWithoutExtension(), 32, y, getWidth() - 200, kRowH,
                                juce::Justification::centredLeft, false);
                    g.setColour (juce::Colour (0xff6c7488));
                    g.setFont (juce::Font (juce::FontOptions (11.0f)));
                    g.drawText (f.getParentDirectory().getFileName(), getWidth() - 166, y, 160, kRowH,
                                juce::Justification::centredRight, false);
                    y += kRowH;
                }
                if (paths.isEmpty())
                {
                    g.setColour (juce::Colour (0xff8f96a8));
                    g.setFont (juce::Font (juce::FontOptions (12.5f)));
                    g.drawText (trF ? trF ("nothing loaded yet", "encara no s'ha carregat res")
                                    : juce::String ("nothing loaded yet"),
                                getLocalBounds(), juce::Justification::centred, false);
                }
            }
            int hoverRow = -1;
            void mouseMove (const juce::MouseEvent& e) override
            { const int r = e.y / kRowH; if (r != hoverRow) { hoverRow = r; repaint(); } }
            void mouseExit (const juce::MouseEvent&) override { hoverRow = -1; repaint(); }
            void mouseDown (const juce::MouseEvent& e) override
            {
                const int r = e.y / kRowH;
                if (juce::isPositiveAndBelow (r, paths.size()) && onPick) onPick (r);
            }
        };

        juce::String title { "Recent" };
        juce::TextButton closeBtn { "X" };
        juce::Viewport view;
        List list;
        RecentOverlay()
        {
            closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff44445e));
            closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            closeBtn.onClick = [this] { setVisible (false); };
            addAndMakeVisible (closeBtn);
            view.setViewedComponent (&list, false);
            view.setScrollBarsShown (true, false);
            addAndMakeVisible (view);
        }
        void setPaths (const juce::StringArray& p)
        {
            list.paths = p;
            list.setSize (juce::jmax (10, view.getWidth() - 10), list.contentHeight());
            list.repaint();
        }
        void resized() override
        {
            closeBtn.setBounds (getWidth() - 28, 6, 22, 22);
            auto r = getLocalBounds().reduced (14, 12);
            r.removeFromTop (24);
            view.setBounds (r);
            list.setSize (juce::jmax (10, r.getWidth() - 10), list.contentHeight());
        }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xf2141a24));
            g.fillRoundedRectangle (r, 10.0f);
            g.setColour (juce::Colour (0xffb27bff));
            g.drawRoundedRectangle (r.reduced (1.5f), 10.0f, 3.0f);
            g.setColour (juce::Colour (0xffb27bff));
            g.setFont (juce::Font (juce::FontOptions (16.0f).withStyle ("Bold")));
            g.drawText (title, getLocalBounds().reduced (16, 10).removeFromTop (22),
                        juce::Justification::centredLeft, false);
        }
    };

    // THE WHOLE NOTICE, ALWAYS. This used to be a fixed 560x220 box whose body was drawn
    // with drawFittedText capped at 8 lines - a notice with a parser's message, or a
    // playground that lost three corners, was squashed and then cut with an ellipsis, and
    // no check in the suite looked at it (the text sweep walks Labels, Buttons and combos;
    // this paints its own). Title and body are wrapped TextLayouts now and heightFor() is
    // the height that shows every line of both; the editor sizes the overlay with it.
    struct NoticeOverlay : juce::Component
    {
        juce::String title, body;
        juce::String moreText;             // "1 more notice waits..." - empty when none wait
        juce::TextButton closeBtn { "X" };
        std::function<void()> onClosed;    // the editor shows the next queued notice
        std::function<void()> onExpand;    // the editor re-lays the overlay around the new height
        // ONE OPTIONAL ACTION per notice (the load warning's "Don't show this again"). The
        // render lambda sets both, and a new notice starts with neither (raiseNotice clears
        // them), so a button can never be left over from the notice before.
        juce::TextButton actionBtn;
        juce::String actionText;
        std::function<void()> onAction;
        // A LONG NOTICE OPENS SHORT. "veo que la ventana close to freezing tiene un texto
        // gigante, haz que todos los textos asi sean desplegables siempre que sea posible"
        // (2026-10-03). The first paragraph stays on screen and the rest is one click away,
        // behind a button that SAYS how much more there is - the text is folded, never cut,
        // and nothing decides for the reader that they did not want it.
        bool expanded = false;
        juce::TextButton expandBtn;
        static constexpr int kPadX = 22, kPadY = 14, kMinTitleH = 24, kGap = 6, kMoreH = 20, kCloseRoom = 30,
                             kActionH = 32;
        // Only a body with a SECOND paragraph folds: a notice that is one paragraph long is
        // already short enough, and a fold with nothing behind it is a button that lies.
        int leadEnd() const
        {
            const int i = body.indexOf ("\n\n");
            return i > 0 && body.length() - i > 24 ? i : -1;
        }
        bool canExpand() const { return leadEnd() > 0; }
        bool hasButtonRow() const { return actionText.isNotEmpty() || canExpand(); }
        juce::String shownBody() const
        { return canExpand() && ! expanded ? body.substring (0, leadEnd()) : body; }
        // How much is folded away, in lines of the body's own paragraphs - a number, so the
        // button is an offer and not a mystery.
        int foldedParagraphs() const
        {
            const int i = leadEnd();
            if (i <= 0) return 0;
            return juce::StringArray::fromTokens (body.substring (i + 2), "\n\n", "").size();
        }
        NoticeOverlay()
        {
            closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff44445e));
            closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            closeBtn.onClick = [this] { setVisible (false); if (onClosed) onClosed(); };
            addAndMakeVisible (closeBtn);
            actionBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2d6da8));
            actionBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            // A COPY is called: the action may close this notice, and the next one's
            // raiseNotice clears onAction - replacing a std::function while it runs.
            actionBtn.onClick = [this] { auto f = onAction; if (f) f(); };
            addChildComponent (actionBtn);
            expandBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3a3a52));
            expandBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            expandBtn.onClick = [this] { expanded = ! expanded; if (onExpand) onExpand(); };
            addChildComponent (expandBtn);
        }
        juce::TextLayout layoutOf (const juce::String& text, juce::Font font, juce::Colour colour, int width) const
        {
            juce::AttributedString as;
            as.setWordWrap (juce::AttributedString::byWord);
            as.append (text, font, colour);
            juce::TextLayout tl;
            tl.createLayout (as, (float) juce::jmax (40, width));
            return tl;
        }
        juce::TextLayout titleLayout (int innerW) const
        { return layoutOf (title, juce::Font (juce::FontOptions (17.0f).withStyle ("Bold")), juce::Colour (0xff5cb8ff), innerW - kCloseRoom); }
        juce::TextLayout bodyLayout (int innerW) const
        { return layoutOf (shownBody(), juce::Font (juce::FontOptions (13.5f)), juce::Colours::white, innerW); }
        // What the WHOLE text would need, folded or not: the fit sweep asks this so a notice
        // whose expanded form cannot fit the window is still reported, never discovered by a
        // reader who opens it.
        juce::TextLayout fullBodyLayout (int innerW) const
        { return layoutOf (body, juce::Font (juce::FontOptions (13.5f)), juce::Colours::white, innerW); }
        // What the WHOLE text needs, folded or not (the fit sweep and the checks that ask
        // whether the box grows to its text use this, so a fold cannot make them pass).
        int heightForFull (int width) const
        {
            const int inner = width - 2 * kPadX;
            return kPadY + juce::jmax (kMinTitleH, (int) std::ceil (titleLayout (inner).getHeight())) + kGap
                 + (int) std::ceil (fullBodyLayout (inner).getHeight())
                 + (moreText.isNotEmpty() ? kMoreH : 0)
                 + (hasButtonRow() ? kActionH : 0) + kPadY;
        }
        int heightFor (int width) const
        {
            const int inner = width - 2 * kPadX;
            return kPadY + juce::jmax (kMinTitleH, (int) std::ceil (titleLayout (inner).getHeight())) + kGap
                 + (int) std::ceil (bodyLayout (inner).getHeight())
                 + (moreText.isNotEmpty() ? kMoreH : 0)
                 + (hasButtonRow() ? kActionH : 0) + kPadY;
        }
        // WHERE THE BODY IS PAINTED - one function for paint and for the check that asks
        // whether the body fits it (noticeProbeForTests), so the two cannot disagree.
        juce::Rectangle<int> bodyArea() const
        {
            auto in = getLocalBounds().reduced (kPadX, kPadY);
            in.removeFromTop (juce::jmax (kMinTitleH, (int) std::ceil (titleLayout (in.getWidth()).getHeight())));
            in.removeFromTop (kGap);
            if (hasButtonRow()) in.removeFromBottom (kActionH);            // the buttons' own row
            if (moreText.isNotEmpty())   in.removeFromBottom (kMoreH);     // the "N more" line
            return in;
        }
        void resized() override
        {
            closeBtn.setBounds (getWidth() - 28, 6, 22, 22);
            actionBtn.setButtonText (actionText);
            actionBtn.setVisible (actionText.isNotEmpty());
            const int rowY = getHeight() - kPadY - 26;
            const int w = juce::jmin (getWidth() - 2 * kPadX, fgfit::snugTextButtonWidth (actionBtn, 24));
            actionBtn.setBounds (kPadX, rowY, juce::jmax (40, w), 24);
            expandBtn.setVisible (canExpand());
            if (canExpand())
            {
                const int ew = juce::jmin (getWidth() - 2 * kPadX, fgfit::snugTextButtonWidth (expandBtn, 24));
                expandBtn.setBounds (getWidth() - kPadX - juce::jmax (40, ew), rowY, juce::jmax (40, ew), 24);
            }
        }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xf2141a24));
            g.fillRoundedRectangle (r, 10.0f);
            g.setColour (juce::Colour (0xff5cb8ff));
            g.drawRoundedRectangle (r.reduced (1.5f), 10.0f, 3.0f);
            auto top = getLocalBounds().reduced (kPadX, kPadY);
            const auto tl = titleLayout (top.getWidth());
            auto titleArea = top.removeFromTop (juce::jmax (kMinTitleH, (int) std::ceil (tl.getHeight())));
            tl.draw (g, titleArea.withWidth (top.getWidth() - kCloseRoom).toFloat());
            const auto in = bodyArea();
            if (moreText.isNotEmpty())
            {
                g.setColour (juce::Colour (0xffffc95c));
                g.setFont (juce::Font (juce::FontOptions (12.5f)));
                g.drawText (moreText, juce::Rectangle<int> (in.getX(), in.getBottom(), in.getWidth(), kMoreH),
                            juce::Justification::bottomLeft, false);
            }
            // Clipped to its own area: when a window too small for the whole notice caps the
            // overlay (the ONE case heightFor cannot satisfy, and noticeProbeForTests reports it
            // as not fitting), the body must not run over the "N more wait" line below it.
            const auto bl = bodyLayout (in.getWidth());
            juce::Graphics::ScopedSaveState keep (g);
            g.reduceClipRegion (in);
            bl.draw (g, in.withHeight (juce::jmax (in.getHeight(), (int) std::ceil (bl.getHeight()))).toFloat());
        }
    };

    // Darkens everything behind the Spectrum-to-Rhythm panel so it does not
    // distract; clicking it (= clicking outside the panel) closes the panel.
    //
    // 0xa8 (66% black) WAS NOT A SCRIM, IT WAS A TINT. Photographed at 1200x650 with the
    // panel open, the keyboard's rainbow along y 575-641 kept about a third of its
    // luminance and every one of its hues, so the two brightest, most saturated bands in
    // the whole window sat in the corners of a picture whose subject is a dialog in the
    // middle - the eye went to the corners. The main window's "TRANSITION" cell was still
    // legible THROUGH it as well, at x 1100-1250: a scrim you can read a word through is
    // not separating anything.
    //
    // 0xd9 is 85% black: what is behind stays visible as shape and position (this is a
    // modal over a window the user is still working in, not a curtain), but the rainbow
    // drops to 15% of its luminance, which puts it below every element of the dialog in
    // front of it. The 0x000008 is the page's own blue cast, kept so the darkened window
    // reads as the same window rather than as a grey photograph of one.
    struct Scrim : juce::Component
    {
        std::function<void()> onClick;
        void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
        void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xd9000008)); }
    };

    // Timbre -> rhythm (MIDI out) panel: pick partials of the CURRENT patch (shown
    // with their just-intonation ratio to the fundamental, or cents when inexact /
    // .scl-derived), route each to a MIDI note + gate length, and set the global
    // divisor x^n. The pulse trains leave on the plugin's MIDI OUTPUT in exact
    // polyrhythm with the sounding timbre (see RhythmEngine.h for the maths).
    struct RhythmPanel : juce::Component, juce::ListBoxModel
    {
        // The -12 / +12 steppers on every slot row. 30 px left 22 for the word, and
        // "+12" measures 26 in the font a 22 px button draws in, so both were squashed
        // on every row of the panel at once.
        static constexpr int kStepBtnW = 36;

        // THE GROUP TITLE'S OWN ROW, and why it is 14 px and not the 13 it was.
        //
        // The five section titles are drawn at 13.0 px Bold now (see the loop that sets
        // them, and the note beside outLbl for why anything at or below 12.5 would have
        // changed nothing at all). A 13 px row held a 12.5 px line with half a pixel to
        // spare; a 13.0 px line in it sits exactly flush with the component's own top and
        // bottom edge, which is the state where one rounding step in the wrong direction
        // shaves the top off a capital. One more pixel of row is the whole cost, and it
        // is paid out of the routed list at the foot of the panel: three pixels across
        // the three stacked boxes, an eighth of one of that list's 22 px rows.
        //
        // NOT taken out of the 24 px control row inside each box instead, which is the
        // other place it could come from: a ComboBox's chevron gutter and an "onled"
        // pill's word are both derived from the row's HEIGHT (fgfit's chevron note and
        // onLedFont, which is h * 0.62), so a pixel off there does not move those
        // controls, it re-measures them - and this panel's mode box is already sized to
        // its widest Catalan item with nothing to spare.
        static constexpr int kSecHdrH = 14;

        //== THE PANEL WEARS ONE HUE, AND IT IS THE SPECTRUM'S =====================
        //
        // "EIGHT ACCENT HUES IN ONE 816x637 DIALOG". Counted from the render: a blue
        // frame and title (0x5cb8ff), a pale blue PLAYBACK, a cyan PULSE RATE that was
        // ALSO the Steps fader, a pink OUTPUT that was also three sliders' tracks, an
        // amber DAW TEMPO that was also the BPM read-out and the loudest button on
        // screen, a green NOTE START that was also the read-out and the empty-state
        // hint, plus the grey text ramp. Four framed boxes in four different colours, of
        // which two had tinted fills and two did not. Meanwhile every other panel in the
        // product wears exactly ONE identity colour on its tab, its border, its captions,
        // its slider thumbs and its read-outs. This one was the only place that opted out.
        //
        // WHY IT DOES NOT GET A COLOUR OF ITS OWN, which was the first thing tried.
        // galactic::palette holds twelve identity colours chosen by search under two hard
        // constraints (SpectrumTests asserts both): no two closer than 15 units of OKLab,
        // and every one at 5.9:1 or better on the panel surface it is drawn on. A brute
        // scan of the whole RGB cube for a THIRTEENTH that clears those - and stays >= 5
        // units apart under deuteranopia and protanopia - tops out at #00a0ff, 12.70
        // units from its nearest neighbour. There is no room left: the set is full, and
        // adding a colour at 12.7 would quietly make the palette worse for every panel to
        // buy a hue for this one.
        //
        // So it WEARS one instead of inventing one, and the rule that lets it is this
        // panel's own: rule (b) is about telling two things apart AT THE SAME TIME, and
        // this is a modal over an opaque scrim - while it is open, nothing else in the
        // window is on screen to be confused with. Nothing here is ever seen beside the
        // Spectrum panel.
        //
        // It wears the SPECTRUM's, because that is what it is made of: every row of its
        // list is a partial of the spectrum loaded right now, the routed trains are named
        // "P3", "P7" after those partials, and the title says SPECTRUM VS RHYTHM. The
        // panel is the spectrum's rhythm face.
        //
        // WHAT KEEPS ITS OWN COLOUR, and why that is not a ninth hue: the ON/OFF pills,
        // the JI switch, the +/- steppers and the row delete are SEMANTIC green and red
        // (galactic green 0x2fb155 / red 0xe0453f) used identically everywhere else in
        // the product - green is on, red is off or destroys. The palette header makes the
        // same exception in as many words ("Load is green, Reset is red... because of
        // what they MEAN"). A state colour is not an identity colour.
        static constexpr juce::uint32 kHueRgb = galactic::palette::kHarmonic;   // 0xa6a816
        static juce::Colour hue (float alpha = 1.0f)
        { return juce::Colour (0xff000000u | kHueRgb).withAlpha (alpha); }
        // The neutral text ramp the whole product uses, named so the panel stops spelling
        // the same three greys out in fourteen places.
        static juce::Colour ink()    { return juce::Colour (0xffe6e9f0); }   // primary text
        static juce::Colour inkDim() { return juce::Colour (0xffb9c0d0); }   // captions
        static juce::Colour inkFaint(){ return juce::Colour (0xff8f96a8); }  // column heads
        FractalAdditiveAudioProcessor* proc = nullptr;
        std::function<juce::String (double)> ratioName;                       // 3/2 or 702c
        std::function<juce::String (const char*, const char*)> trF;           // EN/CA
        std::function<void()> onEdited;                                       // refocus etc.
        std::function<void()> onClose;                                        // hide panel + scrim

        juce::ListBox      list { "partials", this };
        juce::ToggleButton onT, selfT, mpeT, tempoT; // onT = MIDI out; selfT = play in Fibló; tempoT = DAW-tempo sync (CC20)
        juce::ToggleButton droneT;                    // also sound the held note under the pulses
        juce::ToggleButton qTempoT, monoT, qPulseT;   // note-start gate: beat-quantize / mono / pattern-quantize
        juce::Label        secGate;                   // "NOTE START" group header
        juce::ToggleButton feedbackT;                 // internal recursive feedback loop
        juce::Slider       feedbackS;                 // its volume slope (decay per loop)
        juce::Slider       velSlopeS;                 // velocity tilt across the spectrum
        juce::Label        feedbackLbl;               // "Feedback" caption
        juce::Label        outLbl, selfLbl;           // captions under the two big pill toggles
        juce::ComboBox     modeBox;
        juce::Label        heading, listHead, info, noteHead, durHead, velHead, bpmLbl, pickHint;
        // ONE FRAME PER SECTION, and there are five now. Only PULSE RATE used to have
        // one; the other three were a coloured word over a hairline and the two big
        // toggles at the top had no group at all, which is most of why this window read
        // as a pile rather than as a panel.
        juce::Rectangle<int> playBox, rateBox, outBox, tempoBox, gateBox;
        juce::Label        secRate, secOut, secTempo, secPlay;         // coloured group headers
        juce::Label        divLbl, timesLbl, nVal, xVal;      // "divide [n] times by [x]"
        juce::TextButton   nUp, nDn, xUp, xDn;                // tiny round steppers
        juce::TextButton   closeB { "X" }, helpB, jiB, tempoX2, tempoD2;
        // "JI switched MPE out on, so JI off should switch it back" - a PARAMETER, not
        // an editor member: as a member it was lost on reopen and the restore never fired.
        bool jiForcedMpe() const
        {
            return proc != nullptr
                && proc->apvts.getRawParameterValue ("rhythmJiForcedMpe")->load() > 0.5f;
        }
        void setJiForcedMpe (bool b)
        {
            if (proc == nullptr) return;
            if (auto* p = proc->apvts.getParameter ("rhythmJiForcedMpe"))
                p->setValueNotifyingHost (b ? 1.0f : 0.0f);
        }
        juce::String       helpText;                          // shown by the Help button
        // ...and its TITLE, which was hard-coded English while the body beside it changed
        // language. A native message box is not a Component, so nothing walks it: the only
        // way this shows up is by reading the call.
        juce::String       helpTitle { "Spectrum vs Rhythm" };
        // Slot rows live in a scrollable container: kSlots is large ("unlimited"
        // pulses), so the panel shows a window onto however many are routed.
        juce::Viewport     slotView;
        juce::Component    slotRows;
        juce::Label        slotLbl[RhythmEngine::kSlots];
        juce::TextEditor   pitchE[RhythmEngine::kSlots];
        juce::TextButton   s12M[RhythmEngine::kSlots], s12P[RhythmEngine::kSlots]; // -12/+12 (steps only)
        juce::Slider       durS[RhythmEngine::kSlots];
        juce::Slider       velS[RhythmEngine::kSlots]; // per-slot MIDI velocity of each pulse
        juce::TextButton   delB[RhythmEngine::kSlots];
        juce::Label        octLbl, octAllVal;          // "Octave (all pulses)" stepper
        juce::Label        stepsAllLbl;                // "Steps (all pulses)" caption
        juce::Slider       stepsAllS;                  // global scale-step offset (modulatable)
        juce::TextButton   octAllUp, octAllDn;
        int numRows = 0;
        // Which of the two arrangements of the routing pane is currently laid out - see
        // the flip check at the end of refreshSlots().
        bool lastAnyRouted = false;

        // Clicking any empty spot of the panel gives keyboard focus back to the
        // editor (text boxes commit + collapse; the play keys work again).
        void mouseDown (const juce::MouseEvent&) override { if (onEdited) onEdited(); }

        // THE UNIT ON A STEPS FIELD, IN THE LANGUAGE ON SCREEN.
        //
        // "0step", "7step" and "-12step" were drawn identically in both languages, and the
        // rig's translation gate could not see them until the fit sweep learned to walk a
        // TextEditor's own text: a routed row's steps field is a control the user types
        // into, so its CONTENT is a user-visible string like any caption.
        //
        // PER PANEL, NOT PER PROCESS. This was an inline static, and a static is shared by
        // every plugin instance in the host: two instances of Fibló in one set, one
        // switched to Catalan, and the OTHER one's steps fields turned into "7pas" inside
        // an otherwise English window the next time anything refreshed them - and
        // symmetrically, opening a new English editor flipped a Catalan sibling's back.
        // The language is a property of an EDITOR (each has its own flag and its own LANG
        // button), so anything derived from it has to live no wider than one.
        //
        // Not a data race - every reader and the writer are on the message thread - and
        // nothing was ever lost, because parseSteps accepts both languages' words whatever
        // is being shown. It was purely the wrong word on screen, in the one language the
        // user did not choose.
        juce::String stepWord { "step" };
        // Scale steps FROM the played note: "7step" = 7 steps up, "7step-" (or
        // "-7step") = 7 down. Accepts "step"/"steps"/"st". INT_MIN if not a steps spec.
        static int parseSteps (juce::String t)
        {
            t = t.trim().toLowerCase().removeCharacters (" ");
            bool down = false;
            if (t.endsWith ("-"))   { down = true; t = t.dropLastCharacters (1); }
            if (t.startsWith ("-")) { down = true; t = t.substring (1); }
            if      (t.endsWith ("steps")) t = t.dropLastCharacters (5);
            else if (t.endsWith ("step"))  t = t.dropLastCharacters (4);
            else if (t.endsWith ("passos")) t = t.dropLastCharacters (6);
            else if (t.endsWith ("pas"))    t = t.dropLastCharacters (3);
            else if (t.endsWith ("st"))    t = t.dropLastCharacters (2);
            else return std::numeric_limits<int>::min();
            if (t.isEmpty() || ! t.containsOnly ("0123456789")) return std::numeric_limits<int>::min();
            const int v = juce::jlimit (0, 127, t.getIntValue());
            return down ? -v : v;
        }
        juce::String stepsText (int steps) const
        {
            // SHOWN with the minus in front, which is where a reader expects it.
            // "12step-" is what this used to draw, and a trailing minus reads as a typo
            // or as a range rather than as a direction. parseSteps accepts BOTH forms
            // (see it above), so what the user may TYPE is unchanged - only what they are
            // shown. It round-trips: "-12step" parses back to -12.
            return (steps < 0 ? "-" + juce::String (-steps) : juce::String (steps)) + stepWord;
        }

        // Best small fraction p/q ~ v (continued fractions, den <= 16) for the
        // integer polyrhythm chain display.
        static void approxFraction (double v, long long& p, long long& q)
        {
            long long bestP = 1, bestQ = 1; double bestErr = 1.0e18;
            for (long long d = 1; d <= 16; ++d)
            {
                const long long n = (long long) std::llround (v * (double) d);
                if (n <= 0) continue;
                const double err = std::abs (v - (double) n / (double) d);
                if (err < bestErr - 1.0e-12) { bestErr = err; bestP = n; bestQ = d; }
            }
            p = bestP; q = bestQ;
        }

        // MPE pitch spec: "1987.4c" (cents) or a ratio "3" / "3/2" -> linear ratio.
        static double parseRatioSpec (juce::String t)
        {
            t = t.trim();
            // Accept BOTH "700c" and the "700\xc2\xa2" the rest of the UI prints: without
            // the cent sign a pasted-back value was read as a linear RATIO of 700.
            if (t.endsWith (juce::String::fromUTF8 ("\xc2\xa2")))
                t = t.dropLastCharacters (1).trim() + "c";
            if (t.endsWithIgnoreCase ("c"))
                return std::exp2 (t.dropLastCharacters (1).getDoubleValue() / 1200.0);
            if (t.containsChar ('/'))
            {
                const double p = t.upToFirstOccurrenceOf ("/", false, false).getDoubleValue();
                const double q = t.fromFirstOccurrenceOf ("/", false, false).getDoubleValue();
                return (p > 0.0 && q > 0.0) ? p / q : 0.0;
            }
            const double v = t.getDoubleValue();
            return v > 0.0 ? v : 0.0;
        }

        void init()
        {
            // THE TITLE IS THE LOUDEST THING IN THE PANEL NOW, WHICH IT WAS NOT.
            //
            // "SPECTRUM VS RHYTHM" used to be drawn in 0xff5cb8ff - the same blue as the
            // frame two pixels away from it - so the panel's name had the contrast of a
            // border, while "How to route" sat beside it on a saturated amber fill and
            // was the brightest object in the whole dialog. The hierarchy was upside
            // down: the loudest thing was a help button.
            //
            // It is a TITLE CHIP now, the same one every macro panel in the main window
            // wears: a filled rectangle in the panel's hue with the letters knocked out
            // of it in black (the fill is painted in paint(), which runs before the
            // children, so the Label's own text lands on top of it). A filled chip is the
            // highest-contrast object the palette can make, so the name of the panel is
            // now measurably the first thing in it - and it is the same idiom, in the
            // same place, as the titles of the panels behind it.
            heading.setJustificationType (juce::Justification::centredLeft);
            heading.setColour (juce::Label::textColourId, juce::Colours::black);
            heading.setFont (juce::Font (juce::FontOptions (21.0f).withStyle ("Bold")));
            addAndMakeVisible (heading);
            listHead.setJustificationType (juce::Justification::centredLeft);
            listHead.setColour (juce::Label::textColourId, inkFaint());
            listHead.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
            addAndMakeVisible (listHead);
            // Points at the partial list when On is on but nothing is routed yet. It used
            // to be the ON pill's green, which made it read as a state read-out rather
            // than as an instruction; it is the panel's hue, like every other caption.
            pickHint.setJustificationType (juce::Justification::centred);
            pickHint.setColour (juce::Label::textColourId, hue());
            pickHint.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
            addChildComponent (pickHint);
            list.setRowHeight (20);
            list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff141420));
            addAndMakeVisible (list);
            onT.setName ("onled"); // big green(on)/red(off) pill, drawn by GalacticLnF
            addAndMakeVisible (onT);
            selfT.setName ("onled"); // second big pill: "play in Fibló" (interpret internally)
            addAndMakeVisible (selfT);
            // Captions ABOVE the two big pill toggles - and they are CAPTIONS, which is
            // the whole of this change.
            //
            // "TWO WHITE BOLD SUB-LABELS UNDER A SMALL DIM GROUP TITLE, SO THE LESS
            // IMPORTANT THING IS LOUDER." Measured off the rig's photograph at 1200x650,
            // and true: "MIDI out (to DAW)" and "Play in Fibló" were inkDim (0xb9c0d0,
            // near white on this ground) while "PLAYBACK" above them was the panel's olive
            // - which is darker - and the two were exactly the same SIZE, so nothing but a
            // dim colour separated the heading of the group from the caption of one switch
            // inside it. The panel read from the inside out.
            //
            // AND THE SIZES IN THIS FILE ARE NOT THE SIZES ON THE GLASS, which is why the
            // first attempt at this changed nothing at all. GalacticLnF::getLabelFont puts
            // a floor under every Label - jmax (12.5, the font it was given), Bold - so
            // the 11.0f here and the 11.5f on these two were BOTH drawn at 12.5 Bold and
            // moving either of them below the floor moves nothing. Only two things
            // actually reach the screen: a font ABOVE 12.5 (what the group titles now
            // ask for), or the "small" escape hatch the LnF documents, which returns the
            // Label's own font untouched. Both are used here, in opposite directions.
            //
            // So these two join the ramp they belong to - 11.0f Bold in inkFaint is what
            // "VEL", "GATE", "Steps from the note" and "Partials (click to route)" are
            // already set in a few rows below, and those are the same kind of object: a
            // word naming the control under it. The order on the glass is now title (13.0
            // Bold, the panel's identity colour) > caption (11.0 Bold, the faint end of
            // the text ramp), with the pill itself - a saturated fill with white letters -
            // the loudest thing in the group, because it is the control.
            //
            // NOT SOLVED BY DIMMING ALONE, which was tried: with both drawn at 12.5 the
            // title wins only on colour, and olive-on-charcoal against grey-on-charcoal is
            // a difference a reader has to go looking for. Size is the cue that does not
            // need looking for.
            for (auto* l : { &outLbl, &selfLbl })
            {
                l->setJustificationType (juce::Justification::centred);
                l->setColour (juce::Label::textColourId, inkFaint());
                // Out of the 12.5 px floor - see the note above. The side effect is that
                // probeNameFor() reports both of them as "small", which is the same
                // trade the nine prime-number labels already make; the alternative was a
                // caption that cannot be made smaller than the title it sits under.
                l->setName ("small");
                l->setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
                addAndMakeVisible (*l);
            }
            droneT.setColour (juce::ToggleButton::textColourId, inkDim());
            addAndMakeVisible (droneT);
            // Internal feedback loop + its volume slope (only meaningful with self-play).
            feedbackT.setColour (juce::ToggleButton::textColourId, inkDim());
            addAndMakeVisible (feedbackT);
            feedbackLbl.setJustificationType (juce::Justification::centredRight);
            feedbackLbl.setColour (juce::Label::textColourId, inkDim());
            feedbackLbl.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
            addAndMakeVisible (feedbackLbl);
            feedbackS.setSliderStyle (juce::Slider::LinearHorizontal);
            feedbackS.setRange (0.05, 0.95, 0.01);
            feedbackS.setDoubleClickReturnValue (true, 0.5); // double-click = default
            feedbackS.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 16);
            feedbackS.textFromValueFunction = [] (double v) { return juce::String ((int) std::lround (v * 100.0)) + " %"; };
            // Track and thumb in the panel's hue - the three faders in this window used to
            // be pink, pink and cyan for no reason a reader could name.
            feedbackS.setColour (juce::Slider::trackColourId, hue());
            feedbackS.setColour (juce::Slider::thumbColourId, hue());
            addAndMakeVisible (feedbackS);
            addAndMakeVisible (mpeT);
            modeBox.addItem ("Lowest note", 1); // idx-mapped to the rhythmMode parameter
            modeBox.addItem ("All notes",   2);
            addAndMakeVisible (modeBox);

            // FIVE GROUP HEADERS, ONE COLOUR. They were five different hues, which is
            // what "PULSE RATE and NOTE START are unrelated but share cyan" was really a
            // symptom of: with five colours and no rule saying what a colour MEANS, any
            // two of them landing near each other is an accident that reads as a claim.
            // What tells the groups apart is their words and their frames, which is how
            // every other panel in the product does it.
            //
            // ...AND THEY ARE THE BIGGEST WORDS IN THEIR OWN GROUP NOW. A group title that
            // is no bigger than the caption of one switch inside it is not a title - see
            // the long note on outLbl / selfLbl above for what was photographed and for
            // why 11.0f here used to draw at 12.5.
            //
            // 13.0f is the smallest number that actually MOVES: anything at or under the
            // LnF's 12.5 px floor is the size these were already drawn at. It is half a
            // point over the floor, not two - a title has to outrank its captions, not
            // shout at them, and every pixel of it is paid for in kSecHdrH.
            for (auto* l : { &secPlay, &secRate, &secOut, &secTempo, &secGate })
            {
                l->setJustificationType (juce::Justification::centredLeft);
                l->setColour (juce::Label::textColourId, hue());
                l->setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
                addAndMakeVisible (*l);
            }
            for (auto* t : { &qTempoT, &monoT, &qPulseT })
            {
                t->setColour (juce::ToggleButton::textColourId, inkDim());
                addAndMakeVisible (*t);
            }

            // "divide [n] times by [x]": two tiny +/- steppers driving the params.
            for (auto* l : { &divLbl, &timesLbl })
            {
                l->setColour (juce::Label::textColourId, inkDim());
                l->setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
                addAndMakeVisible (*l);
            }
            divLbl.setJustificationType (juce::Justification::centredLeft);  // flush to the left border
            timesLbl.setJustificationType (juce::Justification::centredRight);
            for (auto* v : { &nVal, &xVal })
            {
                v->setJustificationType (juce::Justification::centred);
                v->setColour (juce::Label::textColourId, ink());
                v->setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
                addAndMakeVisible (*v);
            }
            // A STEPPER PAIR IS ONE CONTROL, IN THE PANEL'S COLOUR - the same rule the
            // editor's OCTAVE and OCTAVE RANGE pairs follow (see configStep in
            // PluginEditor.cpp). These were green for "+" and red for "-", which are this
            // product's LOAD and DESTROY colours: an increment painted in the colour of the
            // X that deletes a routed slot, two rows below it.
            //
            // ...AND THEN THEY WERE TWO COLOURS FOR ONE CONTROL, WHICH IS THE SAME FAULT
            // ONE STEP ON. The fix above said "its SECTION's colour" and passed a hue in
            // per call: cyan 0x4fe6ff for the PULSE RATE pair, pink 0xff77c8 for the OUTPUT
            // one. That was written while this panel still had five section hues. It has
            // had ONE since (see kHueRgb above - the five headers, the five frames, every
            // fader track and every read-out are the spectrum's olive), so those two
            // literals were the last of the old scheme, and what a reader saw was the
            // IDENTICAL round -/+ pair drawn teal in one group and magenta in another,
            // 130 px apart in the same dialog. Two colours for one control is a claim that
            // they are two kinds of control; there was nothing left for the colours to
            // mean. `col` is gone with them - a per-call hue is exactly the door the drift
            // came through, so the pairs can no longer disagree even by accident.
            auto miniStep = [this] (juce::TextButton& b, const char* txt, const char* id,
                                    int delta)
            {
                const auto col = hue();
                b.setButtonText (txt);
                b.setName ("round");
                b.setColour (juce::TextButton::buttonColourId, col.withAlpha (0.45f));
                b.onClick = [this, id, delta]
                {
                    if (proc == nullptr) return;
                    if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc->apvts.getParameter (id)))
                    {
                        const float cur = proc->apvts.getRawParameterValue (id)->load();
                        p->setValueNotifyingHost (p->convertTo0to1 (cur + (float) delta));
                    }
                    refreshLive();
                };
                addAndMakeVisible (b);
            };
            // "divide n times by x", under the PULSE RATE heading.
            miniStep (nUp, "+", "rhythmDivN",  1);
            miniStep (nDn, "-", "rhythmDivN", -1);
            miniStep (xUp, "+", "rhythmDivX",  1);
            miniStep (xDn, "-", "rhythmDivX", -1);

            // "How to route" ON A SATURATED AMBER FILL WAS THE BRIGHTEST THING IN THE
            // DIALOG - louder than the panel's own name, louder than the two switches
            // that decide whether it does anything at all. A help button is the least
            // urgent control in any window: it is what you reach for when you are stuck,
            // and it only has to be findable. It is the same quiet slate as the X beside
            // it now (they are the same kind of thing - window furniture, not content),
            // and its word comes out white, so it is perfectly legible and no longer
            // shouting. (White is not set here: GalacticLnF::drawButtonText derives the
            // letter colour from the fill's own brightness - which is exactly why the
            // amber version had BLACK letters on a shouting yellow slab.)
            helpB.setButtonText ("How to route");
            helpB.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff44445e));
            helpB.onClick = [this]
            {
                juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                    helpTitle, helpText, this);
            };
            addAndMakeVisible (helpB);

            // JI pitches TOGGLE: when ON, every routed pulse (present and future) plays
            // the JUST ratio of ITS OWN partial (P3 -> 3/1...), sent retuned via MPE, so
            // the receiver plays the partials' exact pitches. Green when on, red when off.
            // Turning it on auto-enables MPE out (JI needs per-note bend); off reverts
            // the slots to scale steps.
            jiB.setClickingTogglesState (true);
            jiB.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xffe0453f)); // off = red
            jiB.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2fb155)); // on  = green
            jiB.setColour (juce::TextButton::textColourOffId,  juce::Colours::white);
            jiB.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
            jiB.onClick = [this]
            {
                if (proc == nullptr) return;
                const bool on = jiB.getToggleState();
                if (on)
                {
                    // JI needs per-note bend -> MPE out goes on with it. Remember
                    // whether WE were the ones who switched it on, so turning JI back
                    // off leaves the panel exactly as it found it. Without this the
                    // toggle was not an involution: MPE stayed on for ever after, and
                    // its channel spreading kept detuning notes that shared a channel.
                    setJiForcedMpe (! mpeOn());
                    if (jiForcedMpe())
                        if (auto* p = proc->apvts.getParameter ("rhythmMpe"))
                            p->setValueNotifyingHost (1.0f);
                }
                else if (jiForcedMpe())
                {
                    setJiForcedMpe (false);
                    if (auto* p = proc->apvts.getParameter ("rhythmMpe"))
                        p->setValueNotifyingHost (0.0f);
                }
                if (auto* p = proc->apvts.getParameter ("rhythmJi")) // persist the switch itself
                    p->setValueNotifyingHost (on ? 1.0f : 0.0f);
                applyJiToSlots();
                updateJiText();
                refreshSlots();
            };
            addAndMakeVisible (jiB);

            // DAW-tempo sync: while the TOGGLE is on, the editor timer keeps sending
            // CC20 whenever the target BPM changes (see the editor's timerCallback) -
            // a MIDI-mapped host tempo then FOLLOWS pulse 1 continuously.
            bpmLbl.setJustificationType (juce::Justification::centredLeft);
            bpmLbl.setColour (juce::Label::textColourId, hue());
            bpmLbl.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
            addAndMakeVisible (bpmLbl);
            addAndMakeVisible (tempoT);
            for (auto* b : { &tempoX2, &tempoD2 })
            {
                b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff44445e));
                addAndMakeVisible (*b);
            }
            tempoX2.setButtonText ("x2");
            tempoD2.setButtonText ("/2");
            // The octave scaling is a PARAMETER now (the processor drives the tempo,
            // so it has to be reachable from there - and it survives a reload).
            tempoX2.onClick = [this] { nudgeTempoOct (+1); };
            tempoD2.onClick = [this] { nudgeTempoOct (-1); };
            info.setJustificationType (juce::Justification::topLeft);
            info.setColour (juce::Label::textColourId, hue());
            info.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
            addChildComponent (info);   // hidden while nothing is routed - see resized()
            for (auto* h : { &noteHead, &durHead, &velHead })
            {
                h->setJustificationType (juce::Justification::centredLeft);
                h->setColour (juce::Label::textColourId, inkFaint());
                h->setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
                addAndMakeVisible (*h);
            }
            // Velocity SLOPE: a tilt over the whole column of per-pulse velocities.
            // 0 = flat; up = the high partials fade out first, down = the low ones;
            // at the top of its travel the ramp is vertical and nothing is left.
            velSlopeS.setSliderStyle (juce::Slider::LinearHorizontal);
            velSlopeS.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            velSlopeS.setColour (juce::Slider::trackColourId, hue());
            velSlopeS.setColour (juce::Slider::thumbColourId, hue());
            velSlopeS.setDoubleClickReturnValue (true, 0.0); // flat: the centre of its travel
            velSlopeS.setMouseClickGrabsKeyboardFocus (false);
            addAndMakeVisible (velSlopeS);
            // Global STEPS offset for step-pitched pulses (ratio/JI slots ignore it).
            //
            // IT IS A CENTRE-DETENT FADER, AND ITS THUMB SITTING MID-TRACK WHILE THE
            // READ-OUT SAYS 0 IS CORRECT - which is worth writing down, because it looks
            // exactly like a value/paint mismatch and was reported as one. The parameter
            // is rhythmStepsAll, NormalisableRange<float> (-24, +24, 1) with a default of
            // 0 (PluginProcessor.cpp), so 0 is the MIDDLE of the range, not the bottom:
            // the fader shifts every step-pitched pulse up OR down. The double-click
            // return makes that explicit and gives the centre back in one gesture -
            // without it the detent was real but invisible and unreachable.
            stepsAllS.setSliderStyle (juce::Slider::LinearHorizontal);
            stepsAllS.setTextBoxStyle (juce::Slider::TextBoxRight, false, 34, 15);
            stepsAllS.setColour (juce::Slider::trackColourId, hue());
            stepsAllS.setColour (juce::Slider::thumbColourId, hue());
            stepsAllS.setDoubleClickReturnValue (true, 0.0);
            stepsAllS.setMouseClickGrabsKeyboardFocus (false);
            addAndMakeVisible (stepsAllS);
            stepsAllLbl.setJustificationType (juce::Justification::centredRight);
            stepsAllLbl.setColour (juce::Label::textColourId, inkFaint());
            stepsAllLbl.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
            addAndMakeVisible (stepsAllLbl);

            closeB.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff44445e));
            closeB.onClick = [this] { if (onClose) onClose(); };
            addAndMakeVisible (closeB);

            // Scrollable slot-rows container ("unlimited" pulses).
            slotView.setViewedComponent (&slotRows, false);
            slotView.setScrollBarsShown (true, false);
            addAndMakeVisible (slotView);

            // Global octave stepper: shifts ALL pulses together (+-4 octaves).
            octLbl.setJustificationType (juce::Justification::centredRight);
            octLbl.setColour (juce::Label::textColourId, inkDim());
            octLbl.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
            addAndMakeVisible (octLbl);
            octAllVal.setJustificationType (juce::Justification::centred);
            octAllVal.setColour (juce::Label::textColourId, ink());
            octAllVal.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
            addAndMakeVisible (octAllVal);
            // "Octave (all)" sits in the OUTPUT row - same pair, same colour as the two above.
            miniStep (octAllUp, "+", "rhythmOctAll",  1);
            miniStep (octAllDn, "-", "rhythmOctAll", -1);

            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                slotLbl[i].setJustificationType (juce::Justification::centredLeft);
                slotLbl[i].setColour (juce::Label::textColourId, ink());
                slotLbl[i].setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
                slotRows.addChildComponent (slotLbl[i]);

                pitchE[i].setJustification (juce::Justification::centred);
                pitchE[i].setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
                pitchE[i].setColour (juce::TextEditor::outlineColourId, hue (0.55f));
                pitchE[i].setColour (juce::TextEditor::textColourId, juce::Colours::white);
                pitchE[i].setSelectAllWhenFocused (true);
                pitchE[i].onReturnKey = [this, i] { commitPitch (i); if (onEdited) onEdited(); };
                pitchE[i].onFocusLost = [this, i] { commitPitch (i); };
                slotRows.addChildComponent (pitchE[i]);

                // -12 / +12 step an OCTAVE of scale steps; they only apply to steps
                // values (hidden when the slot holds a ratio/cents pitch).
                for (auto* b : { &s12M[i], &s12P[i] })
                {
                    b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff33334a));
                    slotRows.addChildComponent (*b);
                }
                s12M[i].setButtonText ("-12");
                s12P[i].setButtonText ("+12");
                auto step12 = [this, i] (int delta)
                {
                    if (proc == nullptr) return;
                    auto& s = proc->getRhythm().slots[i];
                    if (s.useRatio.load()) return; // steps only
                    s.steps = juce::jlimit (-127, 127, s.steps.load() + delta);
                    refreshSlots();
                };
                s12M[i].onClick = [step12] { step12 (-12); };
                s12P[i].onClick = [step12] { step12 ( 12); };

                durS[i].setSliderStyle (juce::Slider::LinearHorizontal);
                durS[i].setRange (5.0, 500.0, 1.0);
                durS[i].setDoubleClickReturnValue (true, 60.0); // double-click = default
                durS[i].setSkewFactorFromMidPoint (80.0);
                durS[i].setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 16);
                durS[i].textFromValueFunction = [] (double v) { return juce::String ((int) v) + " ms"; };
                // GATE and VEL are two columns of the same table, so they are one colour.
                // GATE used to take the LookAndFeel's default steel and VEL was pink,
                // which said the two columns were different kinds of thing.
                durS[i].setColour (juce::Slider::trackColourId, hue());
                durS[i].setColour (juce::Slider::thumbColourId, hue());
                durS[i].onValueChange = [this, i]
                { if (proc) proc->getRhythm().slots[i].durMs = (float) durS[i].getValue(); };
                slotRows.addChildComponent (durS[i]);

                // Per-slot velocity: the MIDI velocity of every pulse this train sends.
                velS[i].setSliderStyle (juce::Slider::LinearHorizontal);
                velS[i].setRange (1.0, 127.0, 1.0);
                velS[i].setDoubleClickReturnValue (true, 100.0); // double-click = default
                velS[i].setTextBoxStyle (juce::Slider::TextBoxRight, false, 32, 16);
                velS[i].setColour (juce::Slider::trackColourId, hue());
                velS[i].setColour (juce::Slider::thumbColourId, hue());
                velS[i].onValueChange = [this, i]
                { if (proc) proc->getRhythm().slots[i].vel = (int) velS[i].getValue(); };
                slotRows.addChildComponent (velS[i]);

                delB[i].setButtonText ("X");
                delB[i].setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe0453f));
                delB[i].onClick = [this, i]
                {
                    auto v = readSlots();
                    if (i < (int) v.size()) v.erase (v.begin() + i);
                    writeSlots (std::move (v)); // stays compact, no gaps
                    refreshSlots();
                    list.repaint();
                };
                slotRows.addChildComponent (delB[i]);
            }
        }

        //-- ListBoxModel: the current template's partials --------------------------
        int getNumRows() override { return numRows; }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
        {
            const auto* t = proc != nullptr ? proc->getLiveTemplate() : nullptr;
            if (t == nullptr || row >= (int) t->partials.size()) return;
            const bool assigned = slotOfPartial (row) >= 0;
            // The "this one is routed" wash was a flat blue that belonged to nothing.
            // A routed partial is the panel's subject, so it is washed in the panel's own
            // hue over the list's ground - the same 0.22 the frames use for their fills,
            // which is what makes a routed row read as part of this panel and not as a
            // selection borrowed from a file browser.
            if (assigned) { g.setColour (hue (0.22f)); g.fillRect (0, 0, w, h); }
            g.setColour (inkFaint());
            g.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
            g.drawText ("P" + juce::String (row + 1), 6, 0, 40, h, juce::Justification::centredLeft);
            g.setColour (assigned ? juce::Colours::white : inkDim());
            g.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
            g.drawText (ratioName ? ratioName ((double) t->partials[(size_t) row].ratio) : juce::String(),
                        50, 0, w - 56, h, juce::Justification::centredLeft);
        }
        // The routed trains are kept COMPACT and in LIST ORDER (sorted by partial
        // index): no empty rows between them, no reordering surprises when a
        // partial is added or removed.
        struct SlotData { int partial, steps; bool useRatio; double ratio; float durMs; int vel; };
        std::vector<SlotData> readSlots() const
        {
            std::vector<SlotData> v;
            if (proc == nullptr) return v;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                const auto& s = proc->getRhythm().slots[i];
                if (s.partial.load() >= 0)
                    v.push_back ({ s.partial.load(), s.steps.load(), s.useRatio.load(),
                                   s.ratio.load(), s.durMs.load(), s.vel.load() });
            }
            return v;
        }
        void writeSlots (std::vector<SlotData> v)
        {
            if (proc == nullptr) return;
            std::sort (v.begin(), v.end(), [] (const SlotData& a, const SlotData& b) { return a.partial < b.partial; });
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                auto& s = proc->getRhythm().slots[i];
                if (i < (int) v.size())
                {
                    s.steps = v[(size_t) i].steps;  s.useRatio = v[(size_t) i].useRatio;
                    s.ratio = v[(size_t) i].ratio;  s.durMs    = v[(size_t) i].durMs;
                    s.vel   = v[(size_t) i].vel;
                    s.partial = v[(size_t) i].partial;
                    s.enabled = true;
                }
                else
                {
                    s.enabled = false; s.partial = -1;
                    s.steps = 0; s.useRatio = false; s.ratio = 1.0; s.durMs = 60.0f; s.vel = 100;
                }
            }
        }
        void listBoxItemClicked (int row, const juce::MouseEvent&) override
        {
            if (proc == nullptr) return;
            auto v = readSlots();
            const auto it = std::find_if (v.begin(), v.end(), [row] (const SlotData& d) { return d.partial == row; });
            if (it != v.end())
                v.erase (it);                            // click an assigned partial -> unassign
            else if ((int) v.size() < RhythmEngine::kSlots)
            {
                v.push_back ({ row, 0, false, 1.0, 60.0f, 100 }); // assign (writeSlots sorts + compacts)
                // Routing a partial means the user WANTS pulses -> make sure SOME output
                // is on. Self-play is the default sink (plays here); don't force MIDI-out.
                const bool anyOut = proc->apvts.getRawParameterValue ("rhythmSelfPlay")->load() > 0.5f
                                 || proc->apvts.getRawParameterValue ("rhythmOn")->load()       > 0.5f;
                if (! anyOut)
                    if (auto* p = proc->apvts.getParameter ("rhythmSelfPlay"))
                        p->setValueNotifyingHost (1.0f);
            }
            writeSlots (std::move (v));
            // Only the NEWLY routed partial adopts JI - existing slots keep any
            // hand-edited ratio (routing more partials must never reset them).
            if (jiB.getToggleState()) applyJiToSlots (row);
            refreshSlots();
            list.repaint();
        }
        int slotOfPartial (int p) const
        {
            if (proc == nullptr) return -1;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
                if (proc->getRhythm().slots[i].partial.load() == p) return i;
            return -1;
        }

        //-- Pitch spec of a slot ---------------------------------------------------
        // Plain: a note name ("A4"). MPE out on: also cents ("1987.4c") or ratios
        // ("3", "3/2") -> the exact pitch f0*ratio is sent via per-note bend.
        bool mpeOn() const
        {
            return proc != nullptr && proc->apvts.getRawParameterValue ("rhythmMpe")->load() > 0.5f;
        }
        // JI toggle: ON -> routed slots use the JUST ratio of their own partial;
        // OFF -> slots revert to scale steps. `onlyPartial >= 0` applies it to that
        // newly-routed partial ONLY, so routing another partial never resets a ratio
        // the user has hand-edited on an existing slot.
        void applyJiToSlots (int onlyPartial = -1)
        {
            if (proc == nullptr) return;
            const bool on = jiB.getToggleState();
            const auto* t = proc->getLiveTemplate();
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                auto& s = proc->getRhythm().slots[i];
                const int p = s.partial.load();
                if (p < 0 || (onlyPartial >= 0 && p != onlyPartial)) continue;
                if (on && t != nullptr && p < (int) t->partials.size())
                { s.ratio = (double) t->partials[(size_t) p].ratio; s.useRatio = true; }
                else if (! on)
                    s.useRatio = false;
            }
        }
        void updateJiText() { jiB.setButtonText (jiB.getToggleState() ? "JI: ON" : "JI: OFF"); }
        void commitPitch (int i)
        {
            if (proc == nullptr) return;
            auto& s = proc->getRhythm().slots[i];
            const auto t = pitchE[i].getText();
            if (const int st = parseSteps (t); st != std::numeric_limits<int>::min())
            { s.steps = st; s.useRatio = false; }
            else if (const double r = parseRatioSpec (t); r > 0.0)
            {
                if (mpeOn())
                { s.ratio = r; s.useRatio = true; }
                else
                {
                    // Ratios/cents need per-note pitch bend -> MPE out must be on.
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        // The TITLE went out in English while the body beside it changed language: a
                        // native box is not a Component, so nothing walks it.
                        trF ? trF (fgdlg::mpeOut.en, fgdlg::mpeOut.ca) : juce::String (fgdlg::mpeOut.en),
                        trF ? trF ("Ratio and cents pitches (\"3/2\", \"702c\") need MPE out enabled - they are sent retuned with per-note pitch bend. Enable \"MPE out\" or type scale steps (\"7st\").",
                                   "Els tons en ratio o cents (\"3/2\", \"702c\") necessiten MPE out actiu - s'envien reafinats amb pitch bend per nota. Activa \"MPE out\" o escriu passos d'escala (\"7st\").")
                            : juce::String ("Enable MPE out to use ratio/cents pitches."), this);
                }
            }
            refreshSlots(); // reverts the shown text to the stored value when rejected
        }
        int tempoOctValue() const
        {
            return proc != nullptr
                 ? (int) proc->apvts.getRawParameterValue ("rhythmTempoOct")->load() : 0;
        }
        void nudgeTempoOct (int d)
        {
            if (proc == nullptr) return;
            if (auto* p = proc->apvts.getParameter ("rhythmTempoOct"))
                p->setValueNotifyingHost (p->convertTo0to1 ((float) juce::jlimit (-6, 6, tempoOctValue() + d)));
            refreshLive();
        }
        // WHAT THE TEMPO READ-OUT SAYS BEFORE THERE IS A TEMPO.
        //
        // "= ... BPM" was a literal ellipsis standing in for a number, which reads as a
        // value that failed to arrive - the same thing a hung read-out looks like. There
        // is nothing broken here: this label reports the rate of pulse 1, and until a
        // note is held there IS no pulse 1, so the honest text is the reason and not a
        // gap. It says so, in words, in both languages.
        //
        // Read by resized() as well as by refreshLive(), because the box that holds this
        // label is sized to the WIDEST thing the label can ever say - and that is now
        // this sentence in Catalan, not "= 199.9 BPM". A placeholder nobody measured is
        // how the previous one got 13 px for a 52 px string.
        juce::String bpmIdleText() const
        {
            return trF ? trF ("no pulse yet", "cap pols encara") : juce::String ("no pulse yet");
        }
        double tempoBpm() const // pulse 1 (first enabled slot) in BPM, times 2^octave
        {
            if (proc == nullptr) return 0.0;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
                if (proc->getRhythm().slots[i].enabled.load())
                {
                    const double r = (double) proc->getRhythm().getRateHz (i);
                    if (r > 0.0) return r * 60.0 * std::exp2 ((double) tempoOctValue());
                }
            return 0.0;
        }

        //-- Refresh ----------------------------------------------------------------
        void refreshSlots()
        {
            const auto* t = proc != nullptr ? proc->getLiveTemplate() : nullptr;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                const int p = proc != nullptr ? proc->getRhythm().slots[i].partial.load() : -1;
                const bool used = p >= 0;
                const bool stepsPitch = used && ! proc->getRhythm().slots[i].useRatio.load();
                slotLbl[i].setVisible (used); pitchE[i].setVisible (used);
                durS[i].setVisible (used);    delB[i].setVisible (used);
                // +-12 steps only make sense for steps pitches - hidden for ratio/cents.
                s12M[i].setVisible (stepsPitch); s12P[i].setVisible (stepsPitch);
                velS[i].setVisible (used);
                if (used)
                {
                    auto& s = proc->getRhythm().slots[i];
                    juce::String rl;
                    // A routed partial that the CURRENT spectrum no longer has cannot
                    // pulse: say so on the row instead of leaving it looking active.
                    const bool missing = t != nullptr && p >= (int) t->partials.size();
                    if (t != nullptr && ! missing && ratioName)
                        rl = juce::String::fromUTF8 (" \xc2\xb7 ") + ratioName ((double) t->partials[(size_t) p].ratio);
                    else if (missing)
                        // The row is 96 px wide. " - not in this spectrum" measured 170
                        // in the font it is drawn in, so it was cut off mid-word - the
                        // user's own example - AND it was raw English in a bilingual
                        // product. One red word says the same thing and fits; the
                        // sentence is in the row's hover.
                        rl = trF ? " - " + trF ("missing", "no hi \xc3\xa9s") : juce::String (" - missing");
                    // Red stays red: "missing" is a fault, and red-means-broken is the
                    // product's semantic, not this panel's identity.
                    slotLbl[i].setColour (juce::Label::textColourId,
                                          missing ? juce::Colour (0xffe0453f) : ink());
                    slotLbl[i].setText ("P" + juce::String (p + 1) + rl, juce::dontSendNotification);
                    slotLbl[i].setTooltip (missing && trF
                        ? trF ("This train is routed to a partial the CURRENT spectrum does not have, so it "
                               "cannot pulse. Route it to a partial that exists, or load the spectrum it came from.",
                               "Aquest tren est\xc3\xa0 encaminat a un parcial que l'espectre ACTUAL no t\xc3\xa9, "
                               "aix\xc3\xad que no pot polsar. Encamina'l a un parcial que existeixi, o carrega "
                               "l'espectre d'on venia.")
                        : juce::String());
                    if (! pitchE[i].hasKeyboardFocus (false))
                        pitchE[i].setText (s.useRatio.load() && ratioName
                                               ? ratioName (s.ratio.load())
                                               : stepsText (s.steps.load()),
                                           juce::dontSendNotification);
                    durS[i].setValue ((double) s.durMs.load(), juce::dontSendNotification);
                    velS[i].setValue ((double) s.vel.load(), juce::dontSendNotification);
                }
            }
            // Guide the user the moment they switch On with nothing routed yet.
            const bool on = proc != nullptr
                && (proc->apvts.getRawParameterValue ("rhythmOn")->load()       > 0.5f
                 || proc->apvts.getRawParameterValue ("rhythmSelfPlay")->load() > 0.5f);
            const bool none = readSlots().empty();
            pickHint.setVisible (on && none);

            // ROUTING THE FIRST PARTIAL CHANGES THE LAYOUT, NOT JUST THE ROWS.
            //
            // The empty state and the routed state are two different arrangements of this
            // pane - one centred hint, or four column headers plus the velocity-slope
            // fader plus the read-out - and which components exist at all is decided in
            // resized(). Clicking a partial only ever called this function, so the
            // headers and the slope fader stayed hidden (and unpositioned) until
            // something else happened to resize the panel: a language switch, or the host
            // resizing the editor. It was invisible while the read-out was always on,
            // because the read-out filled the space the headers should have been in; with
            // the read-out now part of the routed arrangement it would have been the
            // whole bottom of the pane going missing. One layout pass on the flip, and
            // only on the flip - this runs on every timer tick.
            if (const bool anyRouted = ! none; anyRouted != lastAnyRouted)
            {
                lastAnyRouted = anyRouted;
                resized();
                return;           // resized() has just called layoutSlotRows() itself
            }
            layoutSlotRows(); // the used-row count may have changed
        }

        void refreshLive() // pulse rates + polyrhythm chain, from the audio thread's read-outs
        {
            if (proc == nullptr) return;
            nVal.setText (juce::String ((int) proc->apvts.getRawParameterValue ("rhythmDivN")->load()),
                          juce::dontSendNotification);
            xVal.setText (juce::String ((int) proc->apvts.getRawParameterValue ("rhythmDivX")->load()),
                          juce::dontSendNotification);
            octAllVal.setText (juce::String ((int) proc->apvts.getRawParameterValue ("rhythmOctAll")->load()),
                               juce::dontSendNotification);
            juce::String s;
            const float f0 = proc->getRhythm().getDriverF0();
            s << (f0 > 0.0f ? "f0 = " + juce::String (f0, 2) + " Hz\n"
                            : juce::String (trF ? trF ("(hold a note to pulse)", "(mant\xc3\xa9n una nota per polsar)") : ""))
              ;
            float rates[RhythmEngine::kSlots]; int idx[RhythmEngine::kSlots]; int m = 0;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                const float r = proc->getRhythm().getRateHz (i);
                if (r > 0.0f) { rates[m] = r; idx[m] = i; ++m; }
            }
            for (int k = 0; k < m && k < 4; ++k) // cap the listing; the polyrhythm line summarises the rest
                s << "P" << juce::String (proc->getRhythm().slots[idx[k]].partial.load() + 1)
                  << ": " << juce::String (rates[k], 3) << " Hz ("
                  << juce::String (1000.0f / rates[k], 0) << " ms)\n";
            if (m > 4)
                s << "+" << juce::String (m - 4) << (trF ? trF (" more trains\n", " trens m\xc3\xa9s\n") : juce::String ("\n"));
            if (m >= 2)
            {
                // ONE integer chain "1:2:3" over all active trains (not pairwise):
                // rationalise every rate against the slowest, then scale to the
                // least common denominator and reduce.
                double base = rates[0];
                for (int k = 1; k < m; ++k) base = juce::jmin (base, (double) rates[k]);
                long long num[RhythmEngine::kSlots], den[RhythmEngine::kSlots], L = 1;
                bool ok = base > 0.0;
                if (ok)
                    for (int k = 0; k < m; ++k)
                    {
                        approxFraction ((double) rates[k] / base, num[k], den[k]);
                        if (den[k] <= 0) { ok = false; break; }
                        L = std::lcm (L, den[k]);
                    }
                if (ok)
                {
                    long long chain[RhythmEngine::kSlots], g = 0;
                    for (int k = 0; k < m; ++k) { chain[k] = num[k] * (L / den[k]); g = std::gcd (g, chain[k]); }
                    std::sort (chain, chain + m);
                    s << (trF ? trF ("polyrhythm ", "poliritme ") : juce::String ("polyrhythm "));
                    for (int k = 0; k < m; ++k)
                        s << juce::String (g > 0 ? chain[k] / g : chain[k]) << (k + 1 < m ? ":" : "");
                }
            }
            info.setText (s, juce::dontSendNotification);

            // Guide the user the moment they switch On with nothing routed yet
            // (updated here, every tick, so it reacts to the On toggle too).
            const bool onNow = proc->apvts.getRawParameterValue ("rhythmOn")->load()       > 0.5f
                            || proc->apvts.getRawParameterValue ("rhythmSelfPlay")->load() > 0.5f;
            pickHint.setVisible (onNow && readSlots().empty());

            const double bpm = tempoBpm();
            bpmLbl.setText (bpm > 0.0 ? "= " + juce::String (bpm, 1) + " BPM"
                                      : bpmIdleText(),
                            juce::dontSendNotification);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (14, 10);
            auto top = r.removeFromTop (30);
            closeB.setBounds (top.removeFromRight (24).withSizeKeepingCentre (22, 22));
            top.removeFromRight (5);
            {   // "How to route" / "Com encaminar" - the Catalan is 6 px longer.
                const int hw = juce::jmax (104, fgfit::textButtonWidth (helpB.getButtonText(), 22, false));
                helpB.setBounds (top.removeFromRight (hw).withSizeKeepingCentre (hw, 22));
            }
            heading.setBounds (top); // big title, no toggle inside it
            r.removeFromTop (6);

            // FIVE FRAMED SECTIONS, all treated alike. This panel used to be a stack:
            // two big pills with no group at all, a feedback row, then ONE framed box
            // (PULSE RATE) and three sections that were a coloured word over a hairline.
            // A reader had no way to tell which control belonged with which. Each group
            // gets a frame now - all five in the panel's one colour (see kHueRgb).
            //
            // Every section used to take the full row whatever it held, so PULSE RATE -
            // two captions and six stepper buttons, 272 px of controls - was drawn as a
            // framed box a thousand pixels wide with seven hundred of nothing in it. A
            // frame is a claim about what belongs together; a frame around empty space
            // claims there is something there. So each box is trimmed to what its own
            // controls measure to... which is where the NEXT problem came from:
            //
            // A BOX ENDS WHERE ITS CONTENTS END - SO THE ROW HAS TO END SOMEWHERE ELSE.
            //
            // "THE LEFT COLUMN'S THREE RIGHT EDGES ARE 687 / 478 / 535 - A STAIRCASE."
            // Measured from the render and true: PLAYBACK stopped at 687, PULSE RATE at
            // 478, DAW TEMPO at 535, and OUTPUT ran the full width to 996. Four different
            // right-hand edges down one stack, in no order, none of them explained.
            //
            // The staircase is not a bug in the trimming - it is what trimming ALONE
            // produces. Every box measures to a different width because every box holds
            // different controls, so trimming each to its own contents guarantees as many
            // edges as there are boxes. The two ways out were named by the user when the
            // first version of this was reviewed: "RELLENALO COMO DAW TEMPO / NOTE START,
            // O ACORTALAS" - fill it the way that pair does, or shorten them. Shortening
            // is what produced the staircase, so this takes the other one.
            //
            // PLAYBACK and PULSE RATE now SHARE A ROW, exactly as DAW TEMPO and NOTE
            // START already do: the first is trimmed to its own contents, the second
            // takes the remainder, and the row ends flush with the panel. With OUTPUT and
            // the DAW TEMPO / NOTE START row already reaching that same edge, the stack
            // has ONE right edge instead of four. The old middle row disappears with it,
            // which hands 53 px (a 48 px box and its 5 px gap) straight down to the
            // routed-partials list - about two more rows of it, permanently.
            //
            // WHEN IT CANNOT PAY FOR THE PAIR IT STACKS, which is the same rule the
            // DAW TEMPO / NOTE START pair below already lives by, and for the same reason:
            // in Catalan "Feedback fractal" + "Decaiment" + the fader measure ~505 and
            // "divideix ... vegades per" measures ~310, and 505 + 6 + 310 is over the 792
            // this panel has to give. Two boxes one above the other cost 53 px of height;
            // a squeezed box costs a word. The English render is flush; the Catalan one
            // keeps the arrangement it shipped with rather than crushing anything.
            //
            // The other half of the same finding - "OUTPUT is about 5 px taller than its
            // siblings" - is a measurement error in the audit, not in the layout: every
            // short box here is 13 + 3 + 24 + 8 = 48 px by construction (kShortH below),
            // OUTPUT included, and they are all built by the same helper from the same
            // constant. What differs is the INK: OUTPUT is the only short box holding a
            // ComboBox, which draws its own 1 px outline hard against the 24 px content
            // row where a ToggleButton leaves its edges bare, so the box reads a couple
            // of pixels deeper than the identical box above it.
            constexpr int kBoxPad = 16;                          // the 8 px a side of `reduced (8, 4)`
            constexpr int kShortH = kSecHdrH + 3 + 24 + 8;       // header + one 24 px control row
            constexpr int kPlayH  = kSecHdrH + 3 + (13 + 26 + 4 + 24) + 8;
            const auto sectionIn = [] (juce::Rectangle<int> box, juce::Label& hdr,
                                       juce::Rectangle<int>& boxOut, int contentH)
            {
                boxOut = box;
                auto in = box.reduced (8, 4);
                hdr.setBounds (in.removeFromTop (kSecHdrH));
                in.removeFromTop (3);
                return in.removeFromTop (contentH);
            };
            // Trimmed to its contents - floored so a narrow panel never produces a
            // sliver, and capped at the row so it can only ever shrink.
            const auto trim = [&] (juce::Rectangle<int> row, int contentW)
            {
                return row.withWidth (juce::jlimit (juce::jmin (200, row.getWidth()),
                                                    row.getWidth(), contentW + kBoxPad));
            };

            // WHAT THE TWO TOP GROUPS MEASURE TO, both up front: the packing below is one
            // decision about both of them, so neither can be measured after the other has
            // already been given a rectangle.
            const int w1 = juce::jmax (118, fgfit::labelWidth (outLbl));
            const int w2 = juce::jmax (140, fgfit::labelWidth (selfLbl));
            // PLAYBACK holds two rows and takes the WIDER of them: the pills row (two
            // captioned pills and the drone toggle) and the feedback row (its toggle, its
            // caption and a 240 px fader).
            const int playNeed = juce::jmax (w1 + 14 + w2 + 16 + fgfit::toggleWidth (droneT),
                                             juce::jmax (160, fgfit::toggleWidth (feedbackT))
                                                 + 8 + juce::jmax (54, fgfit::labelWidth (feedbackLbl))
                                                 + 6 + 240);
            // PULSE RATE is two captions and six stepper buttons. That is the whole section.
            const int dw  = juce::jmax (50, fgfit::labelWidth (divLbl));
            const int tw2 = juce::jmax (66, fgfit::labelWidth (timesLbl));
            const int rateNeed = dw + 4 + 66 + tw2 + 4 + 66;
            const bool pairTop = r.getWidth() >= playNeed + kBoxPad + 6 + rateNeed + kBoxPad;

            // PLAYBACK: where the pulses go, and what comes back. The two pills carry
            // their caption ABOVE them now - a pill that says "OFF" over the words "MIDI
            // out (to DAW)" reads as an answer before its question, and every other
            // caption in this build sits above the control it names.
            const auto layPlay = [&] (juce::Rectangle<int> box)
            {
                auto in = sectionIn (box, secPlay, playBox, 13 + 26 + 4 + 24);
                auto capRow = in.removeFromTop (13);
                auto pills  = in.removeFromTop (26);
                outLbl.setBounds (capRow.removeFromLeft (w1));
                capRow.removeFromLeft (14);
                selfLbl.setBounds (capRow.removeFromLeft (w2));
                onT.setBounds (pills.removeFromLeft (w1).withSizeKeepingCentre (juce::jmin (w1, 96), 24));
                pills.removeFromLeft (14);
                selfT.setBounds (pills.removeFromLeft (w2).withSizeKeepingCentre (juce::jmin (w2, 108), 24));
                pills.removeFromLeft (16);
                droneT.setBounds (pills.withSizeKeepingCentre (pills.getWidth(), 24));
                in.removeFromTop (4);
                auto fr = in.removeFromTop (24);
                feedbackT.setBounds (fr.removeFromLeft (juce::jmax (160, fgfit::toggleWidth (feedbackT))));
                fr.removeFromLeft (8);
                feedbackLbl.setBounds (fr.removeFromLeft (juce::jmax (54, fgfit::labelWidth (feedbackLbl))));
                fr.removeFromLeft (6);
                feedbackS.setBounds (fr.removeFromLeft (juce::jmin (240, fr.getWidth())).reduced (0, 1));
            };
            const auto layRate = [&] (juce::Rectangle<int> box)
            {
                auto g1 = sectionIn (box, secRate, rateBox, 24);
                divLbl.setBounds (g1.removeFromLeft (dw));
                g1.removeFromLeft (4);
                nDn.setBounds (g1.removeFromLeft (20).reduced (0, 2));
                nVal.setBounds (g1.removeFromLeft (26));
                nUp.setBounds (g1.removeFromLeft (20).reduced (0, 2));
                timesLbl.setBounds (g1.removeFromLeft (tw2));
                g1.removeFromLeft (4);
                xDn.setBounds (g1.removeFromLeft (20).reduced (0, 2));
                xVal.setBounds (g1.removeFromLeft (26));
                xUp.setBounds (g1.removeFromLeft (20).reduced (0, 2));
            };

            {
                auto row = r.removeFromTop (kPlayH);
                if (pairTop)
                {
                    layPlay (row.removeFromLeft (playNeed + kBoxPad));
                    row.removeFromLeft (6);
                    // TOP-ALIGNED, AT ITS OWN HEIGHT, not stretched to the row. PLAYBACK
                    // is 91 px because it has two control rows and PULSE RATE has one; a
                    // 48 px group blown up to 91 would be a frame around 43 px of nothing,
                    // which is the thing this whole section is arranged to avoid. Sharing
                    // the row's TOP edge puts the two group captions on one baseline,
                    // which is the alignment a reader actually sees.
                    layRate (row.withHeight (kShortH));
                }
                else
                    layPlay (trim (row, playNeed));
            }
            r.removeFromTop (5);
            if (! pairTop)
            {
                layRate (trim (r.removeFromTop (kShortH), rateNeed));
                r.removeFromTop (5);
            }

            // OUTPUT keeps a row of its own: its widest state is the mode box + MPE +
            // JI + an octave stepper + a steps fader, which is more than half a panel -
            // and it is the one box that fills the row it is given, so it is what the
            // other boxes' shared right edge is measured against.
            {
                auto g2 = sectionIn (r.removeFromTop (kShortH), secOut, outBox, 24);
                // MEASURED, NOT 128. Its widest item is "Totes les notes", which needs 112
                // px of text room and had 91 once the chevron gutter came off a flat 128 -
                // so the Catalan panel drew a condensed mode name while the English one
                // ("All notes", 62 px) sat comfortably. Floored at the old 128 so no window
                // that fits today gets narrower.
                modeBox.setBounds (g2.removeFromLeft (
                                       juce::jmax (128, fgfit::snugComboWidth (modeBox, 22))).reduced (0, 1));
                g2.removeFromLeft (8);
                mpeT.setBounds (g2.removeFromLeft (86));
                g2.removeFromLeft (8);
                jiB.setBounds (g2.removeFromLeft (100).reduced (0, 1));
                g2.removeFromLeft (10);
                octLbl.setBounds (g2.removeFromLeft (96));       // "Octave (all)"
                g2.removeFromLeft (4);
                octAllDn.setBounds (g2.removeFromLeft (20).reduced (0, 2));
                octAllVal.setBounds (g2.removeFromLeft (26));
                octAllUp.setBounds (g2.removeFromLeft (20).reduced (0, 2));
                g2.removeFromLeft (10);
                // Sized from its own word, floor 44. "Steps" fitted 44 px and "Passos"
                // does not - and until the language switch actually reached this caption
                // nobody could see that, because it never became "Passos" at all.
                stepsAllLbl.setBounds (g2.removeFromLeft (juce::jmax (44, fgfit::labelWidth (stepsAllLbl))));
                stepsAllS.setBounds (g2.removeFromLeft (juce::jmin (120, g2.getWidth())).reduced (0, 3));
            }
            r.removeFromTop (5);

            // DAW TEMPO and NOTE START, side by side WHEN THE ROW CAN PAY FOR IT.
            //
            // Both boxes state what they need, including the read-out at the end of the
            // tempo row - the first version of this sized the box and let "= ... BPM"
            // take the leftovers, which is how a 52 px label ended up in 44 and then in
            // 24, and it dragged the whole window's fully-legible size from 780x550 to
            // 1600x900. A label that gets what is left over is a label that disappears.
            //
            // When the row cannot hold both, they stack instead of squeezing. Two boxes
            // one above the other cost 41 px of height; a crushed read-out costs the
            // number it exists to show.
            {
                const int tempoNeed = juce::jmax (150, fgfit::toggleWidth (tempoT))
                                    + 4 + 32 + 4 + 32 + 6
                                    // ...and room for the read-out's WIDEST text, not its current one. This
                                    // label says "no pulse yet" until a tempo arrives and "= 120.0 BPM"
                                    // after, and resized() runs while it holds whichever is true at that
                                    // moment - so measuring what it happens to say reserved 13 px for a
                                    // string that needs 52. Same lesson as a ComboBox and its widest item:
                                    // measure what it CAN hold. BOTH strings, now that the placeholder is a
                                    // sentence rather than an ellipsis: in Catalan "cap pols encara" is the
                                    // longer of the two, so a box measured on the number alone would cut
                                    // the words that replace it.
                                    + juce::jmax ((int) std::ceil (juce::GlyphArrangement::getStringWidth (
                                                      fgfit::labelFont (bpmLbl), "= 199.9 BPM")),
                                                  (int) std::ceil (juce::GlyphArrangement::getStringWidth (
                                                      fgfit::labelFont (bpmLbl), bpmIdleText())))
                                    + bpmLbl.getBorderSize().getLeftAndRight() + 3 + 16;
                // MEASURED, not floored. The 218 and the 76 were hand-picked and carried
                // 34 px more than the words need, which is exactly what pushed this row
                // over and made the two boxes stack - costing three rows of the partial
                // list to pad two toggles nobody had measured.
                const int gateNeed  = fgfit::toggleWidth (qTempoT)
                                    + 6 + fgfit::toggleWidth (monoT)
                                    + 6 + fgfit::toggleWidth (qPulseT) + 16;
                const int boxH = kSecHdrH + 3 + 24 + 8;
                const bool sideBySide = r.getWidth() >= tempoNeed + 6 + gateNeed;

                auto layTempo = [&] (juce::Rectangle<int> box)
                {
                    tempoBox = box;
                    auto in = box.reduced (8, 4);
                    secTempo.setBounds (in.removeFromTop (kSecHdrH));
                    in.removeFromTop (3);
                    auto g3 = in.removeFromTop (24);
                    tempoT.setBounds (g3.removeFromLeft (juce::jmax (150, fgfit::toggleWidth (tempoT))));
                    g3.removeFromLeft (4);
                    tempoX2.setBounds (g3.removeFromLeft (32).reduced (0, 1));
                    g3.removeFromLeft (4);
                    tempoD2.setBounds (g3.removeFromLeft (32).reduced (0, 1));
                    g3.removeFromLeft (6);
                    bpmLbl.setBounds (g3);
                };
                auto layGate = [&] (juce::Rectangle<int> box)
                {
                    gateBox = box;
                    auto in = box.reduced (8, 4);
                    secGate.setBounds (in.removeFromTop (kSecHdrH));
                    in.removeFromTop (3);
                    auto g4 = in.removeFromTop (24);
                    // THE SPARE IS SHARED, NOT INHERITED BY WHOEVER IS LAST.
                    //
                    // "LAS CASILLAS DE NOTE START AHORA SE VEN CORTADAS." Measured, and
                    // they were: fgfit::toggleWidth returns the LookAndFeel's exact
                    // drawing budget, which leaves 2 px at the right and nothing more, so
                    // "Quantize to project tempo" got 165 px for the 162.88 it needs and
                    // "Mono" 36 for 33.19 - while the third toggle, taking the remainder,
                    // sat in 139 for 108.47. Nothing overflowed and nothing was clipped;
                    // two words simply ended flush against their own edge, which is what
                    // being cut looks like. Whatever the box has over the three needs is
                    // now split three ways.
                    const int n1 = fgfit::toggleWidth (qTempoT);
                    const int n2 = fgfit::toggleWidth (monoT);
                    const int n3 = fgfit::toggleWidth (qPulseT);
                    const int pad = juce::jmax (0, g4.getWidth() - (n1 + 6 + n2 + 6 + n3)) / 3;
                    qTempoT.setBounds (g4.removeFromLeft (n1 + pad));
                    g4.removeFromLeft (6);
                    monoT.setBounds (g4.removeFromLeft (n2 + pad));
                    g4.removeFromLeft (6);
                    qPulseT.setBounds (g4);
                };

                if (sideBySide)
                {
                    auto both = r.removeFromTop (boxH);
                    layTempo (both.removeFromLeft (tempoNeed));
                    both.removeFromLeft (6);
                    layGate (both);
                }
                else
                {
                    layTempo (r.removeFromTop (boxH));
                    r.removeFromTop (5);
                    layGate (r.removeFromTop (boxH));
                }
            }
            r.removeFromTop (8);

            // THE LIST IS AS WIDE AS ITS WIDEST ROW, not 236 px.
            //
            // 236 was a hand-picked number and the rows inside it are "P16" and a ratio -
            // about 110 px of ink at the default 1024-partial template, where the widest
            // row is "P1024  1024/1". Everything it did not need was taken from the
            // ROUTED rows on the other side, which are the crowded half: each one carries
            // a pitch box, two step buttons, a velocity fader, a gate fader and a delete.
            //
            // Its own header is the floor, because "Partials (click to route)" - and the
            // longer "Parcials (clica per encaminar)" - sits directly above it and a
            // column narrower than its own title is a cut title.
            int listW = 130;
            if (proc != nullptr)
                if (const auto* t = proc->getLiveTemplate())
                    if (! t->partials.empty() && ratioName)
                    {
                        const juce::Font rf (juce::FontOptions (12.5f).withStyle ("Bold"));
                        const auto widest = ratioName ((double) t->partials.back().ratio);
                        listW = 50 + (int) std::ceil (juce::GlyphArrangement::getStringWidth (rf, widest))
                              + 6 + 12;   // +12: the list's own scrollbar
                    }
            listW = juce::jlimit (110, 236,
                                  juce::jmax (listW, fgfit::labelWidth (listHead)));

            auto left = r.removeFromLeft (listW);
            listHead.setBounds (left.removeFromTop (16));
            list.setBounds (left);
            r.removeFromLeft (10);

            // THE EMPTY STATE IS A STATE, not a gap with two notes stuck in it.
            //
            // With nothing routed this whole pane is blank, and it used to carry two
            // small strings floating in different parts of it - a green "click a partial
            // to route it" pointing left, and a separate "(hold a note to pulse)" down in
            // the read-out - with four column headers above them naming columns that did
            // not exist yet. Three unrelated hints and a set of labels for nothing.
            //
            // The headers appear when there is a row to head. The hint gets the middle of
            // the pane it is talking about, centred, at the size of something meant to be
            // read rather than noticed.
            //
            // ...AND THE SECOND HINT IS GONE, which the first pass at this state left
            // behind. The pane still carried TWO strings: the centred "click a partial to
            // route it", and, 76 px below it in the read-out's box, a separate green
            // "(hold a note to pulse)" at a different x, a different weight and a
            // different size. Two hints, misaligned, saying different things about the
            // same emptiness - which is what the audit photographed and called "two hint
            // lines at different x, in different colours, at different weights".
            //
            // The read-out is a read-out OF THE ROUTED TRAINS: their rates in Hz, their
            // periods in ms, and the integer polyrhythm they make. With nothing routed
            // there are no trains, so every line it can print is empty and the only thing
            // left in it is a note about a note - advice, in the box where numbers live.
            // It is hidden until there is something to read out, and the empty state has
            // exactly one voice: the hint, in the middle of the pane it is about.
            const bool anyRouted = ! readSlots().empty();
            lastAnyRouted = anyRouted;   // this IS the arrangement that is now laid out
            for (auto* h : { &noteHead, &durHead, &velHead }) h->setVisible (anyRouted);
            velSlopeS.setVisible (anyRouted);
            info.setVisible (anyRouted);

            if (anyRouted)
            {
                auto head = r.removeFromTop (14);
                head.removeFromLeft (96);
                head.removeFromRight (28);        // over delB (24 + gap)
                durHead.setBounds (head.removeFromRight (104));
                head.removeFromRight (4);
                // The velocity SLOPE knob sits right above the VEL column it tilts.
                // THE SLOPE KNOB AND THE WORD SHARE THE ROW, side by side.
                //
                // The knob took the whole 76 px column and the word was pushed 14 px DOWN
                // to sit under it - but the header row IS 14 px tall, so "VEL" landed
                // inside the first data row, drawn on top of that row's velocity slider.
                // Its two sibling headers sit on the header line and it sat 14 px below
                // them, with a star thumb through the L, reading "VEI". Photographed in
                // the routed-rows scenario at 1200x650.
                //
                // Side by side instead of a taller header: the rows below are the point of
                // this list and a second header line costs one of them. The knob keeps
                // enough width to be grabbed and the word takes what it measures.
                auto velCol = head.removeFromRight (76);
                const int velWordW = juce::jmin (velCol.getWidth() - 24,
                                                 juce::jmax (26, fgfit::labelWidth (velHead)));
                velHead.setBounds (velCol.removeFromRight (velWordW));
                velCol.removeFromRight (2);
                velSlopeS.setBounds (velCol.withY (velCol.getY() - 1).withHeight (15));
                head.removeFromRight (4 + kStepBtnW + 3 + kStepBtnW + 6); // over the -12/+12 pair + gaps
                noteHead.setBounds (head);        // over the pitch box
            }
            else
            {
                pickHint.setBounds (r.withSizeKeepingCentre (r.getWidth(), 48));
            }

            // Scrollable slot rows: the viewport takes all space above the info
            // read-out ("unlimited" pulses - the panel shows a window onto them).
            auto infoArea = r.removeFromBottom (76);
            slotView.setBounds (r.reduced (0, 2));
            layoutSlotRows();
            infoArea.removeFromTop (4);
            info.setBounds (infoArea);
        }

        // Lay the slot rows out inside the scrollable container (called from
        // resized() AND refreshSlots(), since the used count changes the height).
        void layoutSlotRows()
        {
            int used = 0;
            if (proc != nullptr)
                for (int i = 0; i < RhythmEngine::kSlots; ++i)
                    if (proc->getRhythm().slots[i].partial.load() >= 0) ++used;
            const int rowH = 26;
            const bool scroll = used * rowH > slotView.getHeight();
            slotRows.setSize (juce::jmax (60, slotView.getWidth() - (scroll ? 10 : 0)),
                              juce::jmax (1, used * rowH));
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                auto row = juce::Rectangle<int> (0, i * rowH, slotRows.getWidth(), rowH).reduced (0, 2);
                slotLbl[i].setBounds (row.removeFromLeft (96));
                delB[i].setBounds (row.removeFromRight (24));
                row.removeFromRight (4);
                durS[i].setBounds (row.removeFromRight (104));
                row.removeFromRight (4);
                velS[i].setBounds (row.removeFromRight (76));
                row.removeFromRight (4);
                s12P[i].setBounds (row.removeFromRight (kStepBtnW));
                row.removeFromRight (3);
                s12M[i].setBounds (row.removeFromRight (kStepBtnW));
                row.removeFromRight (6);
                pitchE[i].setBounds (row.reduced (0, 1));
            }
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();

            // OPAQUE. THIS WAS A BUG, NOT A STYLE.
            //
            // The fill was 0xf2101018 - alpha 242/255, so 5% of whatever was behind came
            // through the panel. That is invisible over dark ground and glaring over
            // bright ground, and the main window has plenty of the latter: photographed
            // at 1200x650 the title bar's logo tiles were legible THROUGH the panel at
            // x 440-810, y 10-45, right beside the panel's own title, and the word
            // "TRANSITION" showed through at y 235. A modal you can read the window
            // through is not a modal - the reader cannot tell which layer a word is on.
            //
            // The opaque fill is the product's own panel interior: kPanelFill tinted 5%
            // towards this panel's colour, which is exactly what every macro panel in the
            // main window sits on (palette::panelSurface, the same function the contrast
            // checks measure against). So it is not just opaque, it is the SAME surface -
            // the panel now looks like it came out of the same window it is floating over.
            g.setColour (juce::Colour (0xff000000u | galactic::palette::panelSurface (kHueRgb)));
            g.fillRoundedRectangle (r, 10.0f);
            g.setColour (hue());
            g.drawRoundedRectangle (r.reduced (1.5f), 10.0f, 3.0f);

            // THE TITLE CHIP, painted under the heading Label (children paint after this).
            // The same object the main window's macro panels wear: a filled rectangle in
            // the panel's colour with the title knocked out of it in black. Bounded to the
            // text - a chip that runs to the edge of its row is a banner, not a chip - and
            // measured through fgfit like every other width in this file, so the Catalan
            // "ESPECTRE VS RITME" gets its own size rather than the English one's.
            if (! heading.getText().isEmpty() && ! heading.getBounds().isEmpty())
            {
                auto chip = heading.getBounds().toFloat()
                                .withWidth ((float) juce::jmin (heading.getWidth(),
                                                                fgfit::labelWidth (heading) + 6));
                g.setColour (hue());
                g.fillRoundedRectangle (chip, 4.0f);
            }

            // ONE FRAME PER GROUP - and now ONE COLOUR for all five, the panel's own.
            // They were five different hues (pale blue / cyan / pink / yellow / green),
            // which is four accent colours this window did not need and no reader could
            // decode: nothing said what pink MEANT, so PULSE RATE and NOTE START sharing
            // a cyan looked like a claim that they were related. The frame is what groups
            // the controls; the caption is what names the group. Neither needs a hue of
            // its own, and the panel reads as one thing now instead of five.
            for (const auto* box : { &playBox, &rateBox, &outBox, &tempoBox, &gateBox })
            {
                if (box->isEmpty()) continue;
                const auto rb = box->toFloat();
                g.setColour (hue (0.10f));
                g.fillRoundedRectangle (rb, 7.0f);
                g.setColour (hue (0.45f));
                g.drawRoundedRectangle (rb.reduced (0.5f), 7.0f, 1.2f);
            }
        }
    };

    // The selected envelope drawn as a LINE WITH NODES. Every node is draggable and
    // every leg between nodes can be bent by dragging it; both write straight into
    // the real parameters, so the curve on screen IS the signal the modulator sends
    // (ModEngine::shape uses the identical formula).
    struct EnvEditor : juce::Component, juce::TooltipClient
    {
        // Live values (times in seconds, level 0..1, curves -1..1). refresh() pulls
        // them from the parameters; a drag updates them immediately so the node
        // follows the mouse without waiting for a repaint from elsewhere.
        float a = 0.1f, d = 0.3f, s = 0.7f, r = 0.4f, cA = 0.0f, cD = 0.55f, cR = 0.55f;
        juce::Colour col { 0xff000000u | galactic::palette::kCustomMod }; // the CUSTOM modulator identity
        juce::String tip;
        std::function<void (const juce::String&, float)> onEdit;    // suffix, plain value
        std::function<void (const juce::String&, bool)>  onGesture; // suffix, starting
        std::function<float (const juce::String&)>       getParam;  // suffix -> plain value
        juce::String getTooltip() override { return tip; }

        void refresh()
        {
            if (! getParam) return;
            const float na = getParam ("A"),  nd = getParam ("D"),  ns = getParam ("S"),  nr = getParam ("R");
            const float ka = getParam ("CurveA"), kd = getParam ("CurveD"), kr = getParam ("CurveR");
            if (na == a && nd == d && ns == s && nr == r && ka == cA && kd == cD && kr == cR) return;
            a = na; d = nd; s = ns; r = nr; cA = ka; cD = kd; cR = kr;
            repaint();
        }

        static float shape (float t, float c)
        {
            t = juce::jlimit (0.0f, 1.0f, t);
            return std::pow (t, std::pow (2.0f, -3.0f * juce::jlimit (-1.0f, 1.0f, c)));
        }

        // Geometry. Times use a square-root scale so short times still get real estate.
        static constexpr float kMaxT = 5.0f, kPad = 8.0f;
        float sustainW() const { return juce::jmax (12.0f, (float) getWidth() * 0.10f); }
        float legW() const     { return juce::jmax (10.0f, ((float) getWidth() - 2.0f * kPad - sustainW()) / 3.0f); }
        float tToW (float secs) const { return legW() * std::sqrt (juce::jlimit (0.0f, kMaxT, secs) / kMaxT); }
        float wToT (float w) const
        { const float u = juce::jlimit (0.0f, 1.0f, w / juce::jmax (1.0f, legW())); return u * u * kMaxT; }
        float lToY (float lvl) const { return (float) getHeight() - kPad - juce::jlimit (0.0f, 1.0f, lvl) * ((float) getHeight() - 2.0f * kPad); }
        float yToL (float y) const   { return juce::jlimit (0.0f, 1.0f, ((float) getHeight() - kPad - y) / juce::jmax (1.0f, (float) getHeight() - 2.0f * kPad)); }

        // 0 start | 1 attack end (peak) | 2 decay end = SUSTAIN LEVEL | 3 sustain end | 4 release end
        juce::Point<float> node (int i) const
        {
            const float x0 = kPad;
            const float x1 = x0 + tToW (a);
            const float x2 = x1 + tToW (d);
            const float x3 = x2 + sustainW();
            const float x4 = x3 + tToW (r);
            switch (i)
            {
                case 0:  return { x0, lToY (0.0f) };
                case 1:  return { x1, lToY (1.0f) };
                case 2:  return { x2, lToY (s) };
                case 3:  return { x3, lToY (s) };
                default: return { x4, lToY (0.0f) };
            }
        }

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xff101018));
            g.fillRoundedRectangle (b, 3.0f);
            g.setColour (col.withAlpha (0.18f));
            g.drawRoundedRectangle (b.reduced (0.5f), 3.0f, 1.0f);
            g.setColour (col.withAlpha (0.10f));
            g.drawHorizontalLine ((int) lToY (1.0f), b.getX() + 2.0f, b.getRight() - 2.0f);
            g.drawHorizontalLine ((int) lToY (0.0f), b.getX() + 2.0f, b.getRight() - 2.0f);

            juce::Path p;
            p.startNewSubPath (node (0));
            const auto leg = [&] (juce::Point<float> from, juce::Point<float> to, float curve)
            {
                constexpr int steps = 28;
                for (int i = 1; i <= steps; ++i)
                {
                    const float t = (float) i / (float) steps;
                    p.lineTo (from.x + (to.x - from.x) * t,
                              from.y + (to.y - from.y) * shape (t, curve));
                }
            };
            leg (node (0), node (1), cA);
            leg (node (1), node (2), cD);
            p.lineTo (node (3));
            leg (node (3), node (4), cR);

            auto fill = p;
            fill.lineTo (node (4).x, lToY (0.0f));
            fill.lineTo (node (0).x, lToY (0.0f));
            fill.closeSubPath();
            g.setColour (col.withAlpha (0.13f));
            g.fillPath (fill);
            g.setColour (col);
            g.strokePath (p, juce::PathStrokeType (2.0f));

            // Each draggable node IS its letter - bold, a size up from the old dots,
            // with a thin black outline so it reads over the curve and the fill.
            //
            // AND THE FOUR OF THEM ARE PLACED AS A ROW, NOT FOUR TIMES INDEPENDENTLY.
            //
            // Every letter used to be centred on its own node and nothing else was said,
            // which is right only while the four nodes happen to be far apart. They are
            // not: node x is kPad + tToW(time), and tToW of a short time is a couple of
            // pixels, so the two commonest envelopes in this instrument break it. Read off
            // the rig's own photographs of the modulators panel:
            //
            //   FAST ATTACK. a near 0 puts node 1 at x = kPad = 8, and half of a 15 px
            //   Bold "A" is about 5 px of ink with 1.2 px of outline on top - so the left
            //   third of the letter was drawn outside the graph's 1 px border and what
            //   reached the glass was the A's right leg on its own: a "/".
            //   SHORT DECAY. d near 0 puts node 2 within a pixel or two of node 1, so the
            //   D was printed ON TOP of the S. The row read "/D-S" where it should read
            //   A D S R, and neither half of that is a fit problem any gate can see -
            //   nothing overflowed a component and nothing was clipped by JUCE.
            //
            // Three passes, and the ORDER is the whole fix:
            //   1. CLAMP each letter inside the drawing area. The border is a hard limit:
            //      a letter half outside it is not a letter, it is a stroke.
            //   2. LEFT TO RIGHT, push any letter that still overlaps its predecessor far
            //      enough right to clear it by kLetterGap.
            //   3. RIGHT TO LEFT, the same in reverse. Without this pass a fast attack AND
            //      a short decay accumulate through step 2 and shove R off the RIGHT edge -
            //      curing the overlap by re-creating the clipping at the other end.
            // Then the left edge once more, because step 3 can only push left and the edge
            // outranks the spacing: in a graph too narrow to separate four letters, two
            // letters touching is legible and one letter half-drawn is not.
            //
            // NOT MOVED VERTICALLY, and not clamped there either - tried, and there is
            // nothing to clamp. Node y is lToY(0..1), which spans kPad = 8 to height - 8,
            // and these glyphs are ~11 px tall, so the furthest a letter's ink reaches is
            // ~2.5 px inside the border with its 1.2 px outline on top. The letter keeps
            // its node's HEIGHT, which is what still says which node it belongs to once x
            // has been nudged.
            //
            // nodeAt() IS DELIBERATELY NOT MOVED WITH THE LETTERS. mouseDrag turns the
            // ABSOLUTE mouse x into the parameter (wToT (pos.x - node(i-1).x)), so a grab
            // area sitting on a nudged letter would set the value from a position the node
            // is not at and the curve would jump on the first drag pixel. The node is the
            // value; the letter is its label, and only the label moves. (Two nodes that
            // land on the same pixel were never both grabbable anyway - nodeAt returns the
            // first match - which is a separate, pre-existing consequence of a zero-length
            // stage, not something this placement introduces.)
            static const char* letters[5] = { "", "A", "D", "S", "R" };
            constexpr float kLetterGap = 1.5f;      // ink to ink, so two letters never touch
            // Inside the 1 px rounded border, plus the half of the 2.4 px black outline
            // that is drawn outside the glyph's own path bounds.
            const float xLo = b.getX() + 1.0f + 1.2f, xHi = b.getRight() - 1.0f - 1.2f;
            juce::Path glyph[5];
            float cx[5] = {}, halfW[5] = {};
            for (int i : { 1, 2, 3, 4 })
            {
                juce::GlyphArrangement ga;
                ga.addLineOfText (juce::Font (juce::FontOptions (15.0f).withStyle ("Bold")), letters[i], 0.0f, 0.0f);
                ga.createPath (glyph[i]);
                halfW[i] = glyph[i].getBounds().getWidth() * 0.5f;
                cx[i]    = node (i).x;
                // 1. the edges. Written as a jmin then a jmax rather than a jlimit because
                // in a graph narrower than one letter the two bounds cross, and jlimit
                // asserts on that; this way the LEFT edge wins, which is the readable half.
                cx[i] = juce::jmax (xLo + halfW[i], juce::jmin (xHi - halfW[i], cx[i]));
            }
            // 2. left to right.
            for (int i : { 2, 3, 4 })
                cx[i] = juce::jmax (cx[i], cx[i - 1] + halfW[i - 1] + kLetterGap + halfW[i]);
            // 3. right to left, seeded by the right edge.
            for (int i : { 4, 3, 2 })
            {
                cx[i]     = juce::jmin (cx[i], xHi - halfW[i]);
                cx[i - 1] = juce::jmin (cx[i - 1], cx[i] - halfW[i] - kLetterGap - halfW[i - 1]);
            }
            for (int i : { 1, 2, 3, 4 })
                cx[i] = juce::jmax (cx[i], xLo + halfW[i]);
            for (int i : { 1, 2, 3, 4 })
            {
                const bool hot = i == hover || i == dragNode;
                const auto gb = glyph[i].getBounds();
                auto gp = glyph[i];
                gp.applyTransform (juce::AffineTransform::translation (cx[i] - gb.getCentreX(),
                                                                      node (i).y - gb.getCentreY()));
                g.setColour (juce::Colours::black.withAlpha (0.9f));
                g.strokePath (gp, juce::PathStrokeType (2.4f));
                g.setColour (hot ? juce::Colours::white : col);
                g.fillPath (gp);
            }
        }

        //== interaction ======================================================
        int   dragNode = -1, dragSeg = -1, hover = -1;
        float dragStartCurve = 0.0f, dragStartY = 0.0f;

        int nodeAt (juce::Point<float> pos) const
        {
            for (int i : { 1, 2, 3, 4 })
                if (pos.getDistanceFrom (node (i)) < 12.0f) return i; // letter-sized grab area
            return -1;
        }
        int segAt (juce::Point<float> pos) const   // 0 = attack leg, 1 = decay leg, 2 = release leg
        {
            if (pos.x < node (1).x) return 0;
            if (pos.x < node (2).x) return 1;
            if (pos.x > node (3).x) return 2;
            return -1;                              // the flat sustain leg has no curve
        }
        static const char* segParam (int seg)
        { return seg == 0 ? "CurveA" : (seg == 1 ? "CurveD" : "CurveR"); }
        float segCurve (int seg) const { return seg == 0 ? cA : (seg == 1 ? cD : cR); }

        juce::String draggedParam() const
        {
            switch (dragNode)
            {
                case 1:  return "A";
                case 2:  return "D";
                case 3:  return "S";
                case 4:  return "R";
                default: return dragSeg >= 0 ? juce::String (segParam (dragSeg)) : juce::String();
            }
        }

        // Writes the parameter AND the local value, so the node follows the mouse now.
        void put (const juce::String& suffix, float v)
        {
            if      (suffix == "A") a = v;
            else if (suffix == "D") d = v;
            else if (suffix == "S") s = v;
            else if (suffix == "R") r = v;
            else if (suffix == "CurveA") cA = v;
            else if (suffix == "CurveD") cD = v;
            else if (suffix == "CurveR") cR = v;
            if (onEdit) onEdit (suffix, v);
            repaint();
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            const int h = nodeAt (e.position);
            if (h != hover) { hover = h; repaint(); }
            setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor
                                   : (segAt (e.position) >= 0 ? juce::MouseCursor::UpDownResizeCursor
                                                              : juce::MouseCursor::NormalCursor));
        }
        void mouseExit (const juce::MouseEvent&) override { if (hover >= 0) { hover = -1; repaint(); } }

        void mouseDown (const juce::MouseEvent& e) override
        {
            dragNode = nodeAt (e.position);
            dragSeg  = dragNode >= 0 ? -1 : segAt (e.position);
            if (dragSeg >= 0) { dragStartCurve = segCurve (dragSeg); dragStartY = e.position.y; }
            const auto pid = draggedParam();
            if (pid.isNotEmpty() && onGesture) onGesture (pid, true);
            repaint();
        }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            const auto pos = e.position;
            switch (dragNode)
            {
                case 1: put ("A", juce::jlimit (0.0f, kMaxT, wToT (pos.x - node (0).x))); break;
                case 2: // the SUSTAIN node: decay time across, sustain level up/down
                    put ("D", juce::jlimit (0.0f, kMaxT, wToT (pos.x - node (1).x)));
                    put ("S", yToL (pos.y));
                    break;
                case 3: put ("S", yToL (pos.y)); break;   // sustain level only
                case 4: put ("R", juce::jlimit (0.0f, kMaxT, wToT (pos.x - node (3).x))); break;
                default:
                    if (dragSeg >= 0)
                    {
                        // Drag the LINE: up bends it convex, down concave.
                        const float dy = (dragStartY - pos.y) / juce::jmax (24.0f, (float) getHeight() * 0.6f);
                        put (segParam (dragSeg), juce::jlimit (-1.0f, 1.0f, dragStartCurve + dy * 2.0f));
                    }
                    break;
            }
        }
        void mouseUp (const juce::MouseEvent&) override
        {
            const auto pid = draggedParam();
            if (pid.isNotEmpty() && onGesture) onGesture (pid, false);
            dragNode = -1; dragSeg = -1;
            repaint();
        }
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            const int seg = segAt (e.position);      // straighten that leg again
            if (seg < 0) return;
            const juce::String pid = segParam (seg);
            if (onGesture) onGesture (pid, true);
            put (pid, 0.0f);
            if (onGesture) onGesture (pid, false);
        }
    };

    // Drawn-LFO wave editor (Ableton-automation style): drag points; double-click
    // adds a point; right-click deletes one; dragging a segment's MIDDLE bends it
    // (power curve), like curving an automation line. One cycle, x 0..1, y 0..1.
    struct WaveEditor : juce::Component
    {
        // The EN/CA hook every self-painting component in this file needs, for the reason
        // d8aeeaf documents: the translated gate walks juce::Components, so a word drawn
        // straight onto a Graphics is invisible to it and stays in one language forever.
        std::function<juce::String (const char*, const char*)> trF;
        struct Pt { float x = 0, y = 0, curve = 0; }; // curve bends the segment AFTER this point
        std::vector<Pt> pts { { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };
        std::function<void()> onChange, onClose;
        juce::TextButton closeB { "X" };
        int dragIdx = -1, curveIdx = -1;
        float curveStartVal = 0.0f, curveStartY = 0.0f;

        WaveEditor()
        {
            closeB.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff44445e));
            closeB.onClick = [this] { if (onClose) onClose(); };
            addAndMakeVisible (closeB);
        }
        void resized() override { closeB.setBounds (getWidth() - 26, 4, 22, 22); }

        static float shaped (float t, float curve) // Ableton-style segment bend
        { return std::pow (juce::jlimit (0.0f, 1.0f, t), std::exp2 (curve * 3.0f)); }
        float valueAt (float x) const
        {
            if (pts.empty()) return 0.0f;
            if (x <= pts.front().x) return pts.front().y;
            for (size_t i = 1; i < pts.size(); ++i)
                if (x <= pts[i].x)
                {
                    const auto& a = pts[i - 1]; const auto& b = pts[i];
                    const float t = b.x > a.x ? (x - a.x) / (b.x - a.x) : 1.0f;
                    return a.y + (b.y - a.y) * shaped (t, a.curve);
                }
            return pts.back().y;
        }
        void fillTable (float* out, int n) const
        { for (int i = 0; i < n; ++i) out[i] = valueAt ((float) i / (float) (n - 1)); }

        juce::String toText() const
        {
            juce::String s;
            for (const auto& p : pts)
                s += juce::String (p.x, 4) + ":" + juce::String (p.y, 4) + ":" + juce::String (p.curve, 3) + ";";
            return s;
        }
        void fromText (const juce::String& s)
        {
            std::vector<Pt> v;
            for (const auto& tok : juce::StringArray::fromTokens (s, ";", ""))
            {
                auto parts = juce::StringArray::fromTokens (tok, ":", "");
                if (parts.size() == 3)
                    v.push_back ({ parts[0].getFloatValue(), parts[1].getFloatValue(), parts[2].getFloatValue() });
            }
            if (v.size() >= 2) pts = std::move (v);
        }

        juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (14.0f, 30.0f); }
        juce::Point<float> toScreen (const Pt& p) const
        { auto r = plot(); return { r.getX() + p.x * r.getWidth(), r.getBottom() - p.y * r.getHeight() }; }

        void paint (juce::Graphics& g) override
        {
            g.setColour (juce::Colour (0xf2101018));
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);
            g.setColour (juce::Colour (0xff000000u | galactic::palette::kCustomMod));
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 10.0f, 3.0f);
            g.setFont (juce::Font (juce::FontOptions (14.0f).withStyle ("Bold")));
            g.drawText (trF ? trF ("LFO wave  -  drag points, double-click adds, right-click deletes,"
                                   " drag a line's middle to curve it",
                                   "Ona de l'LFO  -  arrossega els punts, doble clic n'afegeix,"
                                   " clic dret n'esborra, arrossega el mig d'una l\xc3\xadnia per corbar-la")
                              : juce::String ("LFO wave  -  drag points, double-click adds, right-click"
                                              " deletes, drag a line's middle to curve it"),
                        14, 6, getWidth() - 48, 20, juce::Justification::centredLeft);
            auto r = plot();
            g.setColour (juce::Colour (0xff20202b));
            g.fillRect (r);
            g.setColour (juce::Colour (0x33ffffff));
            for (int i = 1; i < 4; ++i)
            {
                g.drawHorizontalLine ((int) (r.getY() + r.getHeight() * i / 4.0f), r.getX(), r.getRight());
                g.drawVerticalLine   ((int) (r.getX() + r.getWidth()  * i / 4.0f), r.getY(), r.getBottom());
            }
            juce::Path curve;
            for (int i = 0; i <= 256; ++i)
            {
                const float x = (float) i / 256.0f;
                const auto  p = juce::Point<float> (r.getX() + x * r.getWidth(),
                                                    r.getBottom() - valueAt (x) * r.getHeight());
                i == 0 ? curve.startNewSubPath (p) : curve.lineTo (p);
            }
            g.setColour (juce::Colour (0xff000000u | galactic::palette::kCustomMod));
            g.strokePath (curve, juce::PathStrokeType (2.2f));
            for (const auto& p : pts)
            {
                g.setColour (juce::Colours::white);
                g.fillEllipse (juce::Rectangle<float> (11, 11).withCentre (toScreen (p)));
            }
        }
        int hitPoint (juce::Point<float> m) const
        {
            for (size_t i = 0; i < pts.size(); ++i)
                if (toScreen (pts[i]).getDistanceFrom (m) < 9.0f) return (int) i;
            return -1;
        }
        void mouseDown (const juce::MouseEvent& e) override
        {
            const auto m = e.position;
            const int hp = hitPoint (m);
            if (e.mods.isRightButtonDown())
            {
                if (hp > 0 && hp < (int) pts.size() - 1) { pts.erase (pts.begin() + hp); changed(); }
                return;
            }
            if (hp >= 0) { dragIdx = hp; return; }
            // grab the SEGMENT under the mouse to bend it
            auto r = plot();
            const float x = juce::jlimit (0.0f, 1.0f, (m.x - r.getX()) / r.getWidth());
            for (size_t i = 1; i < pts.size(); ++i)
                if (x <= pts[i].x) { curveIdx = (int) i - 1; break; }
            if (curveIdx >= 0) { curveStartVal = pts[(size_t) curveIdx].curve; curveStartY = m.y; }
        }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            auto r = plot();
            if (dragIdx >= 0)
            {
                auto& p = pts[(size_t) dragIdx];
                p.y = juce::jlimit (0.0f, 1.0f, (r.getBottom() - e.position.y) / r.getHeight());
                if (dragIdx > 0 && dragIdx < (int) pts.size() - 1) // endpoints keep x = 0 / 1
                    p.x = juce::jlimit (pts[(size_t) dragIdx - 1].x + 0.01f,
                                        pts[(size_t) dragIdx + 1].x - 0.01f,
                                        (e.position.x - r.getX()) / r.getWidth());
                changed();
            }
            else if (curveIdx >= 0)
            {
                const bool down = pts[(size_t) curveIdx + 1].y < pts[(size_t) curveIdx].y;
                const float d = (curveStartY - e.position.y) / 120.0f;
                pts[(size_t) curveIdx].curve = juce::jlimit (-1.0f, 1.0f, curveStartVal + (down ? -d : d));
                changed();
            }
        }
        void mouseUp (const juce::MouseEvent&) override { dragIdx = -1; curveIdx = -1; }
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            auto r = plot();
            Pt np { juce::jlimit (0.01f, 0.99f, (e.position.x - r.getX()) / r.getWidth()),
                    juce::jlimit (0.0f, 1.0f, (r.getBottom() - e.position.y) / r.getHeight()), 0.0f };
            for (size_t i = 1; i < pts.size(); ++i)
                if (np.x < pts[i].x) { pts.insert (pts.begin() + (long) i, np); break; }
            changed();
        }
        void changed() { repaint(); if (onChange) onChange(); }
    };

    //==========================================================================
    // Per-partial amplitude editor (the pencil next to the SLOPE fader).
    //==========================================================================

    // The pencil itself, drawn as a path: at the 24 px this cell can spare, a letter
    // or the word "Edit" was unreadable, and the icon says "draw the spectrum" in a
    // way no abbreviation did.
    struct PencilButton : juce::Button
    {
        juce::Colour col { 0xff5cb8ff };
        PencilButton() : juce::Button ("pencil") { setMouseClickGrabsKeyboardFocus (false); }
        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (juce::Colour (down ? 0xff2c3648 : (over ? 0xff232c3c : 0xff171d29)));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (col.withAlpha (over || down ? 1.0f : 0.8f));
            g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);

            // Tip at the bottom-left, blunt end at the top-right; the nib is a triangle
            // so the icon still reads as a pencil and not as a slash.
            const float pad = juce::jmax (3.0f, r.getWidth() * 0.24f);
            const juce::Point<float> tip { r.getX() + pad, r.getBottom() - pad };
            const juce::Point<float> end { r.getRight() - pad, r.getY() + pad };
            const float len = tip.getDistanceFrom (end);
            if (len < 2.0f) return;
            const juce::Point<float> dir { (end.x - tip.x) / len, (end.y - tip.y) / len };
            const juce::Point<float> nrm { -dir.y, dir.x };
            const float w = juce::jmax (1.2f, r.getWidth() * 0.11f);
            const auto shoulder = tip + dir * (len * 0.26f);
            juce::Path body;
            body.startNewSubPath (shoulder + nrm * w);
            body.lineTo (end + nrm * w);
            body.lineTo (end - nrm * w);
            body.lineTo (shoulder - nrm * w);
            body.closeSubPath();
            g.fillPath (body);
            juce::Path nib;
            nib.addTriangle (tip, shoulder + nrm * w, shoulder - nrm * w);
            g.fillPath (nib);
        }
    };

    // The knob grid.
    //
    // ONE Component draws every partial. A juce::Slider per partial is not an option:
    // kMaxPartials is 16384 and a thousand partials is an ordinary patch, and JUCE's
    // per-child hit-testing, focus and repaint bookkeeping collapses long before that
    // (the window took seconds to open and then dropped frames on every mouse move).
    // So this paints only the rows the clip region touches and does its own hit
    // testing, exactly the way ScaleKeyboard draws and dispatches its keys.
    //
    // EVERY partial of the template is listed. There is no "first N" window and no
    // cap: a partial the user cannot see is a partial they cannot un-mute.
    struct PartialAmpGrid : juce::Component, juce::TooltipClient
    {
        partialamp::Model model;
        std::function<juce::String (const char*, const char*)> trF;
        std::function<void()> onGestureStart, onEdited, onGestureEnd;
        bool manualOn = false;   // dims the whole grid when the edits are not sounding

        static constexpr int kCellW = 62, kCellH = 80;

        int   columns()  const { return juce::jmax (1, getWidth() / kCellW); }
        float colWidth() const { return (float) getWidth() / (float) columns(); }
        int   rows()     const { return (model.count() + columns() - 1) / columns(); }
        int   contentHeight() const { return juce::jmax (kCellH, rows() * kCellH); }

        juce::Rectangle<float> cellBounds (int i) const
        {
            const int c = columns();
            const float cw = colWidth();
            return { (float) (i % c) * cw, (float) (i / c) * (float) kCellH, cw, (float) kCellH };
        }
        int cellAt (juce::Point<float> p) const
        {
            const int c = columns();
            const int col = juce::jlimit (0, c - 1, (int) (p.x / colWidth()));
            const int row = (int) (p.y / (float) kCellH);
            if (row < 0) return -1;
            const int i = row * c + col;
            return model.valid (i) ? i : -1;
        }

        //-- painting ----------------------------------------------------------
        void paint (juce::Graphics& g) override
        {
            const int n = model.count();
            if (n == 0)
            {
                g.setColour (juce::Colour (0xff8f96a8));
                g.setFont (juce::Font (juce::FontOptions (13.0f)));
                g.drawText (trF ? trF ("No partials in the current spectrum.",
                                       "Cap parcial a l'espectre actual.")
                                : juce::String(),
                            getLocalBounds(), juce::Justification::centred, false);
                return;
            }
            const auto clip = g.getClipBounds();
            const int c = columns();
            const int r0 = juce::jmax (0, clip.getY() / kCellH);
            const int r1 = juce::jmin (rows() - 1, clip.getBottom() / kCellH);
            for (int r = r0; r <= r1; ++r)
                for (int k = 0; k < c; ++k)
                {
                    const int i = r * c + k;
                    if (i >= n) return;
                    drawCell (g, i, cellBounds (i));
                }
        }

        void drawCell (juce::Graphics& g, int i, juce::Rectangle<float> cell)
        {
            const auto& cd = model.cells[(size_t) i];
            const bool sel = model.isSelected (i);
            const float dim = (manualOn ? 1.0f : 0.55f) * (cd.locked ? 0.45f : 1.0f);

            if (sel)
            {
                g.setColour (juce::Colour (0xff2fb0c8).withAlpha (0.28f));
                g.fillRoundedRectangle (cell.reduced (1.5f), 4.0f);
                g.setColour (juce::Colour (0xff2fb0c8).withAlpha (i == model.anchor ? 0.95f : 0.5f));
                g.drawRoundedRectangle (cell.reduced (1.5f), 4.0f, i == model.anchor ? 1.6f : 1.0f);
            }
            else if (i == hover)
            {
                g.setColour (juce::Colour (0x18ffffff));
                g.fillRoundedRectangle (cell.reduced (1.5f), 4.0f);
            }

            auto body = cell.reduced (2.0f, 3.0f);
            auto top  = body.removeFromTop (13.0f);
            auto bot  = body.removeFromBottom (12.0f);

            // Caption = the PARTIAL NUMBER: "3", never "3/1" (see partialNumberName).
            g.setColour (juce::Colours::white.withAlpha (0.92f * dim));
            g.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
            g.drawText (juce::String::fromUTF8 (dissonance::partialNumberName (cd.ratio).c_str()),
                        top.toNearestInt(), juce::Justification::centred, false);

            // Knob: the arc runs the engine's whole range, and the coloured sweep starts
            // at the ZERO tick, so the eye reads "how far from the compiled amplitude"
            // rather than an absolute position that means nothing on its own.
            const float rad = juce::jmin (body.getWidth(), body.getHeight()) * 0.44f;
            const juce::Point<float> ctr { body.getCentreX(), body.getCentreY() };
            constexpr float a0 = -2.356f, a1 = 2.356f;   // -135 deg .. +135 deg
            const auto angleOf = [&] (float dB)
            {
                const float t = (dB - partialamp::kMinDb) / (partialamp::kMaxDb - partialamp::kMinDb);
                return a0 + juce::jlimit (0.0f, 1.0f, t) * (a1 - a0);
            };
            const float aZero = angleOf (0.0f);
            const float aVal  = angleOf (cd.dB);

            juce::Path back;
            back.addCentredArc (ctr.x, ctr.y, rad, rad, 0.0f, a0, a1, true);
            g.setColour (juce::Colour (0xff39414f).withAlpha (dim));
            g.strokePath (back, juce::PathStrokeType (2.5f));

            const bool muted = partialamp::isMuted (cd.dB);
            const juce::Colour vc = muted        ? juce::Colour (0xffe0453f)
                                  : cd.dB > 0.0f ? juce::Colour (0xffffb020)
                                  : cd.dB < 0.0f ? juce::Colour (0xff5cb8ff)
                                                 : juce::Colour (0xff6c7488);
            if (cd.dB != 0.0f)
            {
                juce::Path val;
                val.addCentredArc (ctr.x, ctr.y, rad, rad, 0.0f,
                                   juce::jmin (aZero, aVal), juce::jmax (aZero, aVal), true);
                g.setColour (vc.withAlpha (dim));
                g.strokePath (val, juce::PathStrokeType (2.8f));
            }
            // Zero tick: where this partial's own compiled amplitude sits.
            g.setColour (juce::Colours::white.withAlpha (0.45f * dim));
            {
                const juce::Point<float> d { std::sin (aZero), -std::cos (aZero) };
                g.drawLine (ctr.x + d.x * (rad - 3.0f), ctr.y + d.y * (rad - 3.0f),
                            ctr.x + d.x * (rad + 3.0f), ctr.y + d.y * (rad + 3.0f), 1.0f);
            }
            // Pointer.
            {
                const juce::Point<float> d { std::sin (aVal), -std::cos (aVal) };
                g.setColour (vc.withAlpha (dim));
                g.drawLine (ctr.x + d.x * rad * 0.25f, ctr.y + d.y * rad * 0.25f,
                            ctr.x + d.x * (rad - 1.0f), ctr.y + d.y * (rad - 1.0f), 2.0f);
            }

            // Read-out. MUTE is spelled out: "-120.0" would read as a number the user
            // could nudge, when it is the point at which the partial stops sounding.
            juce::String vt;
            if (cd.locked)     vt = juce::String::fromUTF8 ("\xe2\x9c\x95");  // locked (see Cell::locked)
            // The one word in this cell, and it was the one string in the window that never
            // went through tr: painted straight onto the Graphics, so walkTextFits could not
            // reach it and the translated gate never saw an English word sitting in a
            // Catalan UI. "MUT" is also two glyphs narrower, which this 62 px cell notices.
            else if (muted)    vt = trF ? trF ("MUTE", "MUT") : "MUTE";
            else if (cd.dB == 0.0f) vt = "0.0";
            else               vt = (cd.dB > 0.0f ? "+" : "") + juce::String (cd.dB, 1);
            g.setColour ((cd.dB == 0.0f ? juce::Colour (0xff8f96a8) : vc).withAlpha (dim));
            g.setFont (juce::Font (juce::FontOptions (10.5f).withStyle (cd.dB == 0.0f ? "Regular" : "Bold")));
            g.drawText (vt, bot.toNearestInt(), juce::Justification::centred, false);
        }

        //-- interaction -------------------------------------------------------
        int   hover = -1;
        float dragStartY = 0.0f;

        juce::String getTooltip() override
        {
            if (! model.valid (hover) || trF == nullptr) return {};
            const auto& cd = model.cells[(size_t) hover];
            const double natDb = cd.natural > 0.0f ? 20.0 * std::log10 ((double) cd.natural) : -144.0;
            juce::String s;
            s << trF ("Partial ", "Parcial ") << (hover + 1) << " / " << model.count()
              << "   *" << juce::String::fromUTF8 (dissonance::partialNumberName (cd.ratio).c_str()) << "*\n"
              << trF ("As compiled: `", "Tal com es compila: `") << juce::String (natDb, 1) << " dB`\n";
            if (cd.locked)
                s << trF ("!Locked!: this partial sits under 0.01 cent from the one below it, so an edit cannot be aimed at it separately.",
                          "!Blocat!: aquest parcial \xc3\xa9s a menys de 0,01 cent del de sota, i una edici\xc3\xb3 no es pot dirigir nom\xc3\xa9s a ell.");
            else if (partialamp::isMuted (cd.dB))
                s << trF ("Your edit: *MUTE*. The partial is !still in the template! - it comes back on undo, or when manual mode goes off.",
                          "La teva edici\xc3\xb3: *MUT*. El parcial !segueix a la plantilla! - torna amb un desfer, o quan apagues el mode manual.");
            else
                s << trF ("Your edit: `", "La teva edici\xc3\xb3: `") << (cd.dB > 0.0f ? "+" : "")
                  << juce::String (cd.dB, 1) << trF (" dB`\nDrag up/down; Ctrl = fine; double-click = back to compiled.",
                                                     " dB`\nArrossega amunt/avall; Ctrl = fi; doble clic = torna al compilat.");
            return s;
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            const int h = cellAt (e.position);
            if (h != hover)
            {
                if (model.valid (hover)) repaint (cellBounds (hover).getSmallestIntegerContainer());
                hover = h;
                if (model.valid (hover)) repaint (cellBounds (hover).getSmallestIntegerContainer());
            }
        }
        void mouseExit (const juce::MouseEvent&) override
        {
            if (model.valid (hover)) repaint (cellBounds (hover).getSmallestIntegerContainer());
            hover = -1;
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const int i = cellAt (e.position);
            if (i < 0) { model.clearSelection(); repaint(); return; }
            if (e.mods.isShiftDown())
            {
                model.shiftClick (i);   // arm, then stretch (see Model::shiftClick)
                repaint();
                return;
            }
            if (! model.isSelected (i)) { model.clearSelection(); repaint(); }
            dragStartY = e.position.y;
            model.beginDrag (i);
            if (onGestureStart) onGestureStart();
        }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! model.dragging()) return;
            const float perPx = e.mods.isCtrlDown() ? partialamp::kDbPerPixelFine
                                                    : partialamp::kDbPerPixel;
            if (model.applyDragDelta ((dragStartY - e.position.y) * perPx))
            {
                repaintDragSet();
                if (onEdited) onEdited();
            }
        }
        void mouseUp (const juce::MouseEvent&) override
        {
            if (! model.dragging()) return;
            model.endDrag();
            if (onGestureEnd) onGestureEnd();
        }
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            const int i = cellAt (e.position);
            if (i < 0) return;
            const auto set = model.dragSetFor (i);
            if (onGestureStart) onGestureStart();
            if (model.resetSet (set)) { repaint(); if (onEdited) onEdited(); }
            if (onGestureEnd) onGestureEnd();
        }
        void repaintDragSet()
        {
            // A selection can be thousands of cells; repainting each rect separately is
            // slower than one pass over the visible rows.
            if (model.dragIdx.size() > 24) repaint();
            else for (int k : model.dragIdx) repaint (cellBounds (k).getSmallestIntegerContainer());
        }
    };

    // The window's contents: the linked ON/OFF, the two facts that surprise everyone,
    // the honest counters, and the scrolling grid.
    struct PartialAmpPanel : juce::Component
    {
        // A gesture is released by mouse-up - and a mouse-up is NOT guaranteed if this
        // window is torn down mid-drag. A hold left standing would stop the processor's
        // ~1 s undo cut for the rest of the session, which is the unbounded-ActionSet
        // bug that cut exists to prevent, so the teardown releases it too.
        ~PartialAmpPanel() override
        {
            if (proc != nullptr && undoGestureHeld) proc->endUndoGesture();
        }
        // True between this panel's own beginUndoGesture and endUndoGesture. Tracked
        // here rather than inferred from the grid's drag state so the destructor above
        // releases EXACTLY what this panel took, once.
        bool undoGestureHeld = false;
        FractalAdditiveAudioProcessor* proc = nullptr;
        std::function<juce::String (const char*, const char*)> trF;
        std::function<void()> onEdited;                     // editor: refresh the slope cell
        // Fired the instant a pencil edit arms manual mode by itself. The editor uses it
        // to grey SLOPE and pop its FROZEN explanation THERE AND THEN, rather than
        // letting the 30 Hz poll do it silently behind a window the user is not looking at.
        std::function<void()> onManualArmed;
        bool armedManualThisGesture = false;                // this drag turned manual on

        juce::ToggleButton onT;                             // twin of the pill under the pencil
        std::unique_ptr<ButtonAttachment> onAtt;
        juce::TextButton   resetAllB;
        juce::Label        headLbl, factsLbl, statLbl, warnLbl;
        juce::Viewport     view;
        PartialAmpGrid     grid;
        // Its own TooltipWindow, PARENTED to this panel. The editor's is parented to the
        // editor, and a tooltip window only serves its own parent's subtree - so in a
        // separate top-level window every hover read-out here was simply invisible.
        juce::TooltipWindow tooltips { this, 600 }; // the editor's own number - see tooltipWindow

        // Edits that bound to no partial of the CURRENT spectrum. Kept verbatim and
        // merged back on every write - see partialamp::mergedGains.
        std::vector<spectrum::PartialGain> homeless;
        std::vector<spectrum::PartialGain> gestureBefore;   // for one undo step per drag
        juce::uint32 lastGen = 0;
        unsigned     lastRev = 0;
        bool         haveSynced = false;

        PartialAmpPanel()
        {
            onT.setName ("onled");
            addAndMakeVisible (onT);
            resetAllB.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe0453f));
            resetAllB.setMouseClickGrabsKeyboardFocus (false);
            resetAllB.onClick = [this] { resetEverything(); };
            addAndMakeVisible (resetAllB);
            for (auto* l : { &headLbl, &factsLbl, &statLbl, &warnLbl })
            {
                l->setInterceptsMouseClicks (false, false);
                addAndMakeVisible (*l);
            }
            headLbl.setFont (juce::Font (juce::FontOptions (15.0f).withStyle ("Bold")));
            headLbl.setColour (juce::Label::textColourId, juce::Colour (0xff5cb8ff));
            factsLbl.setFont (juce::Font (juce::FontOptions (11.5f)));
            factsLbl.setColour (juce::Label::textColourId, juce::Colour (0xffc4ccda));
            statLbl.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
            statLbl.setColour (juce::Label::textColourId, juce::Colour (0xff8f96a8));
            warnLbl.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
            warnLbl.setColour (juce::Label::textColourId, juce::Colour (0xffffb020));
            view.setViewedComponent (&grid, false);
            view.setScrollBarsShown (true, false);
            // The knob list takes the keyboard focus inside its window, so Up/Down/Page
            // scroll it. That also puts it FIRST in the key-event chain, ahead of the
            // window's forward-to-the-editor hook: a key this list uses is used here and
            // never also rings a note.
            view.setWantsKeyboardFocus (true);
            addAndMakeVisible (view);
        }

        void init (FractalAdditiveAudioProcessor& p)
        {
            proc = &p;
            grid.onGestureStart = [this]
            {
                // HOLD OFF THE ~1 s UNDO CUT for as long as this drag lasts. The
                // processor's 60 Hz timer opens a new transaction every second so a
                // slider drag undoes as one step - and that cut used to land inside a
                // pencil drag longer than about a second, splitting it in two. On the
                // gesture that also arms manual mode (below) the halves are the flip and
                // the amplitudes, so one Ctrl+Z took back the edits and left manual ARMED
                // WITH NO EDITS. First, before anything opens a transaction.
                if (! undoGestureHeld) { proc->beginUndoGesture(); undoGestureHeld = true; }
                // ARM MANUAL MODE FIRST, and before gestureBefore is captured.
                // With "partialManual" off - and it ships off - the engine throws these
                // edits away, so the drag below moved the sound by exactly 0.00 dB while
                // the knob, the read-out and the undo stack all said it had worked. See
                // armManualPartialAmpsForEdit for the measurement and for why arming is
                // the right answer rather than refusing the drag.
                // FIRST, because it freezes the slope at its PRE-drag value and puts the
                // edits in force before pushGains() runs on the drag's first frame.
                armedManualThisGesture = proc->armManualPartialAmpsForEdit();
                gestureBefore = proc->getPartialGains();
                if (armedManualThisGesture)
                {
                    refreshText();                     // the panel's banner changes meaning
                    if (onManualArmed) onManualArmed(); // ...and SLOPE goes inert in the main window
                }
            };
            grid.onEdited       = [this] { pushGains(); };
            grid.onGestureEnd   = [this]
            {
                // ONE undo step per gesture: pushing per frame filled the stack with
                // hundreds of micro-moves and made Ctrl+Z useless on a hand-drawn curve.
                // ...and when this gesture also armed manual mode, that flip is already
                // inside the transaction armManualPartialAmpsForEdit opened, so ONE
                // Ctrl+Z takes back the whole action instead of leaving manual on.
                proc->commitPartialGains (gestureBefore, proc->getPartialGains(),
                                          /*startNewTransaction*/ ! armedManualThisGesture);
                armedManualThisGesture = false;
                // ...and the timer may cut again: the gesture is over, and the commit
                // above is already inside the transaction it belongs to.
                if (undoGestureHeld) { proc->endUndoGesture(); undoGestureHeld = false; }
                lastRev = proc->getPartialGainsRevision();
                refreshText();
            };
            onAtt = std::make_unique<ButtonAttachment> (proc->apvts, "partialManual", onT);
        }

        bool manualOn() const
        {
            return proc != nullptr && proc->apvts.getRawParameterValue ("partialManual")->load() > 0.5f;
        }

        void pushGains()
        {
            if (proc == nullptr) return;
            proc->setPartialGains (partialamp::mergedGains (grid.model, homeless), manualOn());
            lastRev = proc->getPartialGainsRevision();
            refreshText();
            if (onEdited) onEdited();
        }

        void resetEverything()
        {
            if (proc == nullptr) return;
            // Clears the visible knobs AND the homeless edits: this button says "no
            // hand-drawn amplitudes at all", so leaving invisible ones behind to
            // reappear later would be a lie. One undo step brings the lot back.
            const auto before = proc->getPartialGains();
            for (auto& c : grid.model.cells) c.dB = 0.0f;
            homeless.clear();
            proc->setPartialGains ({}, manualOn());
            proc->commitPartialGains (before, {});
            lastRev = proc->getPartialGainsRevision();
            grid.repaint();
            refreshText();
            if (onEdited) onEdited();
        }

        //-- keeping up with the spectrum --------------------------------------
        // Called on the editor's 30 Hz timer. The template is rebuilt by a background
        // thread on every Base/Fractal/prime/tuning change, so the knob list has to
        // follow it; and the stored edits can change without any template change at all
        // (an undo, a preset, a reload), which is what the revision counter catches.
        void syncIfNeeded()
        {
            if (proc == nullptr || grid.model.dragging()) return;   // never mid-gesture
            const auto gen = proc->getTemplateGeneration();
            const auto rev = proc->getPartialGainsRevision();
            if (haveSynced && gen == lastGen && rev == lastRev) return;
            lastGen = gen; lastRev = rev; haveSynced = true;
            resync();
        }

        void resync()
        {
            if (proc == nullptr) return;
            const auto* t = proc->getLiveTemplate();

            std::vector<partialamp::Cell> cells;
            if (t != nullptr)
            {
                cells.reserve (t->partials.size());
                for (const auto& p : t->partials)
                    cells.push_back ({ (double) p.ratio, p.amp, spectrum::gainKey ((double) p.ratio), 0.0f, false });
            }

            // Which stored edit belongs to which knob is decided by the SAME pass the
            // audio uses (bindGains), never by a second rule invented here - a knob must
            // not show an edit that some other partial is actually getting.
            const auto& stored = proc->getPartialGains();
            homeless.clear();
            if (t != nullptr && ! stored.empty())
            {
                std::vector<int> where;
                spectrum::bindGains (t->partials, stored, where);
                for (size_t j = 0; j < stored.size(); ++j)
                {
                    if (where[j] >= 0) cells[(size_t) where[j]].dB = stored[j].dB;
                    else               homeless.push_back (stored[j]);
                }
            }
            else
            {
                homeless = stored;
            }

            // Keep the selection across a rebuild that did not actually move the
            // partials (the spectrum is republished on every override write, and losing
            // a 200-knob selection to that would make range editing impossible).
            const bool same = grid.model.cells.size() == cells.size()
                && std::equal (cells.begin(), cells.end(), grid.model.cells.begin(),
                               [] (const partialamp::Cell& a, const partialamp::Cell& b)
                               { return a.key == b.key; });
            std::vector<char> keepSel;
            int keepAnchor = -1;
            if (same) { keepSel = grid.model.sel; keepAnchor = grid.model.anchor; }
            grid.model.setCells (std::move (cells));
            if (same) { grid.model.sel = std::move (keepSel); grid.model.anchor = keepAnchor; }

            layoutGrid();
            refreshText();
            grid.repaint();
        }

        void refreshText()
        {
            if (proc == nullptr || trF == nullptr) return;
            grid.manualOn = manualOn();

            const int n       = grid.model.count();
            const int edited  = grid.model.editedCount();
            const int lockedN = grid.model.lockedCount();
            juce::String s;
            s << n << trF (" partials", " parcials") << "   \xc2\xb7   "
              << edited << trF (" edited", " editats");
            if (! homeless.empty())
                s << "   \xc2\xb7   " << (int) homeless.size()
                  << trF (" waiting for their partial (kept)", " esperant el seu parcial (desats)");
            if (lockedN > 0)
                s << "   \xc2\xb7   " << lockedN << trF (" locked (under 0.01\xc2\xa2 apart)",
                                                          " blocats (a menys de 0,01\xc2\xa2)");
            statLbl.setText (s, juce::dontSendNotification);

            juce::String w;
            if (! manualOn())
                w << trF ("MANUAL IS OFF - these knobs are stored but not sounding. Your first drag switches it on for you (one Ctrl+Z takes both back).",
                          "EL MODE MANUAL \xc3\x89S APAGAT - aquests controls es desen per\xc3\xb2 no sonen. El primer arrossegament l'activa per tu (un sol Ctrl+Z ho desf\xc3\xa0 tot).");
            else
                // THE AMBUSH, ON SCREEN. Manual mode freezes the compile slope, so
                // somebody who drags one partial here and then reaches for AMP DECAY in
                // the main window finds a fader that does nothing and no reason given.
                // It has to be said where the user is looking - which, mid-drag, is this
                // window and not the fader three hundred pixels away in another one.
                w << trF ("MANUAL IS ON - these knobs are the amplitudes now, so AMP DECAY (slope) in the main window is FROZEN where it stood. Switch manual off and it applies again.",
                          "EL MODE MANUAL \xc3\x89S ACTIU - ara les amplituds venen d'aquests controls, i la CAIGUDA AMP (pendent) de la finestra principal queda CONGELADA tal com estava. Apaga el manual i torna a aplicar-se.");
            if (proc->isSpectrumTruncated())
            {
                if (w.isNotEmpty()) w << "\n";
                w << trF ("TRUNCATED: the spectrum hit the ", "TRUNCAT: l'espectre ha topat amb el sostre de ")
                  << spectrum::kMaxPartials
                  << trF ("-partial ceiling (", " parcials (") << proc->getRawPartialCount()
                  << trF (" wanted). The partials the cap removed are NOT in this list.",
                          " en volia). Els parcials que el sostre ha tret NO s\xc3\xb3n en aquesta llista.");
            }
            const bool hadBanner = warnLbl.getText().isNotEmpty();
            warnLbl.setText (w, juce::dontSendNotification);
            warnLbl.setColour (juce::Label::textColourId,
                               proc->isSpectrumTruncated() ? juce::Colour (0xffe0453f)
                                                           : juce::Colour (0xffffb020));
            // The banner's row is zero-height when there is nothing to say, so its
            // appearing has to re-run the layout or the text would land in a 0 px strip
            // and the truncation warning - the one message that must never be missed -
            // would be invisible.
            if (hadBanner != w.isNotEmpty()) resized();
        }

        void applyLanguage()
        {
            if (trF == nullptr) return;
            headLbl.setText (trF ("PARTIAL AMPLITUDES", "AMPLITUDS DELS PARCIALS"), juce::dontSendNotification);
            onT.setButtonText ({});
            resetAllB.setButtonText (trF ("Reset all", "Reinicia-ho tot"));
            resetAllB.setTooltip (trF ("Drop every per-partial edit, including the ones whose partial is not in this spectrum right now. One Ctrl+Z brings them all back.",
                                       "Descarta totes les edicions per parcial, incloses les dels parcials que ara no hi s\xc3\xb3n. Un sol Ctrl+Z les torna totes."));
            onT.setTooltip (trF ("Manual per-partial amplitudes. The SAME switch as the one under the pencil. Dragging a knob turns it on for you if it is off, because otherwise the drag would be stored and never sound. While it is on, SLOPE is frozen.",
                                 "Amplituds manuals per parcial. \xc3\x89s EL MATEIX interruptor que el de sota el llapis. Arrossegar un control l'activa si est\xc3\xa0 apagat, perqu\xc3\xa8 si no l'arrossegament es desaria i no sonaria mai. Mentre estigui actiu, el PENDENT queda congelat."));
            // The two things that surprise everyone, on screen rather than in a tooltip
            // nobody opens: they are not bugs and they are not reversible by guessing.
            factsLbl.setText (trF (
                "Pulling one partial DOWN lifts the others: the voice rebuilds its normalisation from these targets every sample, so this redistributes the sound, it does not attenuate it.\n"
                "A partial at MUTE is still in the template - it comes straight back with Ctrl+Z, or when manual mode goes off. Nothing here removes a partial.\n"
                "SHIFT+click a knob, then SHIFT+click another: both and everything between are selected, and a drag moves the whole selection.",
                "Abaixar un parcial PUJA els altres: la veu reconstrueix la normalitzaci\xc3\xb3 des d'aquests objectius a cada mostra, aix\xc3\xad que aix\xc3\xb2 redistribueix el so, no l'atenua.\n"
                "Un parcial en MUT segueix a la plantilla - torna de seguida amb Ctrl+Z, o quan apagues el mode manual. Res d'aqu\xc3\xad no treu cap parcial.\n"
                "SHIFT+clic en un control i SHIFT+clic en un altre: se seleccionen tots dos i tot el que hi ha entremig, i arrossegar mou tota la selecci\xc3\xb3."),
                juce::dontSendNotification);
            refreshText();
        }

        void layoutGrid()
        {
            grid.setSize (juce::jmax (60, view.getWidth() - (view.isVerticalScrollBarShown() ? 10 : 0)),
                          grid.contentHeight());
            // The height depends on the column count, which depends on the width the
            // scrollbar leaves: settle it in a second pass or a spectrum that only just
            // needs scrolling ends one row short of its last partials.
            grid.setSize (juce::jmax (60, view.getWidth() - (view.isVerticalScrollBarShown() ? 10 : 0)),
                          grid.contentHeight());
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (10, 8);
            auto head = r.removeFromTop (26);
            resetAllB.setBounds (head.removeFromRight (juce::jmax (96, fgfit::textButtonWidth (
                                     resetAllB.getButtonText(), 22, false))).reduced (0, 2));
            head.removeFromRight (8);
            onT.setBounds (head.removeFromRight (58).reduced (0, 2));
            head.removeFromRight (8);
            headLbl.setBounds (head);
            r.removeFromTop (2);
            statLbl.setBounds (r.removeFromTop (16));
            // Three paragraphs, in two languages, at whatever width the window is: a
            // fixed 52 px was 23 short of both, so the last line was cut off inside the
            // panel with nothing to say it had been.
            factsLbl.setBounds (r.removeFromTop (juce::jmax (52, fgfit::labelHeight (factsLbl, r.getWidth()))));
            // MEASURED, NOT 32. This is the same fault the line above it already fixed,
            // left behind on the one label in this window that must never be cut: the
            // manual-amplitudes banner and, worse, the TRUNCATED banner. At the shipped
            // 1100x740 the banner is one line and 32 was enough, so no size anyone looks
            // at ever showed it - but 320x240 is this window's OWN resize floor, it is
            // reachable, and there the text wraps to four lines, juce::Label ellipsises
            // it, and what survives on screen is "...is FROZEN where it stood. Switch
            // manual" with the half of the sentence that says HOW TO UNDO IT simply gone.
            // No gate saw it, because nothing overflowed its bounds.
            //
            // No floor of 32 to go with it: an empty banner still takes 0, and a one-line
            // banner should cost one line rather than a seventh of a small window.
            warnLbl.setBounds (r.removeFromTop (warnLbl.getText().isEmpty()
                                                    ? 0 : fgfit::labelHeight (warnLbl, r.getWidth())));
            r.removeFromTop (4);
            view.setBounds (r);
            layoutGrid();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff0e1118));
            g.setColour (juce::Colour (0xff5cb8ff).withAlpha (0.25f));
            g.drawHorizontalLine (view.getY() - 3, (float) view.getX(), (float) view.getRight());
        }

        // Clicking anywhere empty drops the selection - the same "click outside to
        // deselect" every list in this plugin has.
        void mouseDown (const juce::MouseEvent&) override
        { grid.model.clearSelection(); grid.repaint(); }
    };

    //==========================================================================
    // THE PRESET PLAYGROUND.
    //
    // Four presets on the corners of a rectangle and a draggable point inside it. The
    // point is nothing but a view onto the pgX / pgY PARAMETERS: dragging writes them
    // through beginChangeGesture / setValueNotifyingHost / endChangeGesture so the host
    // records the move, and automation played back moves the point here - or, with this
    // window shut, moves everything it drives anyway, because the blending itself lives
    // in the processor's 60 Hz poll and not in this component.
    //
    // The COLOURS are the feature's vocabulary: each corner owns one, the point is
    // tinted by the weights it is currently made of, and the corner that owns every
    // DISCRETE setting (engine, phase mode, manual amps, realtime arming - things that
    // cannot be averaged) wears a ring. Without that ring "why did the engine change
    // when I crossed the middle?" has no visible answer at all.
    static juce::Colour playgroundCornerColour (int i)
    {
        static const juce::Colour c[4] { juce::Colour (0xff5cb8ff), juce::Colour (0xffffb020),
                                         juce::Colour (0xff4ed08a), juce::Colour (0xffe07ad0) };
        return c[i & 3];
    }

    struct PlaygroundPad : juce::Component, juce::SettableTooltipClient
    {
        FractalAdditiveAudioProcessor* proc = nullptr;
        std::function<juce::String (const char*, const char*)> trF;
        std::function<void()> onMoved;
        bool dragging = false;
        // THE LABEL AS DRAWN, per corner, with the box it was drawn in - the pad paints
        // these itself, so walkTextFits cannot reach them and a silent cut here is
        // invisible to every gate. Recorded in paint() and read by the text-fit probe.
        mutable juce::String lastLabel[4];
        mutable juce::Rectangle<int> lastLabelBox[4];

        // The square the point actually travels in. Inset so a corner's dot and its
        // percentage are drawn INSIDE the component and never clipped by its edge.
        juce::Rectangle<float> padArea() const
        { return getLocalBounds().toFloat().reduced (26.0f); }

        void writePoint (juce::Point<float> p)
        {
            const auto a = padArea();
            const float x = a.getWidth()  > 1.0f ? juce::jlimit (0.0f, 1.0f, (p.x - a.getX()) / a.getWidth())  : 0.0f;
            const float y = a.getHeight() > 1.0f ? juce::jlimit (0.0f, 1.0f, (p.y - a.getY()) / a.getHeight()) : 0.0f;
            if (proc == nullptr) return;
            for (auto* id : { "pgX", "pgY" })
                if (auto* q = proc->apvts.getParameter (id))
                    q->setValueNotifyingHost (juce::String (id) == "pgX" ? x : y);
            repaint();
            if (onMoved) onMoved();
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (proc == nullptr) return;
            // ONE gesture per drag, opened before the first value goes out: a host in
            // touch/latch mode only starts writing automation when it sees the gesture,
            // and only stops when it sees the end - so without this pair a recorded drag
            // is either nothing at all or never stops recording.
            dragging = true;
            for (auto* id : { "pgX", "pgY" })
                if (auto* q = proc->apvts.getParameter (id)) q->beginChangeGesture();
            writePoint (e.position);
        }
        void mouseDrag (const juce::MouseEvent& e) override { if (dragging) writePoint (e.position); }
        void mouseUp (const juce::MouseEvent&) override
        {
            if (! dragging || proc == nullptr) return;
            dragging = false;
            for (auto* id : { "pgX", "pgY" })
                if (auto* q = proc->apvts.getParameter (id)) q->endChangeGesture();
        }

        juce::String getTooltip() override
        {
            if (trF == nullptr) return {};
            return trF ("Drag the point. Each corner is a preset and the point is the blend of them - "
                        "the SOUND only, never the tuning. Its position is the automatable pgX / pgY "
                        "parameters, so a move recorded in the DAW plays back with this window shut.",
                        "Arrossega el punt. Cada cantonada \xc3\xa9s un preset i el punt \xc3\xa9s la seva barreja - "
                        "nom\xc3\xa9s el SO, mai l'afinaci\xc3\xb3. La seva posici\xc3\xb3 s\xc3\xb3n els par\xc3\xa0metres "
                        "automatitzables pgX / pgY, o sigui que un moviment gravat al DAW es reprodueix amb "
                        "aquesta finestra tancada.");
        }

        void paint (juce::Graphics& g) override
        {
            const auto a = padArea();
            g.setColour (juce::Colour (0xff141a24));
            g.fillRoundedRectangle (a, 6.0f);
            g.setColour (juce::Colour (0xff55557a));
            g.drawRoundedRectangle (a.reduced (0.5f), 6.0f, 1.0f);
            // Quarter lines: the middle of each edge and the centre, so the eye can see
            // where "half way" is without measuring.
            g.setColour (juce::Colour (0xff2a3140));
            g.drawLine (a.getCentreX(), a.getY(), a.getCentreX(), a.getBottom(), 1.0f);
            g.drawLine (a.getX(), a.getCentreY(), a.getRight(), a.getCentreY(), 1.0f);

            if (proc == nullptr) return;

            double w[FractalAdditiveAudioProcessor::kPlaygroundCorners];
            proc->getPlaygroundWeights (w);
            const int winner = proc->getPlaygroundNearestCorner();
            const bool active = proc->isPlaygroundActive();

            const int editing = proc->getPlaygroundEditCorner();
            for (int i = 0; i < 4; ++i)
            {
                // The DISPLAY name: a corner carrying unsaved edits reads "name*" here,
                // which is the one place the user is looking while dragging the point.
                const auto name = proc->getPlaygroundCornerDisplayName (i);
                const juce::Point<float> c { (i & 1) ? a.getRight() : a.getX(),
                                             (i & 2) ? a.getBottom() : a.getY() };
                const auto col = playgroundCornerColour (i);
                const float ww = (float) (active ? w[i] : (name.isNotEmpty() ? 0.0 : 0.0));
                // Radius grows with the weight - the size of a corner's dot IS how much
                // of it you are hearing. Empty corners stay a hollow outline.
                const float rad = name.isEmpty() ? 7.0f : 7.0f + 13.0f * ww;
                if (name.isEmpty())
                {
                    g.setColour (juce::Colour (0xff3a4152));
                    g.drawEllipse (juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (c), 1.5f);
                }
                else
                {
                    g.setColour (col.withAlpha (0.30f + 0.70f * ww));
                    g.fillEllipse (juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (c));
                }
                // The corner every DISCRETE setting is coming from - a MORPH-mode
                // concept only. In MIX mode every corner is its own engine and no
                // setting comes "from" anywhere, so the ring would be a lie there.
                if (i == winner && active && ! proc->isPlaygroundMixMode())
                {
                    g.setColour (juce::Colours::white.withAlpha (0.85f));
                    g.drawEllipse (juce::Rectangle<float> (rad * 2.0f + 8.0f, rad * 2.0f + 8.0f).withCentre (c), 2.0f);
                }
                // The corner the main window's controls are standing in for: an amber
                // ring, the same colour the Edit button turns, so "where am I typing?"
                // has an answer on the pad itself and not only in the status text.
                if (i == editing)
                {
                    g.setColour (juce::Colour (0xffffc95c));
                    g.drawEllipse (juce::Rectangle<float> (rad * 2.0f + 14.0f, rad * 2.0f + 14.0f).withCentre (c), 2.5f);
                }

                // Name + percentage, pushed OUTWARDS from the corner. THREE PIECES,
                // because only one of them may be cut: the letter says WHICH corner and
                // the percentage says HOW MUCH of it you are hearing - that number is the
                // entire reason this pad exists - so the NAME is the elastic part and the
                // name is what ellipsises.
                //
                // It used to be one string in a fixed 150 px box drawn with
                // drawText (..., useEllipsis = false), which cut the tail. With a real
                // preset name on the corner the tail is the percentage: "campana que li
                // mola a l'Alfred" came out as "C  campana que li mola a l'" and the
                // number was simply gone. Photographed by scenario 52, which is the first
                // picture of this panel with anything loaded in it.
                //
                // The box also stops a puck-radius short of the corner now. The old one
                // was centred 58 px away and 150 wide, so it reached 17 px PAST the
                // corner and the last of the text sat under the dot - which the comment
                // above it said it never did.
                const juce::Font labFont (juce::FontOptions (12.0f).withStyle ("Bold"));
                const auto lead = juce::String::charToString ((juce::juce_wchar) ('A' + i));
                const juce::String head = lead + "  ";
                const juce::String tail = (active && ! name.isEmpty())
                                        ? "  " + juce::String (w[i] * 100.0, 0) + "%"
                                        : juce::String();
                const float gap  = rad + 5.0f;
                const float room = juce::jmax (44.0f, a.getWidth() * 0.5f - gap - 4.0f);
                auto lab = juce::Rectangle<float> (room, 15.0f)
                               .withCentre ({ 0.0f, c.y + ((i & 2) ? 17.0f : -17.0f) });
                lab.setX ((i & 1) ? c.x - gap - room : c.x + gap);

                juce::String shown = name.isEmpty()
                                   ? (trF ? trF ("empty", "buida") : juce::String ("empty"))
                                   : name;
                const float fixed = juce::GlyphArrangement::getStringWidth (labFont, head + tail);
                const float forName = room - fixed - 2.0f;
                if (juce::GlyphArrangement::getStringWidth (labFont, shown) > forName)
                {
                    const auto dots = juce::String::fromUTF8 ("\xe2\x80\xa6");
                    while (shown.length() > 1
                           && juce::GlyphArrangement::getStringWidth (labFont, shown + dots) > forName)
                        shown = shown.dropLastCharacters (1);
                    shown = shown.trimEnd() + dots;
                }
                g.setColour (name.isEmpty() ? juce::Colour (0xff6a7285) : col);
                g.setFont (labFont);
                lastLabel[i]    = head + shown + tail;
                lastLabelBox[i] = lab.toNearestInt();
                g.drawText (head + shown + tail, lab.toNearestInt(),
                            (i & 1) ? juce::Justification::centredRight : juce::Justification::centredLeft, false);
            }

            // The point, tinted by what it is made of right now.
            const float px = a.getX() + a.getWidth()  * proc->apvts.getRawParameterValue ("pgX")->load();
            const float py = a.getY() + a.getHeight() * proc->apvts.getRawParameterValue ("pgY")->load();
            float r = 0.0f, gg = 0.0f, b = 0.0f, sum = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                const auto col = playgroundCornerColour (i);
                r  += (float) w[i] * col.getFloatRed();
                gg += (float) w[i] * col.getFloatGreen();
                b  += (float) w[i] * col.getFloatBlue();
                sum += (float) w[i];
            }
            const auto pc = sum > 0.001f ? juce::Colour::fromFloatRGBA (r / sum, gg / sum, b / sum, 1.0f)
                                         : juce::Colour (0xff8f96a8);
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillEllipse (juce::Rectangle<float> (20.0f, 20.0f).withCentre ({ px, py }));
            g.setColour (pc);
            g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ px, py }));
            g.setColour (juce::Colours::white);
            g.drawEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ px, py }), 2.0f);
        }
    };

    struct PlaygroundPanel : juce::Component, private juce::Timer
    {
        FractalAdditiveAudioProcessor* proc = nullptr;
        std::function<juce::String (const char*, const char*)> trF;
        // ...and the ONE hook that turns a processor refusal into the reader's language.
        // The status line below dumps getPlaygroundAdaptRefusal() straight in, and that
        // string is composed by the PROCESSOR, which has no language: without this the
        // Catalan reader gets an English sentence in the middle of a Catalan panel. Same
        // shape as trF, wired from the editor beside it.
        std::function<juce::String (const juce::String&)> trErrF;

        juce::Label        headLbl, statLbl;
        juce::Label        cornerLbl[4];
        RefreshingComboBox cornerBox[4];
        juce::TextButton   cornerClear[4];
        // EDIT: hands the MAIN editor's controls to this corner (see
        // FractalAdditiveAudioProcessor::beginPlaygroundCornerEdit for the ownership
        // rules). One corner at a time; the button of the corner being edited reads
        // "OK" and lights up, because "how do I get my own patch back" must be visible
        // from the same place the trip started.
        juce::TextButton   cornerEdit[4];
        // Set by the editor: edit mode began or ended - the main window's preset field
        // and Save button have to change with it.
        std::function<void()> onEditModeChanged;
        // The playground's two modes, as a radio pair driving the pgMode PARAMETER
        // (host-automatable; the buttons are only a view onto it, like the pad onto
        // pgX/pgY): MORPH blends one patch, MIX sounds all four corners at once and
        // the point becomes their volume balance.
        juce::TextButton   modeBtn[2];
        // MIX's per-corner polyphony (the "pgVoices" parameter). VISIBLE by design:
        // five engines at 32 voices each measured ~3x a 48 kHz/512 block on dense
        // spectra, and the fidelity rule is that a CPU limit is a control the user
        // holds, never a cap the code hides.
        juce::Label        voicesLbl;
        juce::String       voicesBaseText;   // see the mono-glide suffix in the timer
        juce::Slider       voicesSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> voicesAtt;
        PlaygroundPad      pad;

        // Called once the processor pointer is set (attachments need the APVTS).
        void attachVoices()
        {
            if (proc == nullptr || voicesAtt != nullptr) return;
            voicesAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                            proc->apvts, "pgVoices", voicesSlider);
        }
        // Its own TooltipWindow, parented HERE: the editor's serves only its own
        // subtree, so in a separate desktop window every hover read-out is invisible
        // without this (the same trap PartialAmpPanel documents).
        juce::TooltipWindow tooltips { this, 600 }; // the editor's own number - see tooltipWindow
        float lastX = -1.0f, lastY = -1.0f;
        int   lastAssigned = -1, lastWinner = -2, lastMode = -1;
        juce::String lastNamesSig;   // every corner's display name: Ctrl+Z can change one with nothing else moving
        // Adaptive-corner state the status line reports: count + whether a refusal is
        // showing. In the change detection because a corner can be SWAPPED without the
        // assigned count moving, and a stale line would claim a follower that is gone.
        int   lastAdaptSig = -1;
        // Which corner is being edited + which carry a "*", as one number (see refresh).
        int   lastEditSig = -1;

        PlaygroundPanel()
        {
            headLbl.setFont (juce::Font (juce::FontOptions (15.0f).withStyle ("Bold")));
            headLbl.setColour (juce::Label::textColourId, juce::Colour (0xff5cb8ff));
            headLbl.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (headLbl);

            statLbl.setFont (juce::Font (juce::FontOptions (12.0f)));
            statLbl.setColour (juce::Label::textColourId, juce::Colour (0xffb8c0d0));
            statLbl.setJustificationType (juce::Justification::topLeft);
            statLbl.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (statLbl);

            for (int i = 0; i < 4; ++i)
            {
                cornerLbl[i].setText (juce::String::charToString ((juce::juce_wchar) ('A' + i)),
                                      juce::dontSendNotification);
                cornerLbl[i].setFont (juce::Font (juce::FontOptions (14.0f).withStyle ("Bold")));
                cornerLbl[i].setColour (juce::Label::textColourId, playgroundCornerColour (i));
                cornerLbl[i].setJustificationType (juce::Justification::centred);
                // A single bold letter in a 16 px box: JUCE's default 5 px side border
                // left 6 px for a glyph that measures 8-10, so even "A" was condensed.
                cornerLbl[i].setBorderSize (juce::BorderSize<int> (0, 1, 0, 1));
                cornerLbl[i].setInterceptsMouseClicks (false, false);
                addAndMakeVisible (cornerLbl[i]);

                cornerBox[i].setColour (juce::ComboBox::outlineColourId, playgroundCornerColour (i).withAlpha (0.6f));
                cornerBox[i].setMouseClickGrabsKeyboardFocus (false);
                cornerBox[i].onBeforePopup = [this, i] { refillCorner (i); };
                cornerBox[i].onChange = [this, i]
                {
                    if (proc == nullptr) return;
                    juce::String err;
                    // Item 1 is "(empty)" - selecting it CLEARS the corner. Anything else
                    // is a preset name read straight off the folder listing.
                    const auto name = cornerBox[i].getSelectedId() <= 1 ? juce::String() : cornerBox[i].getText();
                    // Dropping a different preset here REPLACES the corner from disk, so
                    // it ends any edit of it (and throws the unsaved edits away - the one
                    // explicit act that does, see rule 4). Tell the main window.
                    const bool wasEditing = proc->getPlaygroundEditCorner() == i;
                    noteAssign (i, proc->assignPlaygroundCornerUndoable (i, name, err), err);   // one Ctrl+Z step
                    refresh (true);
                    if (wasEditing && onEditModeChanged) onEditModeChanged();
                };
                addAndMakeVisible (cornerBox[i]);

                cornerEdit[i].setButtonText ("E");
                cornerEdit[i].setMouseClickGrabsKeyboardFocus (false);
                cornerEdit[i].onClick = [this, i]
                {
                    if (proc == nullptr) return;
                    if (proc->getPlaygroundEditCorner() == i) proc->endPlaygroundCornerEdit();
                    else                                      proc->beginPlaygroundCornerEdit (i);
                    refresh (true);
                    if (onEditModeChanged) onEditModeChanged();
                };
                addAndMakeVisible (cornerEdit[i]);

                cornerClear[i].setButtonText ("X");
                cornerClear[i].setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe0453f).withAlpha (0.5f));
                cornerClear[i].setMouseClickGrabsKeyboardFocus (false);
                cornerClear[i].onClick = [this, i]
                {
                    if (proc == nullptr) return;
                    juce::String err;
                    // assignPlaygroundCorner ends edit mode on this corner by itself (it
                    // is what discards unsaved edits), but the MAIN window still has to
                    // be told, or it would keep showing a frozen preset field for a
                    // corner that no longer exists.
                    const bool wasEditing = proc->getPlaygroundEditCorner() == i;
                    noteAssign (i, proc->assignPlaygroundCornerUndoable (i, {}, err), err);   // one Ctrl+Z step
                    cornerBox[i].setSelectedId (1, juce::dontSendNotification);
                    refresh (true);
                    if (wasEditing && onEditModeChanged) onEditModeChanged();
                };
                addAndMakeVisible (cornerClear[i]);
            }

            for (int m = 0; m < 2; ++m)
            {
                modeBtn[m].setMouseClickGrabsKeyboardFocus (false);
                modeBtn[m].onClick = [this, m]
                {
                    if (proc == nullptr) return;
                    // Through the parameter, with a gesture, so the host records the
                    // switch and Ctrl+Z steps back over it like any other control.
                    if (auto* p = proc->apvts.getParameter ("pgMode"))
                    {
                        p->beginChangeGesture();
                        p->setValueNotifyingHost (p->convertTo0to1 ((float) m));
                        p->endChangeGesture();
                    }
                    refresh (true);
                };
                addAndMakeVisible (modeBtn[m]);
            }

            voicesLbl.setFont (juce::Font (juce::FontOptions (12.0f)));
            voicesLbl.setColour (juce::Label::textColourId, juce::Colour (0xff8f96a8));
            voicesLbl.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (voicesLbl);
            voicesSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 34, 18);
            voicesSlider.setMouseClickGrabsKeyboardFocus (false);
            addAndMakeVisible (voicesSlider);

            pad.onMoved = [this] { refresh (false); };
            addAndMakeVisible (pad);
            startTimerHz (30);
        }

        // PUTTING A PRESET ON A CORNER CAN FAIL, AND IT USED TO FAIL IN SILENCE.
        //
        // Both call sites declared a `juce::String err`, handed it to
        // assignPlaygroundCorner, and then threw away the error AND the bool - so the
        // three ways the processor refuses ("that preset no longer exists", "could not be
        // read", "not a Fibl\xc3\xb3 preset", see PluginProcessor.cpp) reached nobody. From
        // the chair it is the worst shape a failure can have: the drop-down keeps showing
        // the name you picked, because the box's text is the box's own, while the corner
        // behind it was CLEARED - assignPlaygroundCorner empties the corner before it
        // reads the file. The pad then blends three corners instead of four and the panel
        // is telling you it has four. That is not a rare state either: the list is the
        // preset FOLDER, so anything that moves a file behind the plugin's back - a
        // rename, a sync client, a second instance saving - produces exactly this.
        //
        // Kept as (corner, reason) rather than as a finished sentence so refresh() builds
        // it in whatever language is on screen at the time; a message parked in the words
        // of the language it happened in is the same defect one level down. The reason
        // itself is the PROCESSOR's, and the processor has no tr() - so it arrives in
        // English and is passed through verbatim rather than paraphrased: half a truth in
        // the right language is worse than the loader's own words.
        void noteAssign (int corner, bool ok, const juce::String& why)
        {
            assignFailedCorner = ok ? -1 : corner;
            assignFailedWhy    = ok ? juce::String() : why;
        }

        void refillCorner (int i)
        {
            if (proc == nullptr) return;
            // The DISPLAY name, so a corner carrying unsaved edits shows its "*" here
            // too. The box's ITEMS stay the plain folder listing - the "*" is a state of
            // this corner, not the name of a preset anyone could pick.
            const auto cur = proc->getPlaygroundCornerDisplayName (i);
            cornerBox[i].clear (juce::dontSendNotification);
            cornerBox[i].addItem (trF ? trF ("(empty)", "(buida)") : juce::String ("(empty)"), 1);
            int id = 2;
            for (const auto& n : proc->getPresetNames())
                cornerBox[i].addItem (n, id++);
            if (cur.isNotEmpty()) cornerBox[i].setText (cur, juce::dontSendNotification);
            else                  cornerBox[i].setSelectedId (1, juce::dontSendNotification);
        }

        void applyLanguage()
        {
            headLbl.setText (trF ? trF ("PRESET PLAYGROUND", "LABORATORI DE PRESETS")
                                 : juce::String ("PRESET PLAYGROUND"), juce::dontSendNotification);
            // MORPHING in Catalan, like the SPECTRUM panel's own MORPH sub-label
            // (tr("MORPH","MORPHING") in layoutProbe's painted labels). It is the same
            // idea named twice in one instrument, and it printed two different words.
            modeBtn[0].setButtonText (trF ? trF ("MORPH", "MORPHING") : juce::String ("MORPH"));
            modeBtn[1].setButtonText (trF ? trF ("MIX", "MESCLA") : juce::String ("MIX"));
            modeBtn[0].setTooltip (trF ? trF ("The corners blend into ONE patch - every control and the "
                                              "spectrum move to the point.",
                                              "Les cantonades es fonen en UN sol patch - cada control "
                                              "i l'espectre segueixen el punt.")
                                       : juce::String());
            modeBtn[1].setTooltip (trF ? trF ("The four presets SOUND AT ONCE, each as its own full engine, "
                                              "and the point balances their volumes (equal power). Your own "
                                              "patch is untouched and comes back when you leave.",
                                              "Els quatre presets SONEN ALHORA, cadascun amb el seu propi "
                                              "motor complet, i el punt equilibra els seus volums (pot\xc3\xa8ncia "
                                              "constant). El teu patch no es toca i torna quan en surts.")
                                       : juce::String());
            // Kept as a member because the mono-glide state appends to it every tick, and
            // re-deriving it there would need trF at a point where the language may have
            // changed since - the suffix must never end up bolted onto itself.
            voicesBaseText = trF ? trF ("Mix voices per corner", "Veus de mescla per cantonada")
                                 : juce::String ("Mix voices per corner");
            voicesLbl.setText (voicesBaseText, juce::dontSendNotification);
            voicesSlider.setTooltip (trF ? trF ("Polyphony of EACH corner engine in mix mode. Mix runs up to five "
                                                "engines at once (four corners plus your patch tracking "
                                                "underneath), so this is the CPU trade-off - a visible one, never "
                                                "a hidden cap. 8 covers normal playing; raise it if your CPU can.",
                                                "Polifonia de CADA motor de cantonada en mode mescla. La mescla fa "
                                                "anar fins a cinc motors alhora (quatre cantonades m\xc3\xa9s el teu "
                                                "patch seguint per sota), o sigui que aix\xc3\xb2 \xc3\xa9s l'equilibri "
                                                "de CPU - visible, mai un l\xc3\xadmit amagat. 8 cobreix el joc "
                                                "normal; puja-ho si la teva CPU pot.")
                                         : juce::String());
            for (int i = 0; i < 4; ++i)
            {
                refillCorner (i);
                cornerBox[i].setTooltip (trF ? trF ("The preset on this corner. Drop up to four; two make a line, "
                                                    "three a triangle, four the whole rectangle. An empty corner "
                                                    "contributes nothing at all.\n\n"
                                                    "The corner keeps its OWN COPY of the preset, and a preset or "
                                                    "project saved with the pad carries every corner's copy - so it "
                                                    "opens the same on another computer, and does not change if the "
                                                    "preset file is later edited, renamed or deleted. To take in the "
                                                    "preset's current file instead, press X and pick the preset again.",
                                                    "El preset d'aquesta cantonada. Posa'n fins a quatre; dos fan una "
                                                    "l\xc3\xadnia, tres un triangle, quatre tot el rectangle. Una "
                                                    "cantonada buida no hi aporta res.\n\n"
                                                    "La cantonada guarda la seva PR\xc3\x92PIA C\xc3\x92PIA del preset, i "
                                                    "un preset o projecte desat amb el pad porta la c\xc3\xb2pia de cada "
                                                    "cantonada - o sigui que s'obre igual en un altre ordinador, i no "
                                                    "canvia si despr\xc3\xa9s s'edita, es reanomena o s'esborra el fitxer "
                                                    "del preset. Per agafar el fitxer actual del preset, prem X i "
                                                    "torna'l a triar.")
                                              : juce::String());
                cornerClear[i].setTooltip (trF ? trF ("Take the preset off this corner.",
                                                      "Treu el preset d'aquesta cantonada.")
                                               : juce::String());
                cornerEdit[i].setTooltip (trF ? trF ("Edit this corner. The MAIN window's controls become this "
                                                     "corner's, live: what you change is heard in the blend "
                                                     "straight away and the corner's name gets a * to say it no "
                                                     "longer matches the preset it came from. Your own patch is "
                                                     "kept aside and comes back untouched when you press OK. "
                                                     "Save (main window) stores the corner under a new name.",
                                                     "Edita aquesta cantonada. Els controls de la finestra "
                                                     "PRINCIPAL passen a ser els d'aquesta cantonada, en directe: "
                                                     "el que canvi\xc3\xafs se sent a la barreja de seguida i el nom "
                                                     "de la cantonada rep un * per dir que ja no coincideix amb el "
                                                     "preset d'on ve. El teu patch es guarda a part i torna intacte "
                                                     "quan prems OK. Desar (finestra principal) guarda la cantonada "
                                                     "amb un nom nou.")
                                              : juce::String());
            }
            lastWinner = -2;   // force the status line to be rebuilt in the new language
            refresh (true);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (10);
            // The mode pair shares the header row, so the pad loses no height to it.
            auto head = r.removeFromTop (20);
            // BOTH mode buttons take the width the WIDER of the two words needs, floor
            // 64: MORPH fitted 64 px and MORPHING (its Catalan) does not, and a pair of
            // buttons that are the same control in two states must stay the same size.
            // Measured at the height they are about to be given, not at the height they
            // currently have - on the first pass that is still zero.
            const int mbH = juce::jmax (1, head.getHeight() - 2);
            const int mbW = juce::jmax (64, fgfit::textButtonWidth (modeBtn[0].getButtonText(), mbH, false),
                                            fgfit::textButtonWidth (modeBtn[1].getButtonText(), mbH, false));
            modeBtn[1].setBounds (head.removeFromRight (mbW).reduced (0, 1));
            head.removeFromRight (3);
            modeBtn[0].setBounds (head.removeFromRight (mbW).reduced (0, 1));
            head.removeFromRight (6);
            headLbl.setBounds (head);
            r.removeFromTop (4);
            statLbl.setBounds (r.removeFromBottom (58));
            r.removeFromBottom (4);
            // MIX's voice budget rides just above the status text; the pad keeps the rest.
            {
                auto vr = r.removeFromBottom (20);
                voicesSlider.setBounds (vr.removeFromRight (96).reduced (0, 1));
                vr.removeFromRight (4);
                voicesLbl.setBounds (vr);
                r.removeFromBottom (2);
            }

            const auto row = [this] (int lo, int hi, juce::Rectangle<int> band)
            {
                auto left  = band.removeFromLeft (band.getWidth() / 2).reduced (2, 0);
                auto right = band.reduced (2, 0);
                const auto lay = [this] (int i, juce::Rectangle<int> cell)
                {
                    cornerLbl[i].setBounds (cell.removeFromLeft (16));
                    // Right to left: X (take the preset off), then E (edit it), then the
                    // name box gets everything left. The two buttons are sized from the
                    // RIGHT so adding E could only ever cost the box width - never push a
                    // control off the end of the row.
                    cornerClear[i].setBounds (cell.removeFromRight (24).reduced (1, 1));
                    cell.removeFromRight (3);
                    cornerEdit[i].setBounds (cell.removeFromRight (26).reduced (1, 1));
                    cell.removeFromRight (3);
                    cornerBox[i].setBounds (cell);
                };
                lay (lo, left);
                lay (hi, right);
            };
            row (0, 1, r.removeFromTop (24));
            row (2, 3, r.removeFromBottom (24));
            pad.setBounds (r.reduced (0, 4));
        }

        void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff0e1118)); }

        // Automation moves the point with this window open, and a preset dropped on a
        // corner changes what the pad has to draw. Both arrive from OUTSIDE this
        // component, so it polls rather than waiting to be told.
        void timerCallback() override { refresh (false); }

        void refresh (bool force)
        {
            if (proc == nullptr) return;
            const float x = proc->apvts.getRawParameterValue ("pgX")->load();
            const float y = proc->apvts.getRawParameterValue ("pgY")->load();
            const int   n = proc->getPlaygroundAssignedCount();
            const int   wn = proc->getPlaygroundNearestCorner();
            const int   adSig = proc->getPlaygroundAdaptiveCount() * 2
                              + (proc->getPlaygroundAdaptRefusal().isNotEmpty() ? 1 : 0);
            const int   mode  = proc->isPlaygroundMixMode() ? 1 : 0;
            // WHICH corner is being edited, and which corners carry unsaved edits. In the
            // change detector because the "*" can appear with nothing else moving at all
            // (a knob turned in the main window is invisible to every other signal here),
            // and a status line that only redraws on a drag would deny the edit exists.
            int editSig = proc->getPlaygroundEditCorner() + 1;
            for (int i = 0; i < 4; ++i)
                if (proc->isPlaygroundCornerEdited (i)) editSig += 16 << i;
            // WHICH PRESET IS ON EACH CORNER, too. A pick and an X are undo steps now, so Ctrl+Z
            // (from either window, or the host) can swap a corner's preset with every signal
            // above standing still - and the box went on naming the preset just taken back,
            // the one mismatch noteAssign's comment calls the worst (review, 2026-09-15).
            juce::String namesSig;
            for (int i = 0; i < 4; ++i)
                namesSig << proc->getPlaygroundCornerDisplayName (i) << "\n";
            if (! force && x == lastX && y == lastY && n == lastAssigned && wn == lastWinner
                && adSig == lastAdaptSig && mode == lastMode && editSig == lastEditSig
                && namesSig == lastNamesSig)
                return;
            lastX = x; lastY = y; lastAssigned = n; lastWinner = wn; lastAdaptSig = adSig;
            lastMode = mode;
            const bool editChanged  = editSig != lastEditSig;
            const bool namesChanged = namesSig != lastNamesSig;
            lastEditSig  = editSig;
            lastNamesSig = namesSig;

            // The corner rows: only an ASSIGNED corner has anything to edit, and only
            // while the playground is really driving (two corners) does editing one mean
            // anything - the blend is what you hear the edit in.
            {
                const int ed = proc->getPlaygroundEditCorner();
                for (int i = 0; i < 4; ++i)
                {
                    const bool can = proc->getPlaygroundCornerName (i).isNotEmpty() && n >= 2;
                    cornerEdit[i].setEnabled (can);
                    cornerEdit[i].setButtonText (i == ed ? juce::String ("OK") : juce::String ("E"));
                    cornerEdit[i].setColour (juce::TextButton::buttonColourId,
                                             i == ed ? juce::Colour (0xffffc95c) : juce::Colour (0xff2d6da8).withAlpha (0.55f));
                    cornerEdit[i].setColour (juce::TextButton::textColourOffId,
                                             i == ed ? juce::Colours::black : juce::Colours::white);
                    // The "*" lives on the name, so the box has to be re-read whenever the
                    // edit signature moved - not only when a corner is (re)assigned.
                    if (editChanged || namesChanged)
                    {
                        const auto disp = proc->getPlaygroundCornerDisplayName (i);
                        if (disp.isNotEmpty())
                        {
                            if (cornerBox[i].getText() != disp)
                                cornerBox[i].setText (disp, juce::dontSendNotification);
                        }
                        else if (namesChanged && cornerBox[i].getSelectedId() != 1)
                            cornerBox[i].setSelectedId (1, juce::dontSendNotification);   // "(empty)"
                    }
                }
            }

            // The mode pair reads like a radio group: the mode in force is lit.
            for (int m = 0; m < 2; ++m)
            {
                modeBtn[m].setColour (juce::TextButton::buttonColourId,
                                      m == mode ? juce::Colour (0xff2d6da8) : juce::Colour (0xff1a2130));
                modeBtn[m].setColour (juce::TextButton::textColourOffId,
                                      m == mode ? juce::Colours::white : juce::Colour (0xff8f96a8));
            }
            // The voice budget only means anything in mix mode; a control that quietly
            // does nothing is the trap this project keeps refusing, so it greys out.
            // MONOPHONIC GLIDE takes it away for the same reason and SAYS SO: while mono
            // is on every corner engine runs one voice, and a control still showing 8
            // would be claiming a polyphony that is not happening.
            // (The status line below says it in words - a label rewritten to carry the
            // reason would have had to grow, and this row has no width to give.)
            const bool monoNow = proc != nullptr
                              && proc->apvts.getRawParameterValue ("monoGlide")->load() > 0.5f;
            // ONE STATE, ONE APPEARANCE. setEnabled dims the two stepper buttons - JUCE's
            // TextButton honours it - but NOT the value box: that box is a juce::Label fed
            // from Slider::textBoxTextColourId, and nothing changes that colour on disable.
            // Measured off the rendered window in MORPH mode, where this control is off:
            // the caption's brightest ink is #2F333D, the +/- glyphs are #4E5459 and THE
            // NUMBER is #8E989B - three brightnesses for one disabled control, with the
            // read-out the brightest of the three. A number at full ink beside a greyed
            // caption reads as live, and this one is not.
            //
            // setAlpha on the whole slider rather than a second colour: it covers the box,
            // both buttons and anything added to this control later, and it is driven by
            // the same expression as the caption beside it, so the two cannot drift apart.
            const bool voicesLive = mode == 1 && ! monoNow;
            voicesSlider.setEnabled (voicesLive);
            voicesSlider.setAlpha (voicesLive ? 1.0f : 0.45f);
            voicesLbl.setAlpha (voicesLive ? 1.0f : 0.45f);
            // ...and SAY WHICH NUMBER IS IN FORCE. Greying the control tells the user it is
            // not theirs to move; it does not stop the box reading 8 while every corner
            // engine is actually running ONE voice, which is a control displaying a number
            // that is not the one the synth is using. The status line explains the rule in
            // prose, but prose below a box showing "8" loses to the box. The suffix is on
            // the LABEL rather than the slider's own text box because that box is driven by
            // the parameter attachment - writing a display value there would either fight
            // the attachment or, worse, move the parameter and lose what the user chose.
            voicesLbl.setText (voicesBaseText
                                   + (monoNow ? (trF ? trF (" - 1 while glide is on",
                                                            " - 1 mentre el glide \xc3\xa9s actiu")
                                                     : juce::String (" - 1 while glide is on"))
                                              : juce::String()),
                               juce::dontSendNotification);

            juce::String s;
            if (n < 2)
            {
                s << (trF ? trF ("Drop a preset on at least TWO corners. With one there is nothing to blend "
                                 "with, so the playground stays out of the way and leaves your patch alone.",
                                 "Posa un preset a com a m\xc3\xadnim DUES cantonades. Amb una no hi ha res amb "
                                 "qu\xc3\xa8 barrejar, aix\xc3\xad que el laboratori no toca res i et deixa el patch "
                                 "tal com el tens.")
                          : juce::String());
            }
            else
            {
                double w[FractalAdditiveAudioProcessor::kPlaygroundCorners];
                proc->getPlaygroundWeights (w);
                s << (mode == 1 ? (trF ? trF ("Mix: ", "Mescla: ") : juce::String ("Mix: "))
                                : (trF ? trF ("Blend: ", "Barreja: ") : juce::String ("Blend: ")));
                bool first = true;
                for (int i = 0; i < 4; ++i)
                    if (proc->getPlaygroundCornerName (i).isNotEmpty())
                    {
                        if (! first) s << " + ";
                        first = false;
                        s << (char) ('A' + i) << " " << juce::String (w[i] * 100.0, 0) << "%";
                    }
                s << "\n";
                if (mode == 1)
                {
                    // MIX: no winner corner, no morphed controls - four whole engines
                    // and a balance. Say exactly that, and that the user's patch waits
                    // untouched underneath.
                    s << (trF ? trF ("All four corners SOUND AT ONCE (equal-power balance). Your own "
                                     "patch is silent but untouched - it comes back exactly as it was.",
                                     "Totes les cantonades SONEN ALHORA (balan\xc3\xa7 de pot\xc3\xa8ncia "
                                     "constant). El teu patch calla per\xc3\xb2 no es toca - torna "
                                     "exactament com era.")
                              : juce::String());
                    // MONOPHONIC GLIDE reaches every corner engine, because there is ONE
                    // note stream and they all hear it. Said here rather than left for
                    // the user to notice: the polyphony control they can see is disabled
                    // and a silent override is exactly what this panel refuses to do.
                    if (monoNow)
                        s << (trF ? trF (" Monophonic glide is ON, so EVERY corner engine runs one voice "
                                         "and glides the same way - \"Mix voices per corner\" waits.",
                                         " El glide monof\xc3\xb2nic \xc3\xa9s actiu, aix\xc3\xad que CADA motor de "
                                         "cantonada porta una sola veu i llisca igual - \"Veus de mescla "
                                         "per cantonada\" espera.")
                                  : juce::String());
                    s << "\n";
                }
                else if (wn >= 0)
                    s << (trF ? trF ("Settings that cannot be averaged (engine, phase, prime mode, manual amps, "
                                     "realtime) come from ", "Els ajustos que no es poden promitjar (motor, fase, mode "
                                     "primer, amplituds manuals, temps real) venen de ")
                              : juce::String ("From "))
                      << (char) ('A' + wn) << ".\n";
                s << (trF ? trF ("The tuning is never touched.", "L'afinaci\xc3\xb3 no es toca mai.")
                          : juce::String());
                if (proc->isPlaygroundSuspendingRealtime())
                    s << (trF ? trF ("  REALTIME is paused while the playground drives the spectrum; it comes "
                                     "back when a corner is cleared.",
                                     "  TEMPS REAL est\xc3\xa0 en pausa mentre el laboratori porta l'espectre; torna "
                                     "quan buidis una cantonada.")
                              : juce::String());
                // Corners saved with the follower armed keep following INSIDE the blend -
                // the one thing that would otherwise look broken ("my adaptive preset
                // stopped adapting") or invisible ("why is the pad moving by itself?").
                if (proc->getPlaygroundAdaptiveCount() > 0)
                    s << (trF ? trF ("  Corners saved with REALTIME keep adapting to what you play, "
                                     "inside the blend.",
                                     "  Les cantonades desades amb TEMPS REAL segueixen adaptant-se al que "
                                     "toques, dins la barreja.")
                              : juce::String());
                if (proc->getPlaygroundAdaptRefusal().isNotEmpty())
                    s << "\n" << (trErrF ? trErrF (proc->getPlaygroundAdaptRefusal())
                                         : proc->getPlaygroundAdaptRefusal());
                // EDIT MODE is the loudest thing this line can say: the main window's
                // controls are somebody else's patch right now, and a user who does not
                // know that will "fix" a corner thinking it is their own sound.
                if (const int ed = proc->getPlaygroundEditCorner(); ed >= 0)
                {
                    s << "\n";
                    s << (trF ? trF ("EDITING ", "EDITANT ") : juce::String ("EDITING "))
                      << (char) ('A' + ed) << ": "
                      << proc->getPlaygroundCornerDisplayName (ed) << ". ";
                    s << (trF ? trF ("The main window's controls are THIS CORNER now - your own patch is "
                                     "kept and comes back with OK. Save stores the corner under a new name; "
                                     "unsaved edits stay on the corner (the *) until you replace it.",
                                     "Els controls de la finestra principal s\xc3\xb3n ARA AQUESTA CANTONADA - "
                                     "el teu patch es guarda i torna amb OK. Desar guarda la cantonada amb un "
                                     "nom nou; els canvis no desats es queden a la cantonada (l'*) fins que "
                                     "la substitueixis.")
                              : juce::String());
                }
            }
            // ...AND THE ASSIGNMENT THAT DID NOT HAPPEN, last, where the eye ends up.
            // Built here rather than stored as a sentence so it is in the language the
            // panel is in NOW (see noteAssign), and it names the corner: with four boxes
            // in a row, "it did not work" is not an answer to "which one".
            if (assignFailedCorner >= 0)
                s << "\n" << (trF ? trF ("Corner ", "Cantonada ") : juce::String ("Corner "))
                  << (char) ('A' + assignFailedCorner) << ": " << assignFailedWhy;
            statLbl.setText (s, juce::dontSendNotification);
            pad.repaint();
        }

        // TESTS: drop a name on a corner THROUGH the box's own onChange, which is the only
        // path a user has and the only one that can swallow anything. A name that is not
        // in the folder is exactly the case life produces - the list is a snapshot of the
        // preset folder and the file can be gone by the time it is picked.
        void assignForTests (int corner, const juce::String& name)
        {
            cornerBox[corner].clear (juce::dontSendNotification);
            cornerBox[corner].addItem (name, 2);
            cornerBox[corner].setSelectedId (2, juce::sendNotificationSync);
        }

        int          assignFailedCorner = -1;   // -1 = the last assignment worked
        juce::String assignFailedWhy;           // the PROCESSOR's own words, verbatim
    };

    // Its own desktop window, like the partial-amplitude editor: the pad wants to be as
    // square and as large as the user cares to make it, which a panel clipped inside the
    // plugin's fixed 1200x650 could not be.
    struct PlaygroundWindow : juce::DocumentWindow
    {
        std::function<void()> onHide;
        std::function<bool (const juce::KeyPress&)> onKey;
        std::function<bool()>                       onKeyState;
        explicit PlaygroundWindow (const juce::String& name)
            : juce::DocumentWindow (name, juce::Colour (0xff0e1118), juce::DocumentWindow::closeButton)
        {
            setUsingNativeTitleBar (true);
            setResizable (true, false);
            setResizeLimits (420, 380, 3000, 3000);
            setWantsKeyboardFocus (true);
        }
        void closeButtonPressed() override { if (onHide) onHide(); }
        // The computer keyboard has to go on playing while this window is in front -
        // the whole point of a morph pad is hearing it move (same forwarding, and the
        // same "one key map, not a second copy", as PartialAmpWindow).
        bool keyPressed (const juce::KeyPress& k) override
        { return (onKey != nullptr && onKey (k)) || juce::DocumentWindow::keyPressed (k); }
        bool keyStateChanged (bool isDown) override
        { return (onKeyState != nullptr && onKeyState()) || juce::DocumentWindow::keyStateChanged (isDown); }
    };

    // A REAL window, not an in-editor overlay: with up to 16384 knobs the useful size
    // is "as big as the screen", which a panel clipped to the plugin's own 1200x650
    // could never be. Resizable, and it keeps its size for the session.
    struct PartialAmpWindow : juce::DocumentWindow
    {
        std::function<void()> onHide;
        // The computer keyboard has to go on playing while this window is in front.
        // It is a separate DESKTOP window, so opening it moves the keyboard focus out
        // of the editor and the QWERTY-as-MIDI mapping simply stopped: you could not
        // hear the partial you were dragging. These two forward the key events to the
        // editor's OWN handlers - one mapping, not a second copy that could drift. In
        // the STANDALONE that is the letter mapping and Ctrl+Z; in a PLUGIN the editor
        // uses only Ctrl+Z/Y and the arrows, and every other key goes on to the host
        // (see the editor's keyPressed: Live's own computer keyboard is how a plugin plays).
        std::function<bool (const juce::KeyPress&)> onKey;
        std::function<bool()>                       onKeyState;
        explicit PartialAmpWindow (const juce::String& name)
            : juce::DocumentWindow (name, juce::Colour (0xff0e1118), juce::DocumentWindow::closeButton)
        {
            setUsingNativeTitleBar (true);
            setResizable (true, false);
            setResizeLimits (320, 240, 6000, 4000);
            // With nothing inside it wanting focus, key events reach this window itself
            // (ComponentPeer::getTargetForKeyPress falls back to the peer's component).
            setWantsKeyboardFocus (true);
        }
        void closeButtonPressed() override { if (onHide) onHide(); }
        bool keyPressed (const juce::KeyPress& k) override
        {
            return (onKey != nullptr && onKey (k)) || juce::DocumentWindow::keyPressed (k);
        }
        bool keyStateChanged (bool isDown) override
        {
            return (onKeyState != nullptr && onKeyState()) || juce::DocumentWindow::keyStateChanged (isDown);
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

    // THE SPECTRUM-vs-TUNING COLUMN'S SECTION BANDS.
    //
    // "esta mal delimitado": the column was one unbroken ladder of rows. A mode selector,
    // then WHAT is being compared, then HOW the search is set up, then the buttons that
    // run it - eleven controls in the widest mode with nothing on screen saying where one
    // group ended and the next began. RhythmPanel answers exactly this question with one
    // rounded frame per group; this is the same claim ("these belong together") made in
    // the one shape this column can afford.
    //
    // WHY ONE RULED GROUND, AND NOT RhythmPanel'S SEPARATE BOXES. Measured, not chosen:
    // RhythmVerify holds the adapted-partial list to three unscrolled rows (54 px) and in
    // the goal-Range shape it had 56 - two pixels of slack in the whole column.
    // A RhythmPanel-style box costs 13 px of header + 3 + 8 of padding = 24 px over its
    // bare content, and three of those is 72 px this column does not have; even stripped
    // of its header, a box needs padding inside its own outline AND air outside it, so two
    // stacked boxes need ~9 px between their last and first rows where 4 px sits today.
    // ONE tinted ground under the whole stack, ruled at each internal boundary, needs
    // padding only at the very top and bottom, and its rules live inside the 4 px gap
    // that already separates two sections - so the delimitation costs the list NOTHING.
    // (It in fact gains: see the gap normalisation in resized(). The list went 56 -> 60
    // in the tightest shape and 81 -> 84 in the ordinary one.)
    //
    // WHAT IS NOT HERE, and why: coloured section HEADERS, the other half of RhythmPanel's
    // idiom. A juce::Label may not be laid out under 12 px (the snapshot rig fails any
    // visible control smaller than its own chrome), so three named sections cost at least
    // 3 x 12 = 36 px minus the 12 px of gaps they would replace. The normalisation below
    // frees 4. They are affordable in this column only if something else leaves it, and
    // nothing in it is spare - so the sections are drawn, and named nowhere, rather than
    // named at the price of the results the panel exists to show.
    //
    // Mouse-transparent (the WarningOverlay pattern) and constructed immediately after
    // gSpecScale so it sits ABOVE the group's opaque interior fill - drawGroupComponentOutline
    // FILLS its rect, so a frame painted in the editor's own paint() would be buried - and
    // BELOW every control, which are all added after it.
    struct SpecSections : juce::Component
    {
        // Section boundaries in this component's own coordinates: the y of each rule.
        // Empty = one section, no rules.
        std::vector<int> rules;
        juce::Colour     col { 0xff808080 };
        SpecSections() { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            if (r.getWidth() < 4.0f || r.getHeight() < 4.0f) return;
            // RhythmPanel's wash, RhythmPanel's line weight - and NO VERTICAL SIDES.
            //
            // The first version drew the whole rounded outline, RhythmPanel's frame
            // exactly. Photographed at 5x it is the doubled border this build refuses
            // everywhere else: gSpecScale's own 2.6 px stroke runs down the column and
            // this one landed 2 px inside it, a thin dark sliver between two parallel
            // orange lines. There is no width to fix that with - the whole distance from
            // the group's inner border to the content is under 7 px - so the sides are
            // simply not drawn. The COLUMN's border is this stack's left and right edge,
            // which is true: the stack is as wide as the column's content.
            //
            // What is left says the same thing without repeating a line that is already
            // on screen: a tinted ground under the whole stack, ruled off at both ends
            // and once at every section boundary.
            juce::Path body;
            body.addRoundedRectangle (r, 7.0f);
            g.setColour (col.withAlpha (0.15f));
            g.fillPath (body);
            g.setColour (col.withAlpha (0.34f));
            // The two ends stop 7 px short at each side - where the rounded corners are,
            // so a horizontal line never runs out past the ground it belongs to.
            g.fillRect (juce::Rectangle<float> (7.0f, 0.0f, r.getWidth() - 14.0f, 1.0f));
            g.fillRect (juce::Rectangle<float> (7.0f, r.getHeight() - 1.0f, r.getWidth() - 14.0f, 1.0f));
            for (const int y : rules)
                g.fillRect (juce::Rectangle<float> (3.0f, (float) y - 0.5f,
                                                    r.getWidth() - 6.0f, 1.0f));
        }
    };

    FractalAdditiveAudioProcessor& processorRef;

    std::unique_ptr<juce::LookAndFeel_V4> lnf;
    // 600 ms, which is 350 plus the 250 the user asked for: "que tarden 0.25 s MAS en
    // aparecer". At 350 a card fired while the pointer was still travelling towards the
    // control it was aimed at, so crossing the panel flashed hovers nobody had asked to
    // read. Still under JUCE's own 700 default - quicker than stock, no longer twitchy.
    // The two floating windows (PartialAmpPanel, PlaygroundPanel) carry the SAME number:
    // a hover that behaves differently in a sub-window is a bug nobody can name.
    juce::TooltipWindow tooltipWindow { this, 600 };
    ScaleKeyboard keyboard;

    SteppedSlider baseNFader, timbreRepeatsFader;
    MagneticSlider fractalFader, harmonicsFader, primeLimitFader; // magnetic whole-value detents
    juce::Slider  slopeFader, gainFader, cutoffFader, resFader, fEnvAmtFader, pEnvAmtFader;
    juce::Slider  ampAdsr[4], filtAdsr[4], pitchAdsr[4];
    juce::ComboBox tuneSourceBox, tuneScopeBox; // beating mode: source + scope
    // PHASE is a fader now, not a two-way box: the user asked for a progressive control
    // between aligned and scattered. The "how does a move reach a note that is already
    // sounding" choice that used to sit beside it is GONE - that answer comes from the
    // SPECTRUM panel's Morph / Dry-Wet transition, so there is nothing to keep in sync.
    juce::Slider   phaseFader;
    juce::ComboBox basePitchBox, primeModeBox, tuningBox, bendBox,
                   dissCountBox, dissModeBox, tuningBankBox;
    RefreshingComboBox presetBox;
    juce::Label presetDirtyMark; // "*" - some parameter drifted from the loaded preset
    void refreshPresetDirtyMark();
    // The corner the main window's controls were standing in for last time the UI was
    // refreshed, and the live patch's own preset name parked while that is true. The
    // preset field is FROZEN during a corner edit (it is not the loaded preset any more),
    // and this is what puts the real name back afterwards.
    int          lastEditCornerSeen = -1;
    bool         cornerEditUiActive = false;   // the TRANSITION guard, not a repeat of the above
    juce::String presetTextBeforeCornerEdit;
    juce::Rectangle<int> titleClickArea; // the title squares; clicking them plays the jingle
    juce::ToggleButton primeToggle[9];
    // The PRIME CONTROL master switch. OFF by default and OFF means no prime filter of
    // any kind - see prepareSettings, which writes the neutral settings rather than
    // teaching the generator about a switch.
    juce::ToggleButton primeOnToggle;
    // THE TRANSITION MODE - Off / Morph / Dry/Wet - as ONE dropdown where Morph and
    // Dry/Wet used to be two cells with two ON/OFF pills. It owns NO parameter: it is a
    // view of the "morph" and "dryWet" booleans, which are unchanged (see the constructor
    // for the four-states-three-sounds mapping and why it is exact). Kept in sync by
    // applyTransitionMode off the same 30 Hz timer that used to grey the Dry/Wet cell.
    juce::ComboBox transitionBox;
    juce::ToggleButton temperToggle; // lives INSIDE the Custom Engine box (the scale it bypasses)
    std::unique_ptr<ButtonAttachment> temperAtt;
    // SPECTRUM panel: the two ENGINES as chips - the TITLE ROW of the one shared
    // engine box, built exactly like the modulator tab strip. Exactly one lit, the
    // selected one's colour worn by the whole panel, and the selected one's controls
    // the only ones inside the box (resized() gates visibility from engineInForce).
    // They READ engineInForce() and WRITE the parameters that decide it, so chip
    // selection and which engine SOUNDS are one thing - and switching away never
    // discards the loaded scale (only "timbreOn" goes down).
    // TEMPERED is NOT one of them: it is a mode of the harmonic engine, a tick box
    // laid out in BOTH views (see temperToggle), not a third engine.
    juce::TextButton engineChip[2];
    // Show the CUSTOM view while nothing sounds custom yet. The engines share ONE box,
    // so with no scale loaded the Load button and the intervals editor only exist
    // inside the custom view - if the CUSTOM chip refused the click until a scale
    // existed (as it used to), the only ways to CREATE one would be unreachable.
    // Cleared the moment any non-harmonic engine state is observed, and by the
    // HARMONIC chip: it may only ever hold the box open as a workspace, never
    // contradict a real engine change.
    bool customViewPinned = false;
    // ONE predicate for "which view is inside the shared engine box", used by
    // resized() (visibility + layout), paint() (dividers/sub-labels) and
    // applyEngineState() (chip lighting + panel colour) - three readers that must
    // never disagree about what is on screen.
    bool customViewShown() const
    {
        using E = FractalAdditiveAudioProcessor::EngineInForce;
        const auto st = processorRef.engineInForce();
        return st == E::CustomScale || (st == E::Harmonic && customViewPinned);
    }
    // What applyEngineState() dressed the panel in. Seeded with the harmonic engine's
    // blue - the state a fresh instance opens in - so the very first paint (which can
    // land before the first applyEngineState) is never a default black frame.
    juce::Colour         spectrumPanelColour { 0xff5cb8ff };
    void applyEngineState(); // dim/enable + chip look + repaint, from engineInForce()
    // SELECTING A VIEW IS SELECTING AN ENGINE. One function, used by both chips and by
    // every other route that changes which view is on screen (Remove, Random), so the
    // two can never be wired differently: showing the custom view puts the custom engine
    // in force and showing the harmonic view takes it out. Writes the PARAMETERS
    // engineInForce() reads (never a member), so the choice is automatable, undoable,
    // and identical whether it came from a click or from the host.
    void selectEngineView (bool custom);
    // The custom view's Load/Remove button wears whichever of its two jobs is live right
    // now - word, colour and tooltip together. In ONE place because a button that says
    // "Load" in green and DELETES the user's spectrum is the worst failure this panel
    // could have, and three call sites setting two of the three properties is how that
    // happens. Called by applyLanguage() and by the timer whenever the engine changes.
    void refreshCustomEngineButtons();
    // The "Load spectrum's tuning" button's word, enabled state and tooltip, in ONE
    // place for the same reason as refreshCustomEngineButtons: called by applyLanguage()
    // and the timer, it must never say "load" while the click would refuse (already
    // active), so the word and the enabled state have to move together.
    void refreshSpectrumTuningButton();
    // The Shepard beats box's honesty dress: while Sync is chosen but the host has no
    // BPM, the FREE ms value is what actually runs - the box turns amber and its
    // tooltip says so. Colour and tooltip move together, in ONE place, called by the
    // timer and by applyLanguage() (which resets lastShepNoBpm so the words re-render).
    void refreshShepardSyncState();
    // ...and the SHEPARD switch's own hover, which is the only place the rise-interval
    // choice can be explained when the choice is not on screen. The rise box appears
    // only when the tuning's period differs from the octave; a tuning that states NO
    // ascending period (a descending .scl, a degenerate 1/1) has nothing to offer, and
    // the box used to just not be there - the rise quietly ran at 2/1 with no way to
    // find out why. Owns the whole tooltip, exactly like refreshShepardSyncState owns
    // the beats box's, and is called by the timer and by applyLanguage() (which resets
    // lastShepRiseState so the words re-render).
    void refreshShepardRiseState();
    // IS THE RISE INTERVAL A CHOICE AT ALL? ONE definition, because three places ask -
    // resized() (which puts the box on screen), the timer's relayout memo, and the
    // hover above - and two of them disagreeing means a box that is laid out and never
    // shown, or shown and never laid out. Both halves are needed: a tuning must STATE a
    // period before "is it the octave" is even a question, and asking periodRatio()
    // alone answered "it is the octave" for a scale that states nothing.
    bool shepardRiseChoiceExists() const
    {
        return processorRef.tuningStatesPeriod()
            && shepard::periodDiffersFromOctave (processorRef.getStatedTuningPeriodRatio());
    }
    // Repaint every control inside the SPECTRUM panel in the engine's colour. The panel
    // is ONE box now, so a fader still wearing its old per-cell hue would be the only
    // thing on screen still claiming the compartments are separate boxes.
    void recolourSpectrumControls (juce::Colour col);
    // The SLOPE (AMP DECAY) fader's colour, decided in ONE place - it is the only control
    // in the panel with a second, INERT state, and having two writers is what left it
    // mint green after a pencil on/off cycle. See the definition.
    void applySlopeFaderColour();
    std::unique_ptr<SliderAttachment> primeLimitFaderAtt;
    // SLOPE cell: the pencil that opens the per-partial amplitude window, and the ON/OFF
    // pill under it. The pill and the one INSIDE the window are two attachments to the
    // same "partialManual" parameter - that is what makes them one control in two
    // places, and it is why switching either one is automatable and undoable.
    PencilButton                      partialEditBtn;
    juce::ToggleButton                partialManualT;
    std::unique_ptr<ButtonAttachment> partialManualAtt;
    PartialAmpPanel                   partialPanel;
    // Declared AFTER the panel it shows, so it is torn down FIRST: the window holds the
    // panel as non-owned content and must not outlive it.
    std::unique_ptr<PartialAmpWindow> partialWindow;
    // The preset playground: the button that lives with the preset selector, its panel,
    // and its window. Same ordering rule - the window must be destroyed before the panel
    // it points at.
    juce::TextButton                 playgroundButton;
    PlaygroundPanel                  playgroundPanel;
    std::unique_ptr<PlaygroundWindow> playgroundWindow;
    // -1 = not applied yet, so the first timer tick (and every language switch, which
    // resets it) writes the slope fader's frozen/normal look and its bilingual tooltip.
    int   lastManualFrozenShown = -1;
    // The TRANSITION cell's Time fader, as TWO sliders sharing one rectangle with exactly
    // one visible: the panel asks for one fader, but "morphTime" and "dryWetTime" are two
    // parameters and each mode must keep the time its user set for it (and its automation
    // lane). Swapping one slider's attachment at runtime would do the same job by throwing
    // an Attachment away and building another every time the mode changes; two permanent
    // attachments and a visibility flip cannot get that wrong.
    juce::Slider       morphTimeFader, dryWetTimeFader;
    juce::TextButton   menuButton; // top-bar menu: Init (reset all) / Panic - works in the VST too
    int                undoTxnTick = 0; // paces the tuning-bank combo refresh
    juce::String       lastSeenTimbreSource; // re-seeds intervalEdit when the timbre changes elsewhere
    // The envelopes share ONE compact box; these chips switch which one shows.
    juce::TextButton   envTabB[4];
    int                envTab = 0; // 0 = Amp, 1 = Pitch, 2 = Filter, 3 = Custom
    // CUSTOM tab = 6 modulators (3 envelopes + 3 LFOs), selected by modSelBox; the
    // faders/combos below re-attach to the selected modulator's parameters.
    juce::ComboBox     modSelBox;                   // Env 1..3 / LFO 1..3
    juce::Slider       custAdsr[4], custRangeFader; // env: A D S R + Range (LFO reuses Range)
    juce::Label        custAdsrLbl[4], lblCustRange;
    juce::ComboBox     lfoWaveBox, lfoRateModeBox, lfoPhaseModeBox;
    juce::Slider       lfoDivSlider, lfoHzSlider, lfoPhaseSlider;
    juce::Label        lblLfoDiv, lblLfoHz, lblLfoPhase;
    juce::ToggleButton modOnT;                      // per-modulator on/off
    juce::TextButton   modUnmapBtn;                 // unmap this modulator from everything
    std::unique_ptr<SliderAttachment>   custAdsrAtt[4], custRangeAtt, lfoDivAtt, lfoHzAtt, lfoPhaseAtt;
    std::unique_ptr<ComboBoxAttachment> lfoWaveAtt, lfoRateModeAtt, lfoPhaseModeAtt;
    std::unique_ptr<ButtonAttachment>   modOnAtt;
    int  modSel = 0;                                // 0..2 env, 3..5 lfo
    WaveEditor waveEditor;                          // drawn-LFO wave overlay
    EnvEditor  envEditor;                           // the selected MODULATOR envelope, as a curve
    EnvEditor  voiceEnv;                            // the Amp / Pitch / Filter envelope, same editor
    int        voiceEnvTab = 0;                     // which one voiceEnv is showing
    int        lastDrawLfo = -1;                    // which LFO the docked wave editor is on
    void attachCustomTab();                         // rebind attachments to the selected modulator
    void showEnvMapMenu (const juce::String& paramID);
    static const char* modName (int mod);            // "Env 1".."LFO 3" - same in both languages
public:
    // TESTS: WHAT ONE ITEM OF THE RIGHT-CLICK MAP MENU SAYS.
    //
    // The menu is a juce::PopupMenu, drawn in a window of its own by the OS's popup
    // machinery, so it is outside every sweep this editor can run over its own children:
    // the fit sweep, the untranslated-text gate and the mojibake gate all walk Components,
    // and a menu item is not one. That is how " (8/8 full)" - an English literal and a
    // hand-typed copy of ModEngine::kMaxTargets - sat in a translated menu unseen. The
    // menu and this probe call the SAME builder, so what a test reads is what a user gets.
    juce::String modMapItemTextForTests (int mod) const { return modMapItemText (mod); }
private:
    juce::String modMapItemText (int mod) const;
    juce::String currentModParamId (const juce::String& templateId) const; // modE1A -> modE2A...
    void addMapListener (juce::Component& c, const juce::String& paramID, bool dynamicMod = false);
    struct ParamMapListener : juce::MouseListener
    {
        FractalAdditiveAudioProcessorEditor* ed = nullptr;
        juce::String pid;
        // CUSTOM-tab controls are re-bound to whichever modulator is selected, so
        // their parameter id changes at runtime: those listeners resolve it live.
        bool dynamicMod = false;
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isRightButtonDown() && ed != nullptr)
                ed->showEnvMapMenu (dynamicMod ? ed->currentModParamId (pid) : pid);
        }
    };
    std::vector<std::unique_ptr<ParamMapListener>> mapListeners;
    juce::TextButton   rhythmBtn;  // top-bar: opens the spectrum->rhythm MIDI panel
    juce::TextButton   octDn, octUp, sprDn, sprUp; // OCTAVE cell: -1/+1 steppers
    juce::Label        octVal, sprVal;             // their value read-outs
    juce::TextButton saveTuningBtn;    // save the CURRENT tuning (any mode) as .scl to a chosen folder
    juce::TextButton tuningBankAddBtn;  // put the CURRENT tuning into the bank
    juce::TextButton recentOpenBtn;     // opens the floating Recent list
    juce::TextButton loadItsSpectrumBtn; // .scl written by this synth: restore its patch
    juce::TextButton loadPresetTuningBtn; // the loaded PRESET carries a different tuning
    // The current CUSTOM SPECTRUM was derived from a tuning (adapted to it, or loaded
    // from a scale file): this loads that tuning. Shown only while the provenance
    // exists; DISABLED (word changes to say so) when that tuning is already active,
    // because reloading what is active would only push a do-nothing undo step.
    juce::TextButton loadSpectrumTuningBtn;
    juce::String     lastSpectrumTuningSig; // skip the per-tick refresh when nothing moved
    // loadTimbreButton is ONE button with two jobs: "Load" while no custom spectrum
    // exists, "Remove" once one does. The Custom-Engine On/Off pill that used to sit
    // beside it is gone - with the engine chips deciding which engine is in force, an
    // Off pill was a second, quieter switch for the same thing, and the pair could
    // disagree on screen. Removing is the destructive half the pill never offered.
    juce::TextButton loadSclButton, loadTimbreButton,
                     langButton, savePresetButton, dissBtn, dissSaveBtn, recentBtn[3], tuningBankLoadBtn,
                     tuningBankClearBtn;
    // RANDOM SPECTRUM: fires the automatable "specRandom" trigger (see the processor).
    // The button does not build anything itself - it writes the parameter, and the
    // processor's own poll builds - so a click and a host automation step take exactly
    // the same path and cannot drift apart.
    juce::TextButton randomSpectrumBtn;
    juce::Label          vibeLabel;   // "Vibe coded by" credit, left of the language button
    juce::HyperlinkButton vibeLink;   // "Asra3" -> Instagram
    juce::TextEditor periodEdit;
    // Absolute-table tunings only: the period is METADATA there (the 128 keys already fix
    // every pitch), so it is Fibló's estimate until the user types over it. Visible only
    // in table mode - a scale's period comes from the scale and must not look editable.
    juce::TextEditor tablePeriodEdit;
    juce::TextButton tablePeriodResetBtn;
    ExpandingEditor  intervalEdit; // Custom Engine: typed intervals -> partials (multi-line)
    juce::Rectangle<int> intervalEditHome; // its laid-out bounds (kept for the focus hook)
    juce::TextEditor refAEdit;     // Reference A: type the Hz directly ("Hz" shown to its right)
    WarningOverlay warningOverlay;
    // Declared with the other overlays; CONSTRUCTED (added to the editor) with gSpecScale,
    // which is what puts it in the right z-order - see the struct's own comment.
    SpecSections   specSections;
    NoticeOverlay  noticeOverlay;
    // HOW A NOTICE IS PUT ON SCREEN, AND WHY IT IS A LAMBDA AND NOT TWO STRINGS.
    //
    // NoticeOverlay::title and ::body are plain juce::Strings. Five places wrote their
    // tr() pairs straight into them and nothing ever wrote them again - so the language
    // button, which re-runs applyLanguage() over every label, button, tooltip and combo
    // in the window, walked straight past the one panel in the middle of the screen that
    // was TELLING THE USER SOMETHING HAD GONE WRONG. A project whose scale would not load
    // said so in English, the user switched the window to Catalan to read it, and the
    // notice was the single thing that did not follow.
    //
    // The fix cannot be "translate it again", because a notice carries data as well as
    // words - a corner letter, a count, a parser's own message - and only the site that
    // raised it knows those. So the site hands over the RECIPE instead: a callable that
    // writes title and body through tr(). Raising it runs it once; applyLanguage runs it
    // again while the panel is up, and every notice in this window follows the language
    // for the same reason and by the same line.
    std::function<void()> noticeRender;
    // THE LOAD WATCH'S WARNING (see dsp/LoadMonitor.h and refreshLoadWarning). One EPISODE
    // is the time from the load going hot to it cooling again: the notice goes up once per
    // episode, is refreshed in place while it lasts, and a notice the user closed with X
    // stays closed until the next one.
    fibload::Policy loadPolicy;
    bool loadEpisodeRaised = false;
    double loadRefreshSeconds = 0.0;   // since the shown numbers were last rewritten (twice a second)
    double lastLoadTickMs = 0.0;       // wall clock of the previous timer tick, for the Policy's calm
    void refreshLoadWarning (double dtSeconds);
    void renderLoadNotice (const fibload::Policy& shown);
    juce::String loadNoticeBody (const fibload::Policy&) const;
    bool loadNoticeUp() const;
    void clearNoticeAction()
    { noticeOverlay.actionText.clear(); noticeOverlay.onAction = nullptr; noticeOverlay.actionBtn.setTooltip ({}); }
    void toggleWarnNearFreeze();
    juce::PopupMenu buildMainMenu();
    void handleMainMenuChoice (int r);
    // THE FOCUS FOLLOWS THE POINTER ONLY IN THE STANDALONE. In a host the window is one of
    // many, and taking the keyboard just because the pointer crossed it took it away from the
    // host (Live's own computer keyboard among it) - see keyPressed.
    bool focusFollowsPointer() const
    { return processorRef.wrapperType == juce::AudioProcessor::wrapperType_Standalone; }
    // The one place the window asks for the keyboard as it opens (the Standalone only - see
    // the constructor). A function of its own so a check can see whether it was asked.
    void queueOpenFocusGrab();
    bool openFocusGrabQueued = false;
    // A GESTURE IN THIS WINDOW: the last mouse press or release on the editor or one of its
    // own floating windows, seen through a global listener (a Button's onClick runs on the
    // release, a Slider's onDragEnd too). See refocusAllowed.
    struct GestureWatch : juce::MouseListener
    {
        explicit GestureWatch (FractalAdditiveAudioProcessorEditor& e) : ed (e) {}
        void mouseDown (const juce::MouseEvent& e) override { note (e); }
        void mouseUp   (const juce::MouseEvent& e) override { note (e); }
        void note (const juce::MouseEvent& e);
        FractalAdditiveAudioProcessorEditor& ed;
    };
    GestureWatch gestureWatch { *this };
    juce::uint32 lastGestureMs = 0;
    static constexpr juce::uint32 kGestureMs = 1500;
    bool isOurComponent (const juce::Component* c) const;
    bool refocusAllowed() const;
    std::unique_ptr<juce::ComponentTraverser> createKeyboardFocusTraverser() override;
    int  loadShownKinds = 0;          // which kinds of trigger the shown warning spoke of
    bool loadWatchHeld = false;       // the snapshot rig only (holdLoadWatchForSnapshot)
    void raiseLoadNotice();
    // NOTICES QUEUE; THEY NEVER OVERWRITE. raiseNotice used to replace whatever was on
    // screen, so two failures parked in the same tick - a project that lost its scale AND
    // a corner - showed only the second, and the first was never seen by anyone. A notice
    // raised while one is up now waits behind it (the panel says how many), and the X shows
    // the next. A KEY is for the notices that restate a live condition (a search coming
    // back short, realtime refused): raising the same key again refreshes that notice in
    // place instead of stacking copies. Processor failures pass no key - every one queues.
    juce::String noticeKey;
    struct QueuedNotice { juce::String key; std::function<void()> render; };
    std::vector<QueuedNotice> noticeQueue;
    void raiseNotice (std::function<void()> render, const juce::String& key = {});
    void showNextNotice();
    // Takes back ONE kind of notice - on screen or queued - and nothing else. The degree
    // search used to hide the overlay outright when it succeeded, whatever it was saying.
    void withdrawNotice (const juce::String& key);
    void layoutNotice (bool keepTop = false);
    // The processor's realtime refusals in the window's language (see the definition).
    juce::String realtimeRefusalText (const juce::String& raw) const;
    RecentOverlay  recentOverlay;   // floating list of every scale file loaded
    DissDegreeList degreeList;   // active-scale dissonance list (lives inside dissView)
    DissHeader     dissHeader;   // Step / Sethares-dissonance column header
    juce::Viewport dissView;     // scrolls degreeList when a scale has many steps
    // Spectrum-to-Tuning mode + the "from spectrum's segment" partial picker.
    juce::ComboBox   specModeBox;                 // dyads Sethares / segment / chords Sethares
    // Chord-search mode (n-note Sethares consonance ranking).
    juce::ComboBox   chordNotesBox, chordCountBox, chordMinMaxBox;
    juce::Label      chordNotesLbl, chordPerLbl, chordProgLbl;
    juce::TextEditor chordPeriodEdit;
    // Minimum interval between generated notes (cents, default 33) - both creators.
    juce::TextEditor dissMinEdit, chordMinEdit;
    juce::Label      dissMinLbl, chordMinLbl, chordCountLbl;
    // Loaded-chord degree view: back (all chords) / forward (this chord's ranked degrees).
    juce::TextButton chordViewBack, chordViewFwd;
    bool             chordDegView = false;
    // n-AD selector over the degree list (Degrees / Dyads / Triads / ... up to the
    // scale's own note count, live) + its ranked-combination list.
    juce::ComboBox   nadBox;
    NadList          nadList;
    juce::Viewport   nadView;
    int              lastNadGen = 0, lastNadSel = 1, lastNadD = 0, lastNotesMax = 0;
    bool             lastNadRootOnly = true;
    juce::String     lastNadOpts;   // Min/Max + period + smallest interval, as typed
    double           lastNadSig = -1.0e30;
    juce::uint32     lastNadSpecMs = 0;      // rate limit for SPECTRUM-driven recomputes (see timer)
    double           lastNadSpecSig = -1.0;  // last ACCEPTED spectrum signature (held between refreshes)
    // ONE REMEMBERED PERIOD PER MODE. The search wants 2/1 and the adaptation wants 8/1 -
    // eight partials spread over three octaves is the spectrum that mode exists to build,
    // and 2/1 crammed them into one - so the field cannot have a single default. The rule:
    // the text belongs to the mode it was typed in. Leaving a mode parks whatever the field
    // says under that mode and the mode you arrive at shows its own value back; nothing a
    // user typed is ever thrown away, it just stays where it was typed.
    // [0] = "Find tunings based on beating", [1] = "Adapt spectrum to tuning based on beating".
    juce::String     periodTextByMode[2] { "2", "8" };
    int              lastSpecMode = 1;
    void             switchPeriodTextForMode (int newMode);
    juce::TextButton chordGoBtn;
    ChordList        chordList;
    juce::Viewport   chordView;
    int              lastChordGen = 0;
    //== "Adapt spectrum to tuning based on beating" - the INVERSE of the search above:
    //   the notes are given and the PARTIALS move (see Dissonance.h::adaptSpectrum).
    juce::ComboBox   adaptTargetBox;   // the loaded tuning / the chord held right now
    juce::ComboBox   adaptPartialsBox; // how many partials the adapted spectrum has
    juce::ComboBox   adaptGoalBox;     // minimum / maximum / inside a range
    juce::TextEditor adaptLoEdit, adaptHiEdit;   // the range's two ends (goal = range)
    juce::Label      adaptPartialsLbl, adaptRangeLbl, adaptInfoLbl;
    //== THE REGISTER BAND the adaptation optimises over: ONE number, N, counted in the
    //   TUNING'S OWN PERIODS and running BOTH WAYS from the note being played - the band is
    //   [anchor - N periods, anchor + N periods] (see
    //   FractalAdditiveAudioProcessor::registerAnchorKey for which note the anchor is).
    //
    //   It was two absolute bounds first ("C2".."C4", then periods 2..4), which were pinned
    //   to fixed pitches so the adaptation handed back the same spectrum whichever register
    //   was played; then a COUNT OF STEPS running upward, which followed the hands but was
    //   two things at once: unreadable as a width ("62" is two periods in 31-EDO) and
    //   one-sided, so the spectrum was never asked what it does BELOW the note. This form
    //   answers both: a period is the unit the tuning itself is built from, so the number
    //   needs no division to be read and is never re-derived on a scale change, and the
    //   band straddles the played note.
    //
    //   Sethares' critical band is a width in Hz, so the answer genuinely differs per
    //   register - and differs going DOWN as much as going up: the interval that beats
    //   worst is ~434 cents in the bass and ~61 cents four octaves up. Which registers a
    //   spectrum was optimised for therefore has to be on screen, not implied.
    //
    //   There is no aggregate selector beside it any more. `adaptAggBox` offered mean over
    //   the range vs minimax (the worst register); minimax stopped being a choice the day
    //   the range was anchored at the played note, and it has not come back with the
    //   symmetric band - the mean is what SpectrumTests (8) measures every width on. The
    //   aggregate is the mean, always; the tooltip on this field says so.
    juce::TextEditor adaptRegPeriodsEdit;
    // `adaptRegSpanLbl` is what stops N being a magic number: it names the ANCHOR the band
    // hangs off and the two keys it really reaches, so the width is readable without
    // arithmetic, the user can see WHICH note is being counted from, and a band shortened
    // by the end of the keyboard says so instead of looking symmetric.
    juce::Label      adaptRegLbl, adaptRegSpanLbl;
    // N, in periods. < 0 means "not typed yet": until then the field shows - and the search
    // uses - dissonance::kAdaptRegisterDefaultPeriods. Unlike the step count this replaces,
    // a typed N needs no protection from a tuning change: periods ARE the tuning's units,
    // so "1.5" means one and a half periods of whatever is loaded, before and after.
    double           adaptRegPeriods = -1.0;
    int              registerDegreesPerPeriod() const;
    double           registerPeriodsEffective() const;     // N as typed, or the default width
    // ...and how much of it the 128-key board really has on each side of the anchor. They
    // differ from N only near the ends of the keyboard, and when they do the panel SAYS the
    // shorter numbers: quietly sliding the anchor to make N fit would optimise for
    // registers the user never asked about, with nothing on screen to show it.
    double           registerPeriodsFittingDown() const;
    double           registerPeriodsFittingUp() const;
    double           registerPeriodsFrom (double endHz) const;   // Hz -> periods off the anchor
    juce::String     registerPeriodText (int key) const;   // key -> the period it starts
    juce::String     registerKeyText (int key) const;      // key -> "period:degree", e.g. "3:0"
    juce::String     adaptRegSpanText() const;             // the sentence under the N field
    juce::String     adaptRegSpanLine (const juce::String& anchor, double periods,
                                       double fitDown, double fitUp,
                                       const juce::String& loHz, const juce::String& hiHz,
                                       const juce::String& qualifier) const;
    // How the period the band is built on is qualified, if at all: empty for a scale that
    // states its own, a marker for a .tun table where it is Fibló's estimate or - worse -
    // the fallback nobody supplied. Appended to the line under the field, because a band
    // measured in a unit the tuning does not really have must not look like a fact.
    juce::String     registerPeriodQualifier() const;
    juce::String     registerPeriodQualifierText (bool estimated) const;
    juce::String     registerHzText (double hz) const;      // a band end, as the panel prints it
    void             syncAdaptRegisterSpan();
    void             commitAdaptRegisterPeriods();
    // The key whose ACTIVE-TUNING pitch is nearest `hz` - how a register in Hz is named
    // back to the user. Through the tuning, so a shifted or table-anchored instrument
    // names its own keys and not 12-EDO's.
    int              nearestKeyForHz (double hz) const;
    juce::ToggleButton adaptAmpsToggle;          // shape AMPLITUDES as well as positions
    juce::TextButton adaptGoBtn;
    AdaptList        adaptList;
    juce::Viewport   adaptView;
    int              lastAdaptGen = 0;
    void runSpectrumAdapt();
    //== THE ONE READ-OUT under the two buttons. There used to be two lines of numbers
    //   there and neither said what it was for ("avg C2-C4: 769.9 -> 27.8 (96% less)" over
    //   "sounding now: 31.4 (Reps/Fractal)"), the second one usually clipped. This has a
    //   single job - WHAT IS HAPPENING, OR WHAT WILL HAPPEN IF YOU PRESS - and answers it
    //   in one sentence, in priority order (see adaptReadoutText). Every number that was
    //   on screen is still available, in the read-out's tooltip.
    juce::String adaptReadoutText() const;
    juce::String adaptReadoutTip() const;
    // Its five states, each a function of its own numbers so nothing is written twice and
    // the layout probe can measure every one of them at its widest (see readoutVariants).
    juce::String adaptLineWorking   (int pct) const;
    juce::String adaptLineLag       (double ms) const;
    juce::String adaptLineFollowing (int updates) const;
    juce::String adaptLineCost      (double ms) const;
    juce::String adaptLineResult    (int pct, bool less, const juce::String& span, bool drift) const;
    // What ONE Adapt with the panel's current settings will cost, in ms, and the same
    // figure for one REALTIME update (which uses fewer registers). Both come from the
    // processor's estimator, so the price on the button is the number the guard uses.
    double adaptCostMs (bool realtime) const;
    // The result the last finished run reported, kept so the read-out can be rebuilt on
    // any tick (a warning may replace it and then have to give it back). `adaptInfoTipBase`
    // is the language's own explanation of what this line is for, set by applyLanguage and
    // always the first paragraph of the tooltip.
    juce::String adaptResultLine, adaptResultTip, adaptInfoTipBase;
    juce::uint32 lastAdaptTipMs = 0;   // the tooltip is rebuilt at 4 Hz, not at timer rate
    void refreshAdaptButtonText();
    //== REALTIME: one button per direction, and the one shared place that tells the
    //   processor what they should be doing (see syncRealtimeLink).
    juce::TextButton chordRtBtn, adaptRtBtn;
    bool             chordRtOn = false, adaptRtOn = false;
    // ADAPTIVE LOOKAHEAD: the follower's own latency trade, on the follower's own row.
    // Not a parameter (see RtSettings::lookahead) - it travels in the realtime settings
    // like the rest of the link, and the panel adopts the live value when it opens.
    juce::TextButton lookaheadBtn;
    bool             lookaheadOn = false;
    // The target the user chose by hand, parked while REALTIME forces the box to the held
    // chord (the only target a follower can meaningfully have) and handed straight back
    // when realtime goes off. 0 = nothing parked.
    int              adaptTargetBeforeRt = 0;
    FractalAdditiveAudioProcessor::RtSettings realtimeSettingsFromPanel() const;
    void syncRealtimeLink();
    void syncAdaptTargetLock();
    void paintRealtimeButtons();
    // Chord preview scheduling (messages sent into the processor's collector when due).
    struct PrevEv { juce::uint32 dueMs; juce::MidiMessage msg; };
    std::vector<PrevEv> previewQueue;
    std::vector<std::pair<int,int>> previewNotes; // (ch,key) of the current preview - for explicit stops
    void stopChordPreview();                      // explicit note-offs + bend recentre (MPE-safe)
    void playChordPreview (int row);
    void hoverPreview (const std::vector<double>& ratios); // sounds while hovering a row
    void runChordSearch();
    // Hover-dissonance cache: Sethares dissonance of every key AGAINST the hovered
    // one (recomputed only when the hovered note / timbre / tuning changes).
    void updateHoverDissCache (int hoveredNote);
    int    hoverDissNote = -1;
    double hoverDissSig  = -1.0e30;
    std::array<float, 128> hoverDissVal {};
    float  hoverDissLo = 0.0f, hoverDissHi = 1.0f;
    SegmentList      segList;                     // partial picker (inside segView)
    juce::Viewport   segView;
    juce::TextEditor segPeriodEdit;               // "2" / "3/2" / "500c"
    juce::ToggleButton segTopToggle;              // period = highest selected partial
    juce::Label      segPeriodLbl, segHelpLbl;
    // Cached active-scale dissonance (recomputed only when timbre/tuning change) +
    // the sort mode toggled by the header.
    std::vector<FractalAdditiveAudioProcessor::DissDegreeInfo> activeDiss;
    double lastScaleSig   = -1.0e30; // change signature (timbre gen + tuning shape)
    juce::uint32 lastScaleDissMs = 0; // rate limit: a modulated spectrum changes the
                                      // signature every tick, and this analysis is advisory
    Scrim          rhythmScrim;
    RhythmPanel    rhythmPanel;
    std::unique_ptr<ButtonAttachment>   rhythmOnAtt, rhythmSelfAtt, rhythmDroneAtt, rhythmFbAtt, rhythmMpeAtt, rhythmTempoAtt,
                                        rhythmQTempoAtt, rhythmMonoAtt, rhythmQPulseAtt;
    std::unique_ptr<SliderAttachment>   rhythmFbDecayAtt, rhythmVelSlopeAtt, rhythmStepsAllAtt;
    std::unique_ptr<ComboBoxAttachment> rhythmModeAtt;
    juce::uint32 lastRhythmGen = 0; // refresh the partial list when the template changes
    juce::uint32 lastSegGen = 0;    // refresh the segment picker on template change

    // Only the boxes that really are GroupComponents. The SPECTRUM panel and everything
    // nested in it (both engines, the shared cell), plus ESSENTIALS and TUNING, are
    // PAINTED from the rects below - which is what keeps their controls clickable: an
    // opaque GroupComponent added after its children buries them (they keep their
    // tooltips, so the bug looks like "the control is there but dead").
    juce::GroupComponent gFilter, gAmpEnv, gPitchEnv, gTuning, gSpecScale, gScaleDiss;
    juce::Label lblBaseN, lblBasePitch, lblPrimeLimit, lblCutoff, lblRes, lblEnvAmt,
                lblTimbreRepeats, lblBend, lblPeriod, lblSteps, lblRefA, lblRefHz, lblPitchAmt,
                lblTransTime, lblTuningBank, tuningStatusLabel, tuningNameLabel, timbreStatusLabel, recentLabel,
                tuningPeriodLbl;
    juce::Label ampAdsrLbl[4], filtAdsrLbl[4], pitchAdsrLbl[4], primeLbl[9];

    std::unique_ptr<SliderAttachment>   baseNAtt, slopeAtt, fractalAtt, gainAtt,
                                        cutoffAtt, resAtt, fEnvAmtAtt, timbreRepeatsAtt, pEnvAmtAtt,
                                        harmonicsAtt, phaseAtt;
    std::unique_ptr<SliderAttachment>   ampAdsrAtt[4], filtAdsrAtt[4], pitchAdsrAtt[4];
    std::unique_ptr<ComboBoxAttachment> basePitchAtt,
                                        primeModeAtt, tuningAtt, bendAtt, tuningBankAtt;
    std::unique_ptr<ButtonAttachment>   primeToggleAtt[9], primeOnAtt;
    std::unique_ptr<SliderAttachment>   morphTimeAtt, dryWetTimeAtt;

    //== MONOPHONIC GLIDE (the PITCH tab of the shared envelope box) ============
    // The toggle sits on the PITCH tab because that is where the user asked for it, and
    // because that is where it belongs: it is a pitch behaviour, not an envelope. The
    // time fader stands beside the pitch-envelope amount as a second "extra" column;
    // both are visible only while that tab is showing.
    juce::ToggleButton                monoGlideT;
    juce::Slider                      glideFader;
    juce::Label                       lblGlide;
    std::unique_ptr<ButtonAttachment> monoGlideAtt;
    std::unique_ptr<SliderAttachment> glideAtt;

    // SHEPARD MODE (bottom row of the Tuning box): the mode toggle, the ONE rate
    // control in its two dresses (the ms fader in Free mode, the beats box in Sync -
    // resized() shows exactly one of them), and the rise-interval choice, which is on
    // screen ONLY while the active tuning's period is not an octave (nothing to
    // choose otherwise). The v1 LEVEL fader is gone: the Shepard is the voice now,
    // so its level was a second master volume, and the only other thing a depth
    // control could do - stop the window reaching zero at its edges - would break the
    // silent wrap that makes the whole illusion click-free.
    juce::ToggleButton shepOnT;
    // WHICH WAY THE TONE GOES, as one arrow you press. A TextButton in toggle mode rather
    // than a two-item ComboBox: there are exactly two states, the glyph IS the answer in
    // both languages, and it costs the row 24 px against a dropdown's ~90. Its parameter
    // is shepDown, so an automation lane can sweep the direction like anything else.
    juce::TextButton   shepDirBtn;
    juce::ComboBox     shepModeBox, shepDivBox, shepRiseBox;
    juce::Slider       shepTimeFader;
    std::unique_ptr<ButtonAttachment>   shepOnAtt, shepDirAtt;
    std::unique_ptr<ComboBoxAttachment> shepModeAtt, shepDivAtt, shepRiseAtt;
    std::unique_ptr<SliderAttachment>   shepTimeAtt;
    // (Whether the rise-interval box is on screen rides in the timer's `packed`
    //  layout signature - see the tuning-period bit there.)
    // Last "Sync chosen but the host offers no BPM" state shown (-1 = not yet):
    // the timer redresses the beats box amber and says in its tooltip that the
    // free ms value is in force, instead of letting it claim a sync that is not
    // happening. See shepardSyncHasNoBpm().
    int lastShepNoBpm = -1;
    // The LFO rate cap's dressing, cached the same way and for the same reason: the timer
    // runs at 30 Hz and re-colouring plus re-writing a tooltip on every tick is work
    // nobody asked for. -1 means "not yet dressed", so the first tick always writes.
    // Reset by applyLanguage() so the sentence comes back in the new language.
    int lastLfoLimited = -1;
    void refreshLfoRateLimitState();
public:
    // TESTS: the two things refreshLfoRateLimitState writes, read back.
    //
    // "The engine knows it capped the rate" and "the user can find out" are different
    // claims, and only the second one is the promise this project makes. So the test asks
    // the CONTROL what colour it is wearing and what its hover says, rather than asking
    // the engine again what it already published.
    juce::Colour lfoRateColourForTests() const
    { return lfoHzSlider.findColour (juce::Slider::thumbColourId); }
    juce::String lfoRateTooltipForTests() { return lfoHzSlider.getTooltip(); }
    void selectModulatorForTests (int i) { modSel = juce::jlimit (0, 5, i); resized(); }
private:
    // 0 = period is the octave (nothing to choose), 1 = a real choice is on screen,
    // 2 = the tuning states no period at all. -1 forces the next refresh to write.
    int lastShepRiseState = -1;

    std::unique_ptr<juce::FileChooser>  fileChooser;
    juce::Component::SafePointer<juce::AlertWindow> activePrompt; // open save-preset dialog, if any

    juce::Label meterLabel, helpLabel;

    bool catalan = false;
public:
    // TESTS: trErr answers its input unchanged in English by design, so a test of the
    // TRANSLATION has to say which language it is asking in.
    void setLanguageForTests (bool ca) { catalan = ca; }
    juce::String trErrForTests (const juce::String& s) const { return trErr (s); }
private:
    // Guards for the read-out refresh applyLanguage() ends with (see its tail).
    // `editorConstructed` keeps the constructor's own applyLanguage() from running a
    // timer tick before setSize() and applyEngineState() have happened; `inLanguageRefresh`
    // makes the refresh non-reentrant, so nothing it touches can start a second one.
    bool editorConstructed  = false;
    bool inLanguageRefresh  = false;
    int  baseKey = 60; // MIDI note of the leftmost computer key; Z/X shift it by one period
    int  lastPrimeMode = -1;
    // Packs engineInForce() with "is a custom scale loaded at all" - the second bit
    // can change without the first one moving (loading a scale while the engine is
    // off), and the custom view's Off button hangs on it. -1 = never applied yet.
    int  lastEngineShown = -1;
    int  lastResetOffState = -1; // packs Reset/Off visibility to detect changes for relayout
    // The scale name the tuning row was last laid out FOR. Its width decides whether the
    // name fits beside the mode selector or needs a row of its own, and setText() does no
    // layout - so without this a scale loaded at run time keeps the previous name's cell.
    juce::String lastTuningNameLaidOut { "\x01" }; // never a real name: forces the first pass
    int  lastDissGeneration = 0; // refresh the degree list / shortfall notice once per generation
    int  lastKbLo = -1, lastKbNum = -1; // last keyboard window applied (re-adapt on tuning change)
    // SPECTRUM macro-panel: the outer frame, its three nested boxes and their sub-cells.
    // Everything here is a rect only - the frames, the title chips, the dividers, the
    // sub-labels and the override badges are all drawn in paint() from these.
    juce::Rectangle<int> spectrumBounds;       // the whole SPECTRUM panel
    // ONE shared engine box: the HARMONIC / CUSTOM chips are its title row, and only
    // the selected engine's controls are laid out inside it (the other's are hidden).
    // Two separate framed boxes said "two independent things" about two engines that
    // are mutually exclusive, and left the Custom Engine squeezed into a fifth of the
    // panel while most of its box was air.
    juce::Rectangle<int> engineBoxBounds;
    juce::Rectangle<int> hcBase, hcHarm, hcPrime;  // harmonic view's sub-cells (empty in custom view)
    juce::Rectangle<int> bothEnginesBounds;    // nested: what applies to EITHER engine
    // its sub-cells. bePhase sits to the RIGHT of beRange on the same row - the two
    // together are the row that beRange alone used to be, which is why the divider above
    // them is drawn from their union and not from either one.
    juce::Rectangle<int> beSlope, beFractal, beTrans, beRange, bePhase;
    // The TRANSITION item (1 Off / 2 Morph / 3 Dry/Wet) the box was last set to show.
    // -1 = never applied, so the first timer tick writes it. Cached for the same reason
    // lastManualFrozenShown is: this runs at timer rate and the work it guards swaps
    // visibility, re-lays the cell and repaints.
    int lastTransitionShown = -1;
    void applyTransitionMode();
    // Write the two booleans the TRANSITION box is a view of, as one host gesture each.
    void setTransitionParams (bool morphOn, bool dryWetOn);
    // The ONE writer of the two Time faders' colour (see applySlopeFaderColour).
    void applyTransitionFaderColour();
    juce::Rectangle<int> essentialsBounds;     // Essentials macro-group frame
    // its 3 sub-cells (dividers/labels in paint). PHASE used to be a fourth and is now a
    // sub-section of the SPECTRUM panel, beside OCTAVE RANGE.
    juce::Rectangle<int> ecGain, ecRef, ecOct;
    // BINAURAL shares the OCTAVE row, to its right, behind its own divider - the same
    // shape the user asked for and the same shape TEMPER has beside HARMONICS. Empty when
    // the row cannot hold both, which is what hides the fader and its painted title.
    juce::Rectangle<int> ecBin;
    juce::Slider binauralFader;
    std::unique_ptr<SliderAttachment> binauralAtt;
    juce::Rectangle<int> lBarBounds, lFootBounds; // the inverted-L Tuning box: dissonance BAR (right) + controls FOOT (bottom-left)
    void refreshDegreeList (bool showNotice = true); // generated-scale shortfall notice
    void updateScaleDissonance();  // recompute the active-scale dissonance when timbre/tuning change
    void rebuildScaleDissList();   // (re)build + sort degreeList.rows from the cached activeDiss
    void commitRefA();       // parse the Reference-A text box -> set the refA parameter
    void refreshRefAText();  // set the Reference-A text box from the current parameter
    // ...and the ONE formatter it and the constructor share: the value at the most
    // decimals `room` px of the field's own font can hold, integer part never dropped.
    juce::String refATextForRoom (double c3, int room) const;
    // NOTE NAMES. `noteNamesOn` mirrors the persisted preference (the processor owns it);
    // the cache is the spelling of the CURRENT tuning, keyed on its EDO so it is rebuilt
    // only when the answer can have changed. See refreshDegreeNames().
    void         refreshDegreeNames();
    juce::String noteNamesReason() const;
    bool         scaleIsJustIntonation() const;
    bool                      noteNamesOn = false;
    int                       degreeNameEdo = -1;  // -1 = never built, 0 = "not an EDO"
    std::vector<juce::String> degreeNameCache;

    std::map<int, int> heldKeys;
    juce::uint32 lastFocusedMs = 0; // debounce: keep playing through brief focus flickers
    juce::uint32 lastFocusReclaimMs = 0; // rate limit on the hover-driven focus grab (see refocus)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FractalAdditiveAudioProcessorEditor)
};
