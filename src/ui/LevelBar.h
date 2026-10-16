#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <okstudio/Theme.h>

namespace murmur::ui
{
// A horizontal input level bar. Display only; the editor's timer feeds it 0..1.
class LevelBar : public juce::Component
{
public:
    void setLevel(float v)
    {
        // Fast attack, slow release so a hum reads as a steady bar.
        v = juce::jlimit(0.0f, 1.0f, v);
        shown = v > shown ? v : shown * 0.85f + v * 0.15f;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour(okstudio::theme::surface);
        g.fillRoundedRectangle(b, 5.0f);
        auto fill = b.reduced(2.0f);
        fill.setWidth(fill.getWidth() * shown);
        g.setColour(shown > 0.9f ? okstudio::theme::warn : okstudio::theme::good);
        g.fillRoundedRectangle(fill, 4.0f);
    }

private:
    float shown = 0.0f;
};
} // namespace murmur::ui
