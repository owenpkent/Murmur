// Processor state: parameters survive a save/load round trip, and the calibration tree
// rides along. These need a real MurmurProcessor (APVTS + setStateInformation), so
// Murmur_tests links the plugin target. tests/DspTests.cpp owns the runner's main();
// this file is self-registering statics only.

#include "../src/PluginProcessor.h"
#include <juce_events/juce_events.h>

namespace murmur::tests
{
namespace
{
    // A processor plus the message loop JUCE expects around one.
    struct Host
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        MurmurProcessor processor;
    };

    float paramOf(MurmurProcessor& p, const juce::String& id)
    {
        auto* raw = p.apvts.getRawParameterValue(id);
        return raw != nullptr ? raw->load() : std::numeric_limits<float>::quiet_NaN();
    }

    // Sets a parameter by its real (denormalised) value.
    void setParam(MurmurProcessor& p, const juce::String& id, float value)
    {
        if (auto* param = p.apvts.getParameter(id))
            param->setValueNotifyingHost(param->convertTo0to1(value));
    }

    juce::MemoryBlock save(MurmurProcessor& p)
    {
        juce::MemoryBlock block;
        p.getStateInformation(block);
        return block;
    }
} // namespace

class StateTests : public juce::UnitTest
{
public:
    StateTests() : juce::UnitTest("Murmur processor state", "murmur") {}

    void runTest() override
    {
        beginTest("it is an audio effect that takes audio in and MIDI both ways");
        {
            Host h;
            expect(! h.processor.isMidiEffect(), "not a MIDI effect");
            expect(h.processor.acceptsMidi(), "accepts MIDI (Live needs the input bus)");
            expect(h.processor.producesMidi(), "produces MIDI");
            expectEquals(h.processor.getTotalNumInputChannels(), 2);
            expectEquals(h.processor.getTotalNumOutputChannels(), 2);
        }

        beginTest("every parameter survives a state round trip");
        {
            Host a;
            setParam(a.processor, "mode", 1.0f);
            setParam(a.processor, "sensitivity", 0.25f);
            setParam(a.processor, "quantize", 0.9f);
            setParam(a.processor, "swing", 0.4f);
            setParam(a.processor, "grid", 4.0f);
            setParam(a.processor, "root", 7.0f);
            setParam(a.processor, "scale", 2.0f);
            setParam(a.processor, "scaleLock", 0.0f);
            const auto block = save(a.processor);

            Host b; // a fresh instance at defaults
            expect(paramOf(b.processor, "mode") == 0.0f, "starts in Melody");
            b.processor.setStateInformation(block.getData(), (int) block.getSize());

            expectEquals(paramOf(b.processor, "mode"), 1.0f);
            expectWithinAbsoluteError(paramOf(b.processor, "sensitivity"), 0.25f, 0.002f);
            expectWithinAbsoluteError(paramOf(b.processor, "quantize"), 0.9f, 0.002f);
            expectWithinAbsoluteError(paramOf(b.processor, "swing"), 0.4f, 0.002f);
            expectEquals(paramOf(b.processor, "grid"), 4.0f);
            expectEquals(paramOf(b.processor, "root"), 7.0f);
            expectEquals(paramOf(b.processor, "scale"), 2.0f);
            expectEquals(paramOf(b.processor, "scaleLock"), 0.0f);
        }

        beginTest("defaults are musical: Melody, scale lock on, 1/16 grid");
        {
            Host h;
            expectEquals(paramOf(h.processor, "mode"), 0.0f);
            expectEquals(paramOf(h.processor, "grid"), 2.0f);
            expectEquals(paramOf(h.processor, "scaleLock"), 1.0f);
        }

        beginTest("state carries a calibration wrapper, and a second round trip is stable");
        {
            Host a;
            const auto block = save(a.processor);
            auto xml = juce::AudioProcessor::getXmlFromBinary(block.getData(), (int) block.getSize());
            expect(xml != nullptr, "state is valid XML");
            const auto root = juce::ValueTree::fromXml(*xml);
            expect(root.getChildWithName("PARAMS").isValid(), "parameters are saved");
            expect(root.getChildWithName("MURMUR_CALIBRATION").isValid(), "calibration wrapper is saved");

            Host b;
            b.processor.setStateInformation(block.getData(), (int) block.getSize());
            const auto again = save(b.processor);
            Host c;
            c.processor.setStateInformation(again.getData(), (int) again.getSize());
            expectEquals(paramOf(c.processor, "grid"), paramOf(a.processor, "grid"));
        }

        beginTest("garbage state is ignored without crashing");
        {
            Host h;
            const char junk[] = "not a state blob";
            h.processor.setStateInformation(junk, (int) sizeof(junk));
            expectEquals(paramOf(h.processor, "mode"), 0.0f);
        }
    }
};

static StateTests stateTests;
} // namespace murmur::tests
