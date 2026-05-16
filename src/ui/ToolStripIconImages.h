#pragma once

#include <juce_graphics/juce_graphics.h>

enum class EditTool;

namespace mini_daw_ui
{

/// Draws the Pointer or Split tool PNG (magenta keyed to transparent) centered in `iconArea`.
void drawToolStripToolGlyph(juce::Graphics& g, EditTool tool, juce::Rectangle<float> iconArea);

} // namespace mini_daw_ui
