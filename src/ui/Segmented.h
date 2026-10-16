#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <okstudio/MouseOnly.h>
#include <okstudio/Theme.h>
#include <functional>

namespace murmur::ui
{
// A row of mutually exclusive segments bound to a choice parameter. Left-click only:
// click a segment to pick it. Every segment is at least minHitPx tall. The big Mode
// toggle and the grid-division control are both this.
class Segmented : public juce::Component
{
public:
    Segmented(juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID,
              const juce::StringArray& labels, float fontSize)
        : names(labels), font(fontSize),
          param(apvts.getParameter(paramID)),
          attachment(*param, [this](float v) { setSelectedFromNormalised(v); }, nullptr)
    {
        okstudio::ui::makeMouseOnly(*this);
        attachment.sendInitialUpdate();
    }

    int getSelected() const { return selected; }

    void paint(juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour(okstudio::theme::surface);
        g.fillRoundedRectangle(b, 8.0f);
        const float w = b.getWidth() / (float) names.size();
        for (int i = 0; i < names.size(); ++i)
        {
            auto seg = juce::Rectangle<float>(b.getX() + w * (float) i, b.getY(), w, b.getHeight()).reduced(3.0f);
            const bool on = i == selected;
            g.setColour(on ? okstudio::theme::accent : (i == hover ? okstudio::theme::surfaceHi : juce::Colours::transparentBlack));
            g.fillRoundedRectangle(seg, 6.0f);
            g.setColour(on ? okstudio::theme::background : okstudio::theme::text);
            g.setFont(juce::Font(juce::FontOptions(font, juce::Font::bold)));
            g.drawText(names[i], seg, juce::Justification::centred);
        }
        g.setColour(okstudio::theme::outline);
        g.drawRoundedRectangle(b.reduced(0.5f), 8.0f, 1.0f);
    }

    void mouseMove(const juce::MouseEvent& e) override { setHover(indexAt(e.x)); }
    void mouseExit(const juce::MouseEvent&) override { setHover(-1); }
    void mouseDown(const juce::MouseEvent& e) override
    {
        const int i = indexAt(e.x);
        if (i < 0 || param == nullptr)
            return;
        attachment.setValueAsCompleteGesture((float) i);
    }

private:
    int indexAt(int x) const
    {
        if (names.isEmpty() || getWidth() <= 0)
            return -1;
        return juce::jlimit(0, names.size() - 1, x * names.size() / getWidth());
    }
    void setHover(int i)
    {
        if (i != hover)
        {
            hover = i;
            repaint();
        }
    }
    void setSelectedFromNormalised(float v)
    {
        // Called by the attachment with a real (denormalised) value for choice params.
        selected = juce::jlimit(0, names.size() - 1, (int) std::lround(v));
        repaint();
    }

    juce::StringArray names;
    float font;
    juce::RangedAudioParameter* param;
    juce::ParameterAttachment attachment;
    int selected = 0, hover = -1;
};
} // namespace murmur::ui
