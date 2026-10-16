#pragma once

#include "MurmurHostProcessor.h"
#include "../PluginEditor.h"
#include <okstudio/RotaryKnob.h>
#include <okstudio/Theme.h>
#include <memory>
#include <vector>

namespace murmur
{
// One known instrument VST3, as listed in the picker.
struct InstrumentEntry
{
    juce::String name;
    juce::String vendor;
    juce::String file; // full path of the .vst3
};

// The list of instrument VST3s Murmur Host knows about. "Scan" walks the folders a DAW scans
// (and Ableton's custom folder), asks each .vst3 what it contains, and keeps only the
// instruments. The result is cached in the user's app-data folder so the list is there
// the next time without scanning again. Scanning loads each plugin briefly, so it runs on a
// background thread and only when the user presses Scan.
class InstrumentCatalog
{
public:
    InstrumentCatalog();

    const std::vector<InstrumentEntry>& entries() const { return list; }
    void loadCache();
    void saveCache() const;
    void setEntries(std::vector<InstrumentEntry> e) { list = std::move(e); }

    // Blocking; call from a background thread. Never lists Murmur Host itself.
    static std::vector<InstrumentEntry> scanInstalled(const std::function<bool()>& shouldExit);
    static juce::Array<juce::File> vst3Folders();
    static juce::File cacheFile();

private:
    std::vector<InstrumentEntry> list;
};

// The plugin window is the full Murmur editor, then one strip (instrument picker, Scan, Show
// or Hide GUI, Eject, Mix), then the hosted instrument's own editor embedded below. Mouse-only:
// single left-click, targets >= 34 px, no typing.
class MurmurHostEditor : public juce::AudioProcessorEditor,
                         private juce::ChangeListener,
                         private juce::ComponentListener
{
public:
    explicit MurmurHostEditor(MurmurHostProcessor&);
    ~MurmurHostEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class ScanThread;

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void componentMovedOrResized(juce::Component&, bool, bool wasResized) override;

    void rebuildPicker();
    void startScan();
    void pickerChanged();
    void openInstrumentEditor();  // (re)build the embedded GUI from the current instance
    void closeInstrumentEditor(); // must run before the instance goes away
    void refreshStatus();
    void fitWindow();
    int instrumentEditorHeight() const;

    MurmurHostProcessor& host;
    okstudio::theme::LookAndFeel lnf;

    MurmurEditor murmurEditor;
    InstrumentCatalog catalog;

    juce::Label instrumentLabel, status, mixLabel, mixValue;
    juce::ComboBox picker;
    juce::TextButton scanButton { "Scan" }, guiButton { "Hide GUI" }, ejectButton { "Eject" };
    okstudio::RotaryKnob mixKnob;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAtt;

    std::unique_ptr<juce::AudioProcessorEditor> instEditor;
    bool instShown = true;
    bool scanning = false;
    std::unique_ptr<ScanThread> scanThread;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MurmurHostEditor)
};
} // namespace murmur
