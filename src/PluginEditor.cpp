#include "PluginEditor.h"
#include "StandaloneSupport.h"
#include <okstudio/MouseOnly.h>
#include <okstudio/Scales.h>

namespace murmur
{
namespace
{
    constexpr int hitH = okstudio::ui::minHitPx + 10; // comfortable, above the kit minimum

    const char* const classNames[4] = { "Kick", "Snare", "Closed Hat", "Open Hat" };

    void styleLabel(juce::Label& l, const juce::String& text, float size = 12.0f,
                    juce::Colour colour = okstudio::theme::textDim,
                    juce::Justification j = juce::Justification::centredLeft)
    {
        l.setText(text, juce::dontSendNotification);
        l.setFont(juce::Font(juce::FontOptions(size, juce::Font::bold)));
        l.setColour(juce::Label::textColourId, colour);
        l.setJustificationType(j);
        l.setInterceptsMouseClicks(false, false);
    }
} // namespace

MurmurEditor::MurmurEditor(MurmurProcessor& p, bool checkForUpdates)
    : juce::AudioProcessorEditor(p), processor(p),
      modeToggle(p.apvts, "mode", { "Melody", "Drums" }, 24.0f),
      gridToggle(p.apvts, "grid", { "1/4", "1/8", "1/16", "1/8T", "1/16T", "1/32" }, 14.0f),
      viewToggle({ "Live", "Edit" }, 16.0f, [this](int i)
                 {
                     editShown = i == 1;
                     applyModeVisibility();
                     if (onViewChanged)
                         onViewChanged();
                     resized();
                 }),
      takeView(p)
{
    setLookAndFeel(&lnf);
    okstudio::ui::makeMouseOnly(*this);
    if (p.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
        standalone::unmuteInput(); // Murmur is deaf otherwise; see StandaloneSupport.h

    styleLabel(title, "Murmur", 26.0f, okstudio::theme::text);
    styleLabel(subtitle, "voice to MIDI", 13.0f);
    styleLabel(tempoReadout, "", 14.0f, okstudio::theme::accentSoft, juce::Justification::centredRight);
    styleLabel(lastReadout, "Last: none", 22.0f, okstudio::theme::text);
    styleLabel(levelLabel, "Input", 12.0f);
    styleLabel(gridLabel, "Grid", 12.0f);
    addAndMakeVisible(gridLabel);
    styleLabel(calibHint, "", 14.0f, okstudio::theme::warn);
    addAndMakeVisible(title);
    addAndMakeVisible(subtitle);
    addAndMakeVisible(tempoReadout);
    addAndMakeVisible(lastReadout);
    addAndMakeVisible(levelLabel);
    addAndMakeVisible(calibHint);
    addAndMakeVisible(levelBar);
    addAndMakeVisible(modeToggle);
    addAndMakeVisible(gridToggle);
    addAndMakeVisible(viewToggle);
    addChildComponent(takeView);

    addKnob(sensitivity, "Sensitivity", "sensitivity");
    addKnob(quantize, "Quantize", "quantize");
    addKnob(swing, "Swing", "swing");

    addCombo(rootBox, rootLabel, "Root", okstudio::scales::noteNames(), "root", rootAtt);
    addCombo(scaleBox, scaleLabel, "Scale", okstudio::scales::names(), "scale", scaleAtt);
    okstudio::ui::makeMouseOnly(scaleLockButton);
    addAndMakeVisible(scaleLockButton);
    scaleLockAtt = std::make_unique<ButtonAtt>(processor.apvts, "scaleLock", scaleLockButton);

    for (int i = 0; i < numClasses; ++i)
    {
        calibButtons[(size_t) i].setButtonText(classNames[i]);
        clearButtons[(size_t) i].setButtonText("Clear");
        okstudio::ui::makeMouseOnly(calibButtons[(size_t) i]);
        okstudio::ui::makeMouseOnly(clearButtons[(size_t) i]);
        calibButtons[(size_t) i].onClick = [this, i] { startCalibration(i); };
        clearButtons[(size_t) i].onClick = [this, i]
        {
            processor.getEngine().clearCalibration(static_cast<dsp::MurmurEngine::DrumClass>(i));
            if (calibratingClass == i)
                calibratingClass = -1;
        };
        addChildComponent(calibButtons[(size_t) i]);
        addChildComponent(clearButtons[(size_t) i]);
    }

    updateButton.setVisible(false);
    updateButton.setColour(juce::TextButton::buttonColourId, okstudio::theme::good.withAlpha(0.85f));
    okstudio::ui::makeMouseOnly(updateButton);
    addChildComponent(updateButton);

    updaterConfig.productName = "Murmur";
    updaterConfig.releasesRepo = "okstudio1/murmur-releases";
    updaterConfig.currentVersion = juce::String(MURMUR_VERSION);
    updaterConfig.assetPrefix = "MurmurSetup-";
    if (checkForUpdates)
    {
        juce::Component::SafePointer<MurmurEditor> safe(this);
        okstudio::updater::checkAsync(updaterConfig, [safe](okstudio::updater::UpdateInfo info)
        {
            if (auto* e = safe.getComponent())
                e->showUpdate(info);
        });
    }

    setResizable(true, true);
    setResizeLimits(680, 500, 1800, 1200);
    setSize(defaultWidth, defaultHeight);

    drumsShown = processor.isDrumsMode();
    applyModeVisibility();
    startTimerHz(30);
}

MurmurEditor::~MurmurEditor()
{
    stopTimer();
    setLookAndFeel(nullptr);
}

void MurmurEditor::addKnob(Knob& k, const juce::String& text, const juce::String& paramID)
{
    k.slider.setRange(0.0, 1.0, 0.001);
    addAndMakeVisible(k.slider);
    styleLabel(k.name, text, 13.0f, okstudio::theme::text, juce::Justification::centred);
    styleLabel(k.value, "", 12.0f, okstudio::theme::textDim, juce::Justification::centred);
    addAndMakeVisible(k.name);
    addAndMakeVisible(k.value);
    k.att = std::make_unique<SliderAtt>(processor.apvts, paramID, k.slider);
    auto* valueLabel = &k.value;
    auto* slider = &k.slider;
    k.slider.onValueChange = [valueLabel, slider]
    {
        valueLabel->setText(juce::String(juce::roundToInt(slider->getValue() * 100.0)) + "%",
                            juce::dontSendNotification);
    };
    k.slider.onValueChange();
}

void MurmurEditor::addCombo(juce::ComboBox& box, juce::Label& label, const juce::String& text,
                            const juce::StringArray& items, const juce::String& paramID,
                            std::unique_ptr<ComboAtt>& att)
{
    styleLabel(label, text);
    addAndMakeVisible(label);
    box.addItemList(items, 1);
    okstudio::ui::makeMouseOnly(box);
    addAndMakeVisible(box);
    att = std::make_unique<ComboAtt>(processor.apvts, paramID, box);
}

void MurmurEditor::startCalibration(int drumClass)
{
    calibratingClass = drumClass;
    processor.getEngine().beginCalibration(static_cast<dsp::MurmurEngine::DrumClass>(drumClass));
}

void MurmurEditor::showUpdate(const okstudio::updater::UpdateInfo& info)
{
    pendingUpdate = info;
    updateButton.setButtonText("Update to v" + info.version);
    updateButton.setVisible(true);
    juce::Component::SafePointer<MurmurEditor> safe(this);
    updateButton.onClick = [this, safe]
    {
        updateButton.setEnabled(false);
        updateButton.setButtonText("Starting" + juce::String::fromUTF8("\xe2\x80\xa6"));
        okstudio::updater::downloadAndInstallAsync(updaterConfig, pendingUpdate,
                                                   [safe](juce::String status, bool failed)
        {
            if (auto* e = safe.getComponent())
            {
                e->updateButton.setButtonText(status);
                e->updateButton.setEnabled(failed);
            }
        });
    };
    resized();
}

juce::String MurmurEditor::drumName(int note)
{
    switch (note)
    {
        case 36: return "Kick";
        case 38: return "Snare";
        case 42: return "Closed Hat";
        case 46: return "Open Hat";
        default: return juce::MidiMessage::getMidiNoteName(note, true, true, 3);
    }
}

void MurmurEditor::applyModeVisibility()
{
    // Live view: today's controls. Edit view: only the header and the take view.
    const bool live = ! editShown;
    const bool drums = drumsShown;
    for (auto* c : std::initializer_list<juce::Component*> { &modeToggle, &gridToggle, &gridLabel, &levelBar,
                                                            &levelLabel, &lastReadout, &sensitivity.slider,
                                                            &sensitivity.name, &sensitivity.value, &quantize.slider,
                                                            &quantize.name, &quantize.value, &swing.slider,
                                                            &swing.name, &swing.value })
        c->setVisible(live);
    for (auto* c : std::initializer_list<juce::Component*> { &rootBox, &scaleBox, &rootLabel, &scaleLabel,
                                                            &scaleLockButton })
        c->setVisible(live && ! drums);
    for (int i = 0; i < numClasses; ++i)
    {
        calibButtons[(size_t) i].setVisible(live && drums);
        clearButtons[(size_t) i].setVisible(live && drums);
    }
    calibHint.setVisible(live && drums);
    takeView.setVisible(editShown);
    takeView.setDrumsMode(drums);
    repaint();
}

void MurmurEditor::timerCallback()
{
    auto& engine = processor.getEngine();

    const bool drums = processor.isDrumsMode();
    if (drums != drumsShown)
    {
        drumsShown = drums;
        calibratingClass = -1;
        applyModeVisibility();
        resized();
    }

    levelBar.setLevel(engine.getInputLevel());

    const int note = engine.getLastNote();
    if (note < 0)
        lastReadout.setText(drums ? "Last hit: none" : "Last note: none", juce::dontSendNotification);
    else
        lastReadout.setText((drums ? "Last hit: " : "Last note: ")
                                + (drums ? drumName(note) : juce::MidiMessage::getMidiNoteName(note, true, true, 3)),
                            juce::dontSendNotification);

    // Free tempo only matters when the host transport is stopped (no grid to follow).
    if (processor.isHostPlaying())
    {
        tempoReadout.setText("", juce::dontSendNotification);
    }
    else
    {
        const double bpm = engine.getInferredBpm();
        tempoReadout.setText(bpm > 0.0 ? "Free tempo: " + juce::String(juce::roundToInt(bpm)) + " BPM"
                                       : juce::String("Free tempo: listening"),
                             juce::dontSendNotification);
    }

    // Calibration lights: on while the engine is collecting examples for the chosen class.
    const bool calibrating = engine.isCalibrating();
    if (! calibrating)
        calibratingClass = -1;
    for (int i = 0; i < numClasses; ++i)
    {
        const bool lit = calibrating && calibratingClass == i;
        calibButtons[(size_t) i].setColour(juce::TextButton::buttonColourId,
                                           lit ? okstudio::theme::warn : okstudio::theme::surfaceHi);
        calibButtons[(size_t) i].setColour(juce::TextButton::textColourOffId,
                                           lit ? okstudio::theme::background : okstudio::theme::text);
        calibButtons[(size_t) i].repaint();
    }
    calibHint.setText(calibrating ? juce::String("Make the ") + classNames[juce::jmax(0, calibratingClass)]
                                          + " sound 3 times"
                                  : juce::String("Pick a sound to teach Murmur your beatbox"),
                      juce::dontSendNotification);
}

void MurmurEditor::paint(juce::Graphics& g)
{
    g.fillAll(okstudio::theme::background);
    g.setColour(okstudio::theme::panel);
    g.fillRect(getLocalBounds().removeFromTop(70));
}

void MurmurEditor::resized()
{
    auto area = getLocalBounds().reduced(16);

    auto header = area.removeFromTop(54);
    title.setBounds(header.removeFromLeft(130));
    subtitle.setBounds(header.removeFromLeft(100).withTrimmedTop(18));
    viewToggle.setBounds(header.removeFromLeft(180).withSizeKeepingCentre(180, hitH));
    header.removeFromLeft(12);
    if (updateButton.isVisible())
        updateButton.setBounds(header.removeFromRight(170).withSizeKeepingCentre(170, hitH));
    tempoReadout.setBounds(header);

    if (editShown)
    {
        area.removeFromTop(8);
        takeView.setBounds(area);
        return;
    }

    area.removeFromTop(22);
    modeToggle.setBounds(area.removeFromTop(68));
    area.removeFromTop(16);

    // Middle band: knobs on the left, grid + level + last note on the right.
    auto middle = area.removeFromTop(juce::jmax(150, area.getHeight() / 2 - 20));
    auto knobs = middle.removeFromLeft(juce::jmin(middle.getWidth() / 2, 420));
    const int kw = knobs.getWidth() / 3;
    Knob* ks[3] = { &sensitivity, &quantize, &swing };
    for (auto* k : ks)
    {
        auto col = knobs.removeFromLeft(kw);
        k->name.setBounds(col.removeFromTop(22));
        k->value.setBounds(col.removeFromBottom(20));
        k->slider.setBounds(col.withSizeKeepingCentre(juce::jmin(col.getWidth(), col.getHeight(), 96),
                                                       juce::jmin(col.getWidth(), col.getHeight(), 96)));
    }

    middle.removeFromLeft(24);
    auto side = middle;
    gridLabel.setBounds(side.removeFromTop(20));
    gridToggle.setBounds(side.removeFromTop(hitH));
    side.removeFromTop(16);
    auto levelRow = side.removeFromTop(26);
    levelLabel.setBounds(levelRow.removeFromLeft(54));
    levelBar.setBounds(levelRow);
    side.removeFromTop(14);
    lastReadout.setBounds(side.removeFromTop(36));

    area.removeFromTop(8);

    // Bottom band: mode-specific.
    auto bottom = area;
    if (drumsShown)
    {
        calibHint.setBounds(bottom.removeFromTop(28));
        bottom.removeFromTop(8);
        const int cw = bottom.getWidth() / numClasses;
        for (int i = 0; i < numClasses; ++i)
        {
            auto col = bottom.withX(bottom.getX() + cw * i).withWidth(cw).reduced(6, 0);
            calibButtons[(size_t) i].setBounds(col.removeFromTop(56));
            col.removeFromTop(8);
            clearButtons[(size_t) i].setBounds(col.removeFromTop(hitH).withSizeKeepingCentre(juce::jmin(col.getWidth(), 110), hitH));
        }
    }
    else
    {
        auto row = bottom.removeFromTop(20);
        const int colW = juce::jmin(220, bottom.getWidth() / 3);
        rootLabel.setBounds(row.removeFromLeft(colW));
        row.removeFromLeft(12);
        scaleLabel.setBounds(row.removeFromLeft(colW));
        auto controls = bottom.removeFromTop(hitH);
        rootBox.setBounds(controls.removeFromLeft(colW));
        controls.removeFromLeft(12);
        scaleBox.setBounds(controls.removeFromLeft(colW));
        controls.removeFromLeft(24);
        scaleLockButton.setBounds(controls.removeFromLeft(170));
    }
}
} // namespace murmur
