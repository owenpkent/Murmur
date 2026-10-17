#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <okstudio/MouseOnly.h>
#include <okstudio/Theme.h>
#include <functional>
#include <memory>
#include <vector>

namespace murmur::ui
{
// A row of mutually exclusive segments bound to a choice parameter. Left-click only:
// click a segment to pick it. Every segment is at least minHitPx tall. The big Mode
// toggle and the grid-division control are both this.
//
// Each segment is also an invisible child that UI Automation sees as a button named by its
// label, so a screen reader (and scripts/capture-window.ps1 -InvokeButtons) can press it.
// The children never take the mouse; the parent does all the drawing and hit testing.
class Segmented : public juce::Component
{
public:
    Segmented(juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID,
              const juce::StringArray& labels, float fontSize)
        : names(labels), font(fontSize),
          param(apvts.getParameter(paramID))
    {
        okstudio::ui::makeMouseOnly(*this);
        makeSegments();
        attachment = std::make_unique<juce::ParameterAttachment>(
            *param, [this](float v) { setSelectedFromNormalised(v); }, nullptr);
        attachment->sendInitialUpdate();
    }

    // Pure view state, not bound to any parameter (the Live | Edit toggle, the Take Editor
    // mode). onChange fires on a click that picks a different segment.
    Segmented(const juce::StringArray& labels, float fontSize, std::function<void(int)> onChange,
              int initial = 0)
        : names(labels), font(fontSize), param(nullptr), selected(initial), changed(std::move(onChange))
    {
        okstudio::ui::makeMouseOnly(*this);
        makeSegments();
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
    void mouseDown(const juce::MouseEvent& e) override { pick(indexAt(e.x)); }

    void resized() override
    {
        for (int i = 0; i < (int) segments.size(); ++i)
            segments[(size_t) i]->setBounds(getWidth() * i / names.size(), 0,
                                            getWidth() * (i + 1) / names.size() - getWidth() * i / names.size(),
                                            getHeight());
    }

    // A click on segment i, from the mouse or from UI Automation.
    void pick(int i)
    {
        if (i < 0 || i >= names.size())
            return;
        if (attachment != nullptr)
        {
            attachment->setValueAsCompleteGesture((float) i);
        }
        else if (i != selected)
        {
            selected = i;
            repaint();
            if (changed)
                changed(i);
        }
    }

private:
    struct Segment : juce::Component
    {
        Segment(Segmented& o, int i) : owner(o), index(i)
        {
            setTitle(o.names[i]);
            setInterceptsMouseClicks(false, false);
            setWantsKeyboardFocus(false);
        }
        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
        {
            return std::make_unique<juce::AccessibilityHandler>(
                *this, juce::AccessibilityRole::button,
                juce::AccessibilityActions().addAction(juce::AccessibilityActionType::press,
                                                       [this] { owner.pick(index); }));
        }
        Segmented& owner;
        int index;
    };

    void makeSegments()
    {
        for (int i = 0; i < names.size(); ++i)
            addAndMakeVisible(*segments.emplace_back(std::make_unique<Segment>(*this, i)));
    }

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
    std::unique_ptr<juce::ParameterAttachment> attachment;
    int selected = 0, hover = -1;
    std::function<void(int)> changed;
    std::vector<std::unique_ptr<Segment>> segments;
};
} // namespace murmur::ui
