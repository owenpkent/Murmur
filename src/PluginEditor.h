#pragma once

#include "PluginProcessor.h"
#include "ui/LevelBar.h"
#include "ui/Segmented.h"
#include <okstudio/RotaryKnob.h>
#include <okstudio/Theme.h>
#include <okstudio/Updater.h>
#include <array>

namespace murmur
{
// Macro-first and mouse-only. A big Melody | Drums toggle, three knobs (Sensitivity,
// Quantize, Swing), a grid segmented control, then the mode's own row: scale controls in
// Melody, a Calibrate row in Drums. A 30 Hz timer polls the engine's atomic getters.
class MurmurEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MurmurEditor(MurmurProcessor&);
    ~MurmurEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    using ComboAtt = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct Knob
    {
        okstudio::RotaryKnob slider;
        juce::Label name, value;
        std::unique_ptr<SliderAtt> att;
    };

    void timerCallback() override;
    void addKnob(Knob&, const juce::String& text, const juce::String& paramID);
    void addCombo(juce::ComboBox&, juce::Label&, const juce::String& text, const juce::StringArray& items,
                  const juce::String& paramID, std::unique_ptr<ComboAtt>&);
    void startCalibration(int drumClass);
    void showUpdate(const okstudio::updater::UpdateInfo&);
    void applyModeVisibility();
    static juce::String drumName(int note);

    MurmurProcessor& processor;
    okstudio::theme::LookAndFeel lnf;

    juce::Label title, subtitle, tempoReadout, lastReadout, levelLabel, gridLabel, calibHint;
    ui::Segmented modeToggle, gridToggle;
    Knob sensitivity, quantize, swing;
    ui::LevelBar levelBar;

    // Melody-only.
    juce::ComboBox rootBox, scaleBox;
    juce::Label rootLabel, scaleLabel;
    juce::ToggleButton scaleLockButton { "Scale Lock" };
    std::unique_ptr<ComboAtt> rootAtt, scaleAtt;
    std::unique_ptr<ButtonAtt> scaleLockAtt;

    // Drums-only.
    static constexpr int numClasses = 4;
    std::array<juce::TextButton, numClasses> calibButtons;
    std::array<juce::TextButton, numClasses> clearButtons;
    int calibratingClass = -1;

    juce::TextButton updateButton;
    okstudio::updater::Config updaterConfig;
    okstudio::updater::UpdateInfo pendingUpdate;

    bool drumsShown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MurmurEditor)
};
} // namespace murmur
