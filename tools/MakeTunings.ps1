# Regenerates assets/tunings/*.tun - the six factory tunings that ship INSIDE the
# binary (see juce_add_binary_data(GalacticAssets ...) in CMakeLists.txt).
#
# WHY A GENERATOR AND NOT SIX HAND-WRITTEN FILES: each .tun carries 128 exact
# frequencies to ten decimal places plus a 128-line integer fallback, so a hand-edited
# file is 260 lines of numbers nobody can proof-read and nobody can re-derive. The
# scales themselves are four lines of arithmetic each; everything else is output. Same
# argument as tools/PaletteSearch.cpp: the artefact is a RESULT, so the derivation
# lives in the tree beside it.
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools/MakeTunings.ps1
#
#-------------------------------------------------------------------------------
# WHY .tun AND NOT .scl
#
# TuningManager::loadScalaFile reads both, but only .tun states an ABSOLUTE reference
# frequency; a .scl states ratios and leaves the anchor to whoever loads it. The user
# asked for .tun, and .tun is also what the plugin's own Save button writes - a factory
# tuning has to be the same kind of file as one the user exports.
#
# WHAT THE PLUGIN'S PARSER NEEDS, line by line (source/dsp/TuningManager.h):
#
#   * parseTun() refuses a file that specifies fewer than ALL 128 MIDI notes ("the rest
#     would have to be invented"), so every file below writes all 128 - including the
#     ones whose top keys land far above hearing. That is not a bug: a .tun IS a 128-key
#     map, and Bohlen-Pierce really does reach 69 kHz thirteen steps at a time.
#   * [Exact Tuning] wins over [Tuning] whenever it is present. [Tuning] is written too,
#     rounded to whole cents, as the v1 fallback for readers that predate the exact block
#     - exactly what TuningManager::writeTunText does.
#   * tunToScala() then tries to hold the file as a SCALE (degrees + period) rather than
#     as an anonymous 128-key table, and it tries the file's OWN [Scale Begin] block
#     first. Every file below declares one. Note honestly what that IS and IS NOT worth
#     for these six: without a block the fallback SEARCH takes the smallest exact repeat
#     (31ED2 repeats perfectly well at one key, i.e. "1 degree per 38.7097 cents") and
#     only overrides it when a CANONICAL period also repeats - and every period here,
#     2/1 and 3/1 alike, is on that canonical list. So the search would land on the
#     right answer anyway. The block is written because it is what the plugin's own
#     exporter writes, because it makes the files self-describing to other software,
#     and because it is the only thing that saves a scale whose period is NOT canonical
#     (TuningManager's own example: 9 equal degrees per 1000 cents came back as 1 per
#     111.111). It is belt-and-braces here, not the load-bearing wall.
#   * The block is VERIFIED, never trusted: tryStructure re-derives all 128 keys from it
#     and refuses it if any key misses by more than 0.01 cents. Ten decimal places of
#     cents is ~1e-7 cents of round-trip error, four orders below that tolerance.
#
# THE ONE THING .tun COSTS: tunToScala converts to .scl text in CENTS, so a degree that
# was written as a ratio arrives as a tempered value and TuningManager::DegreeSrc marks
# it non-rational. The harmonic-series file below therefore shows "~9/8 *" rather than
# an exact "9/8" on the keyboard's interval labels. The pitches are exact either way -
# it is only the CLAIM of exactness that a .tun cannot carry. If those labels matter
# more than the absolute reference, that file (and only that file) wants to be a .scl.
#-------------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'
$inv = [System.Globalization.CultureInfo]::InvariantCulture

$outDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'assets\tunings'
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

# The plugin's own anchor: scale degree 0 sits on MIDI key 60 at 12-TET middle C
# (TuningManager::kRefKey / kRefFreq). Anchoring the factory files here means loading
# one changes the SCALE and nothing else - a 12ed2.tun loaded on top of 12-TET is
# silent, note for note, which is the only way to hear that the loader is honest.
$kRefKey  = 60
$kRefFreq = 261.6255653005986

function Format-Fixed([double] $v, [int] $dp)
{
    # InvariantCulture is not optional: this machine's locale writes a decimal COMMA,
    # and TuningManager::isTunNumber rejects any character outside "0123456789.+-eE".
    # A file generated under es-ES would be refused at the door by its own plugin.
    return $v.ToString('F' + $dp, $inv)
}

function Write-Tun
{
    param(
        [string]   $fileName,   # e.g. "7ed2.tun"
        [string]   $name,       # what the plugin shows as the scale's name
        [string]   $blurb,      # one line of [Info] Description
        [double[]] $ratios,     # degree 0..N-1 as frequency ratios; ratios[0] must be 1
        [double]   $period,     # the repeat interval as a ratio (2 = octave, 3 = tritave)
        [string[]] $notes       # header comment lines (';' is the .tun comment character)
    )

    # NOT $N. PowerShell variable names are CASE-INSENSITIVE, so $N and the key loop's
    # $n are one variable: the first iteration set the degree count to 0, every later
    # key divided by it, and the whole thing died on "cannot convert NaN to Int32"
    # rather than on anything resembling its cause.
    $degrees = $ratios.Length

    # The 128 keys, built the way TuningManager::loadScalaString builds them, so the
    # file and the loader cannot disagree about what key 60 is.
    $freq = New-Object 'double[]' 128
    for ($n = 0; $n -lt 128; $n++)
    {
        $d    = $n - $kRefKey
        $oct  = [Math]::Floor([double]$d / [double]$degrees)
        $step = $d - $oct * $degrees
        $freq[$n] = $kRefFreq * [Math]::Pow($period, $oct) * $ratios[[int]$step]
    }

    $base  = $freq[0]
    $cents = New-Object 'double[]' 128
    for ($n = 0; $n -lt 128; $n++) { $cents[$n] = 1200.0 * [Math]::Log($freq[$n] / $base, 2.0) }

    $sb = New-Object System.Text.StringBuilder
    $add = { param($line) [void]$sb.Append($line).Append("`n") }

    & $add "; AnaMark TUN v2 - a factory tuning that ships inside Fiblo Galactic."
    & $add "; Generated by tools/MakeTunings.ps1 - edit that, not this."
    & $add ";"
    foreach ($l in $notes) { & $add ("; " + $l) }
    & $add ";"
    & $add ("; Degree 0 is MIDI key " + $kRefKey + " at " + (Format-Fixed $kRefFreq 10) + " Hz (12-TET middle C),")
    & $add "; which is where the plugin anchors a scale's 1/1."
    & $add ""
    & $add "[Info]"
    & $add ("Name=" + $name)
    & $add "Author=Fiblo Galactic"
    & $add ("Description=" + $blurb)
    & $add ""

    # The structure, declared. tunToScala reads exactly two numbers out of this block -
    # the top degree index (= degrees per period) and its cents value (= the period) -
    # and proves both against the 128 pitches below before believing either.
    & $add "[Scale Begin]"
    & $add ("; " + $degrees + " degrees per period of " + (Format-Fixed (1200.0 * [Math]::Log($period, 2.0)) 6) + " cents")
    & $add ("basekey=" + $kRefKey)
    for ($j = 0; $j -lt $degrees; $j++)
    {
        & $add ("degree " + $j + "=" + (Format-Fixed (1200.0 * [Math]::Log($ratios[$j], 2.0)) 10))
    }
    & $add ("degree " + $degrees + "=" + (Format-Fixed (1200.0 * [Math]::Log($period, 2.0)) 10))
    & $add "[Scale End]"
    & $add ""

    & $add "; Whole cents, for readers that predate [Exact Tuning]. Ignored by Fiblo."
    & $add "[Tuning]"
    for ($n = 0; $n -lt 128; $n++)
    {
        & $add ("note " + $n + "=" + [Math]::Round($cents[$n], [System.MidpointRounding]::AwayFromZero).ToString('F0', $inv))
    }
    & $add ""

    & $add "; The block that actually carries the pitch."
    & $add "[Exact Tuning]"
    & $add ("basefreq=" + (Format-Fixed $base 10))
    for ($n = 0; $n -lt 128; $n++)
    {
        & $add ("note " + $n + "=" + (Format-Fixed $cents[$n] 10))
    }

    $path = Join-Path $outDir $fileName
    # LF and no BOM: .gitattributes normalises the repo to LF, and a BOM would sit in
    # front of the first comment character where nothing expects it.
    [System.IO.File]::WriteAllText($path, $sb.ToString(), (New-Object System.Text.UTF8Encoding $false))
    Write-Host ("  wrote " + $fileName + "  (" + $degrees + " degrees, period " + (Format-Fixed $period 6) + ", base " + (Format-Fixed $base 6) + " Hz)")
}

function Equal-Divisions([int] $n, [double] $period)
{
    $r = New-Object 'double[]' $n
    for ($i = 0; $i -lt $n; $i++) { $r[$i] = [Math]::Pow($period, [double]$i / [double]$n) }
    return $r
}

Write-Host "Regenerating assets/tunings ..."

# 1. THE HARMONIC SERIES, harmonics 8..16. The only unequal scale here, and the one
#    this synth exists for: its degrees ARE the partials the additive engine builds,
#    so a note played on it lands on a harmonic of the tonic rather than near one.
#    Harmonic 8 is the 1/1 because it is the lowest harmonic whose next eight
#    neighbours all fall inside one octave - 8..16 is the octave, cut by nature.
# Every quotient is parenthesised because PowerShell's comma binds TIGHTER than "/":
# @(1.0, 9.0/8.0, 10.0/8.0) parses as (1.0,9.0)/(8.0,10.0)/8.0 and dies with
# "[System.Object[]] does not contain a method named op_Division" - a wrong scale that
# announces itself, luckily, rather than a wrong scale that loads.
$harmonic8to16 = @(1.0, (9.0/8.0), (10.0/8.0), (11.0/8.0), (12.0/8.0), (13.0/8.0), (14.0/8.0), (15.0/8.0))

Write-Tun -fileName 'harmonic-series-8-16.tun' -name 'harmonic series 8-16' `
    -blurb 'Harmonics 8 to 16 over harmonic 8: 9/8 5/4 11/8 3/2 13/8 7/4 15/8 2/1' `
    -ratios $harmonic8to16 `
    -period 2.0 `
    -notes @('Harmonic series, partials 8 to 16, with harmonic 8 as the 1/1.',
             'Degrees: 9/8 10/8 11/8 12/8 13/8 14/8 15/8, repeating at 16/8 = 2/1.',
             'Two of them (11/8 and 13/8) exist nowhere in 12-TET.')

# 2..5. Equal divisions of the octave. 12ed2 is here on purpose even though the synth
#    has a 12-TET mode: it is the control. Loading it must change nothing audible, and
#    that is the cheapest way for a user to tell a broken tuning path from a broken ear.
Write-Tun -fileName '7ed2.tun' -name '7ed2' `
    -blurb '7 equal divisions of the octave (171.4286 cents per step)' `
    -ratios (Equal-Divisions 7 2.0) -period 2.0 `
    -notes @('7 equal divisions of the octave: every step 1200/7 = 171.4286 cents.',
             'A seven-note keyboard with no black keys - close to the equipentatonic',
             'and gamelan-adjacent tunings, and nothing like a diatonic scale.')

Write-Tun -fileName '12ed2.tun' -name '12ed2' `
    -blurb '12 equal divisions of the octave - standard equal temperament' `
    -ratios (Equal-Divisions 12 2.0) -period 2.0 `
    -notes @('12 equal divisions of the octave: ordinary equal temperament.',
             'Included as the CONTROL. Loading it on top of the 12-TET mode must be',
             'inaudible, note for note - if it is not, the tuning path is wrong and',
             'not the ear.')

Write-Tun -fileName '24ed2.tun' -name '24ed2' `
    -blurb '24 equal divisions of the octave - quarter tones' `
    -ratios (Equal-Divisions 24 2.0) -period 2.0 `
    -notes @('24 equal divisions of the octave: quarter tones.',
             'Contains 12ed2 exactly, so every ordinary interval survives and the new',
             'twelve sit halfway between them.')

Write-Tun -fileName '31ed2.tun' -name '31ed2' `
    -blurb '31 equal divisions of the octave - near-exact meantone, very close 5/4 and 7/4' `
    -ratios (Equal-Divisions 31 2.0) -period 2.0 `
    -notes @('31 equal divisions of the octave: each step 38.7097 cents.',
             'Its major third is 0.8 cents from a just 5/4 and its harmonic seventh is',
             '1.1 cents from 7/4, which is why 31 keeps being rediscovered.')

# 6. BOHLEN-PIERCE. The one file here whose PERIOD is not the octave, and therefore the
#    one that proves the period is read from the file rather than assumed: 13 equal
#    divisions of 3/1, no octave anywhere in it.
Write-Tun -fileName 'bohlen-pierce.tun' -name 'Bohlen-Pierce' `
    -blurb '13 equal divisions of the tritave 3/1 (146.3042 cents per step)' `
    -ratios (Equal-Divisions 13 3.0) -period 3.0 `
    -notes @('Bohlen-Pierce: 13 equal divisions of 3/1, the tritave.',
             'There is NO octave in this tuning. It repeats at a twelfth, so the whole',
             'keyboard spans far more than a 2/1 scale would - the top keys really do',
             'reach past hearing, which is what a 128-key map of a wide period looks',
             'like and not a fault in the file.')

Write-Host "Done."
