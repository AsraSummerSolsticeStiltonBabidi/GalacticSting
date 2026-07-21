#pragma once

#include <juce_graphics/juce_graphics.h>

// The whole UI (everything except the Kablammo main title) renders in Comic Sans,
// loaded once from the user's own Windows font file - see PluginEditor.cpp. This
// single declaration lets every translation unit (editor, keyboard, nested paint
// helpers) ask for a comic-face Font at a given height.
namespace galactic
{
    juce::Font comicFont (float height);
}
