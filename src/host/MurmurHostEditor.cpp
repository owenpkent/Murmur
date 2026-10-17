#include "MurmurHostEditor.h"
#include <okstudio/MouseOnly.h>
#include <algorithm>

namespace murmur
{
namespace
{
    constexpr int stripH = 96;
    constexpr int hitH = okstudio::ui::minHitPx + 10;

    void styleLabel(juce::Label& l, const juce::String& text, float size, juce::Colour colour,
                    juce::Justification j = juce::Justification::centredLeft)
    {
        l.setText(text, juce::dontSendNotification);
        l.setFont(juce::Font(juce::FontOptions(size, juce::Font::bold)));
        l.setColour(juce::Label::textColourId, colour);
        l.setJustificationType(j);
        l.setInterceptsMouseClicks(false, false);
    }
} // namespace

// ---- InstrumentCatalog -------------------------------------------------------------------

InstrumentCatalog::InstrumentCatalog() { loadCache(); }

juce::File InstrumentCatalog::cacheFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("OK Studio").getChildFile("Murmur").getChildFile("HostInstruments.xml");
}

juce::Array<juce::File> InstrumentCatalog::vst3Folders()
{
    juce::Array<juce::File> dirs;
   #if JUCE_WINDOWS
    dirs.add(juce::File("C:\\Program Files\\Common Files\\VST3"));
    dirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                 .getChildFile("Ableton").getChildFile("vst3"));
   #elif JUCE_MAC
    dirs.add(juce::File("/Library/Audio/Plug-Ins/VST3"));
    dirs.add(juce::File("~/Library/Audio/Plug-Ins/VST3"));
   #endif
    return dirs;
}

void InstrumentCatalog::loadCache()
{
    list.clear();
    if (auto xml = juce::XmlDocument::parse(cacheFile()))
        for (auto* e : xml->getChildWithTagNameIterator("instrument"))
            if (juce::File(e->getStringAttribute("file")).exists())
                list.push_back({ e->getStringAttribute("name"), e->getStringAttribute("vendor"),
                                 e->getStringAttribute("file") });
}

void InstrumentCatalog::saveCache() const
{
    juce::XmlElement root("instruments");
    for (const auto& e : list)
    {
        auto* x = root.createNewChildElement("instrument");
        x->setAttribute("name", e.name);
        x->setAttribute("vendor", e.vendor);
        x->setAttribute("file", e.file);
    }
    const auto f = cacheFile();
    f.getParentDirectory().createDirectory();
    root.writeTo(f);
}

std::vector<InstrumentEntry> InstrumentCatalog::scanInstalled(const std::function<bool()>& shouldExit)
{
    juce::VST3PluginFormat format;
    std::vector<InstrumentEntry> out;

    for (const auto& dir : vst3Folders())
    {
        if (! dir.isDirectory())
            continue;
        // A .vst3 is a file or a bundle folder; either way it is one entry.
        juce::StringArray files = format.searchPathsForPlugins(juce::FileSearchPath(dir.getFullPathName()),
                                                               true, false);
        for (const auto& path : files)
        {
            if (shouldExit())
                return out;
            const juce::File file(path);
            if (file.getFileNameWithoutExtension().equalsIgnoreCase("Murmur Host")
                || file.getFileNameWithoutExtension().equalsIgnoreCase("Murmur"))
                continue; // hosting yourself is a hall of mirrors, not an instrument
            juce::OwnedArray<juce::PluginDescription> types;
            format.findAllTypesForFile(types, path);
            for (auto* d : types)
                if (d->isInstrument)
                {
                    out.push_back({ d->name, d->manufacturerName, path });
                    break;
                }
        }
    }

    std::sort(out.begin(), out.end(), [](const InstrumentEntry& a, const InstrumentEntry& b)
              { return a.name.compareIgnoreCase(b.name) < 0; });
    out.erase(std::unique(out.begin(), out.end(), [](const InstrumentEntry& a, const InstrumentEntry& b)
                          { return a.file == b.file; }),
              out.end());
    return out;
}

// ---- Scan thread -------------------------------------------------------------------------

class MurmurHostEditor::ScanThread : public juce::Thread
{
public:
    ScanThread(std::function<void(std::vector<InstrumentEntry>)> done)
        : juce::Thread("Murmur Host scan"), onDone(std::move(done)) {}
    ~ScanThread() override { stopThread(20000); }

    void run() override
    {
        auto result = InstrumentCatalog::scanInstalled([this] { return threadShouldExit(); });
        if (threadShouldExit())
            return;
        juce::MessageManager::callAsync([cb = onDone, r = std::move(result)]() mutable { cb(std::move(r)); });
    }

private:
    std::function<void(std::vector<InstrumentEntry>)> onDone;
};

// ---- MurmurHostEditor --------------------------------------------------------------------

MurmurHostEditor::MurmurHostEditor(MurmurHostProcessor& p)
    : juce::AudioProcessorEditor(p), host(p), murmurEditor(p, /*checkForUpdates*/ false)
{
    setLookAndFeel(&lnf);
    okstudio::ui::makeMouseOnly(*this);

    addAndMakeVisible(murmurEditor);
    murmurEditor.setResizable(false, false); // the window owns sizing
    murmurEditor.onViewChanged = [this] { fitWindow(); }; // Edit view asks for a taller editor

    styleLabel(instrumentLabel, "Instrument", 12.0f, okstudio::theme::textDim);
    styleLabel(status, "", 13.0f, okstudio::theme::textDim);
    styleLabel(mixLabel, "Mix", 13.0f, okstudio::theme::text, juce::Justification::centred);
    styleLabel(mixValue, "", 12.0f, okstudio::theme::textDim, juce::Justification::centred);
    for (auto* c : { &instrumentLabel, &status, &mixLabel, &mixValue })
        addAndMakeVisible(c);

    okstudio::ui::makeMouseOnly(picker);
    picker.onChange = [this] { pickerChanged(); };
    addAndMakeVisible(picker);

    for (auto* b : { &scanButton, &guiButton, &ejectButton })
    {
        okstudio::ui::makeMouseOnly(*b);
        addAndMakeVisible(b);
    }
    scanButton.onClick = [this] { startScan(); };
    ejectButton.onClick = [this] { host.ejectInstrument(); };
    guiButton.onClick = [this]
    {
        instShown = ! instShown;
        refreshStatus();
        fitWindow();
    };

    mixKnob.setRange(0.0, 1.0, 0.001);
    addAndMakeVisible(mixKnob);
    mixAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(host.apvts, "mix", mixKnob);
    mixKnob.onValueChange = [this]
    {
        mixValue.setText(mixKnob.getValue() < 0.001   ? "Voice"
                         : mixKnob.getValue() > 0.999 ? "Instrument"
                                                      : juce::String(juce::roundToInt(mixKnob.getValue() * 100.0)) + "%",
                         juce::dontSendNotification);
    };
    mixKnob.onValueChange();

    host.onInstrumentWillChange = [this] { closeInstrumentEditor(); };
    host.addChangeListener(this);

    rebuildPicker();
    openInstrumentEditor();
}

MurmurHostEditor::~MurmurHostEditor()
{
    scanThread = nullptr; // joins before anything it might call back into goes away
    host.onInstrumentWillChange = nullptr;
    host.removeChangeListener(this);
    closeInstrumentEditor();
    setLookAndFeel(nullptr);
}

void MurmurHostEditor::rebuildPicker()
{
    picker.clear(juce::dontSendNotification);
    const auto& list = catalog.entries();
    int selected = 0;
    for (size_t i = 0; i < list.size(); ++i)
    {
        picker.addItem(list[i].vendor.isNotEmpty() ? list[i].name + "  (" + list[i].vendor + ")" : list[i].name,
                       (int) i + 1);
        if (juce::File(list[i].file) == host.instrumentPath())
            selected = (int) i + 1;
    }
    if (list.empty())
        picker.setTextWhenNothingSelected("No instruments known: press Scan");
    else
        picker.setTextWhenNothingSelected("Choose an instrument");

    if (selected > 0)
        picker.setSelectedId(selected, juce::dontSendNotification);
    else if (host.hasInstrument()) // loaded from saved state, not (yet) in the scanned list
        picker.setText(host.instrumentName(), juce::dontSendNotification);
    else
        picker.setSelectedId(0, juce::dontSendNotification);
}

void MurmurHostEditor::pickerChanged()
{
    const int id = picker.getSelectedId();
    const auto& list = catalog.entries();
    if (id < 1 || (size_t) id > list.size())
        return;
    host.loadInstrument(juce::File(list[(size_t) id - 1].file));
    refreshStatus();
}

void MurmurHostEditor::startScan()
{
    if (scanning)
        return;
    scanning = true;
    scanButton.setEnabled(false);
    scanButton.setButtonText("Scanning...");
    juce::Component::SafePointer<MurmurHostEditor> safe(this);
    scanThread = std::make_unique<ScanThread>([safe](std::vector<InstrumentEntry> found)
    {
        if (auto* e = safe.getComponent())
        {
            e->scanning = false;
            e->catalog.setEntries(std::move(found));
            e->catalog.saveCache();
            e->scanButton.setEnabled(true);
            e->scanButton.setButtonText("Scan");
            e->rebuildPicker();
            e->refreshStatus();
        }
    });
    scanThread->startThread();
}

int MurmurHostEditor::instrumentEditorHeight() const
{
    return (instEditor != nullptr && instShown) ? instEditor->getHeight() : 0;
}

void MurmurHostEditor::openInstrumentEditor()
{
    closeInstrumentEditor();
    if (auto* inst = host.instrumentInstance())
    {
        if (auto* ed = inst->createEditorIfNeeded())
            instEditor.reset(ed);
        else
            instEditor = std::make_unique<juce::GenericAudioProcessorEditor>(*inst);
        instEditor->addComponentListener(this);
        addAndMakeVisible(*instEditor);
        instEditor->setVisible(instShown);
    }
    rebuildPicker();
    refreshStatus();
    fitWindow();
}

void MurmurHostEditor::closeInstrumentEditor()
{
    if (instEditor == nullptr)
        return;
    instEditor->removeComponentListener(this);
    removeChildComponent(instEditor.get());
    if (auto* inst = host.instrumentInstance())
        inst->editorBeingDeleted(instEditor.get()); // no-op for the generic fallback
    instEditor = nullptr;
}

void MurmurHostEditor::refreshStatus()
{
    guiButton.setButtonText(instShown ? "Hide GUI" : "Show GUI");
    guiButton.setEnabled(host.hasInstrument());
    ejectButton.setEnabled(host.hasInstrument());

    if (host.lastError().isNotEmpty())
    {
        // "Instrument not found: <path>" and friends; audio keeps passing meanwhile.
        status.setColour(juce::Label::textColourId, okstudio::theme::warn);
        status.setText(host.lastError().startsWith("Instrument not found") ? "Instrument not found, passing audio through"
                                                                           : host.lastError(),
                       juce::dontSendNotification);
        status.setTooltip(host.lastError());
    }
    else
    {
        status.setColour(juce::Label::textColourId, okstudio::theme::textDim);
        const bool standaloneApp = host.wrapperType == juce::AudioProcessor::wrapperType_Standalone;
        status.setText(host.hasInstrument() ? "Playing " + host.instrumentName()
                       : standaloneApp      ? "No instrument: silent until you Scan and pick one"
                                            : "No instrument: the voice passes through",
                       juce::dontSendNotification);
        status.setTooltip({});
    }
}

void MurmurHostEditor::changeListenerCallback(juce::ChangeBroadcaster*)
{
    openInstrumentEditor();
}

void MurmurHostEditor::componentMovedOrResized(juce::Component&, bool, bool wasResized)
{
    if (wasResized)
        fitWindow(); // the instrument resized its own GUI
}

void MurmurHostEditor::fitWindow()
{
    const int w = juce::jmax(MurmurEditor::defaultWidth,
                             instEditor != nullptr && instShown ? instEditor->getWidth() : 0);
    const int h = murmurEditor.preferredHeight() + stripH + instrumentEditorHeight();
    if (instEditor != nullptr)
        instEditor->setVisible(instShown);
    if (w != getWidth() || h != getHeight())
        setSize(w, h);
    else
        resized();
}

void MurmurHostEditor::paint(juce::Graphics& g)
{
    g.fillAll(okstudio::theme::background);
    g.setColour(okstudio::theme::panel);
    g.fillRect(juce::Rectangle<int>(0, murmurEditor.preferredHeight(), getWidth(), stripH));
}

void MurmurHostEditor::resized()
{
    auto area = getLocalBounds();
    murmurEditor.setBounds(area.removeFromTop(murmurEditor.preferredHeight()).withWidth(MurmurEditor::defaultWidth));

    auto strip = area.removeFromTop(stripH).reduced(16, 8);
    auto mix = strip.removeFromRight(110);
    mixLabel.setBounds(mix.removeFromTop(18));
    mixValue.setBounds(mix.removeFromBottom(16));
    mixKnob.setBounds(mix.withSizeKeepingCentre(juce::jmin(mix.getWidth(), mix.getHeight()),
                                                 juce::jmin(mix.getWidth(), mix.getHeight())));
    strip.removeFromRight(12);

    instrumentLabel.setBounds(strip.removeFromTop(18));
    auto row = strip.removeFromTop(hitH);
    ejectButton.setBounds(row.removeFromRight(80));
    row.removeFromRight(8);
    guiButton.setBounds(row.removeFromRight(100));
    row.removeFromRight(8);
    scanButton.setBounds(row.removeFromRight(100));
    row.removeFromRight(8);
    picker.setBounds(row);
    status.setBounds(strip.removeFromTop(22));

    if (instEditor != nullptr && instShown)
        instEditor->setTopLeftPosition((getWidth() - instEditor->getWidth()) / 2, area.getY());
}
} // namespace murmur
