#pragma once

//==============================================================================
// THE EDITOR PHOTOGRAPHS ITSELF.
//
// "no lo hagas a ciegas, usa screenshots (mira tamanos de texto, alineacion,
//  distribucion, jerarquia de tamanos, ubicaciones, mayusculas y botones lo mas simple
//  y clara posible, espaciado, tipografia, resolucion, etc, piensa como un disenador
//  grafico, quiero que el sinte sea bonito, sin lagear)"
//
// A layout cannot be redesigned by someone who cannot see it, and until this existed
// nothing in this repository could: all four verifiers (SpectrumTests, TuningTests,
// RhythmVerify, FftVerify) construct the PROCESSOR, and RhythmVerify's run 35 is the
// only one that builds an editor at all - to measure rectangles, never to look at one.
//
// AND AN OS SCREENSHOT IS NOT AN OPTION. A full-screen capture once caught the user's
// private WhatsApp on the other monitor; a window-rect capture cut the right column off;
// synthetic clicks land on the wrong control under DPI virtualisation and once moved a
// real parameter; SetForegroundWindow minimises the app. So the picture is taken from
// INSIDE the process, by the component itself:
// juce::Component::createComponentSnapshot renders the component tree into an Image with
// no OS compositing whatsoever - it cannot see another window, another monitor, or one
// pixel outside the editor's own bounds, because there is no screen involved at all.
//
// HOW IT IS DRIVEN. juce_add_plugin generates the Standalone's main(), so there is no
// entry point of ours to edit; the command line is read through
// juce::JUCEApplicationBase, which is reachable from anywhere in the process. The single
// call site is the tail of the editor's constructor (see PluginEditor.cpp) and it does
// exactly this on an ordinary launch: ask JUCE whether this binary is a standalone app
// at all (false in every plugin wrapper), and if so compare the command line against one
// literal. No snapshot flag, no allocation of ours, no work, no behaviour change - the
// interactive path is the path that ships.
//
// Run it with:
//     "Fiblo Galactic.exe" --snapshot <outputDir> [--scale 2] [--only <substring>]
//==============================================================================

namespace fgsnap
{
    // Called once from the editor's constructor. Returns immediately - and does nothing
    // else, ever - unless this process is the Standalone AND its command line asks for a
    // snapshot run. When it does ask, the walk is posted to the message thread (so the
    // app has finished starting, fonts and LookAndFeel are warm, and the editors the rig
    // builds are not constructed inside another editor's constructor) and the app quits
    // when the last file is written.
    void runIfRequested();
}
