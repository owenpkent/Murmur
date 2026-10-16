// Murmur Host: the processor with no instrument loaded. State round trip (including the
// host-only Mix parameter and a saved instrument that is not on disk), Mix default and range,
// and audio passing through untouched when nothing is hosted. A real hosted instrument is not
// loaded here: that needs a third-party VST3 on the machine.
// Self-registering statics only; tests/DspTests.cpp owns the runner's main().

#include "../src/host/MurmurHostProcessor.h"
#include <juce_events/juce_events.h>

namespace murmur::tests
{
namespace
{
    struct HostFixture
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        MurmurHostProcessor processor;
    };

    float mixOf(MurmurHostProcessor& p) { return p.apvts.getRawParameterValue("mix")->load(); }

    void setMix(MurmurHostProcessor& p, float v)
    {
        if (auto* param = p.apvts.getParameter("mix"))
            param->setValueNotifyingHost(param->convertTo0to1(v));
    }

    juce::MemoryBlock save(MurmurHostProcessor& p)
    {
        juce::MemoryBlock block;
        p.getStateInformation(block);
        return block;
    }
} // namespace

struct HostTests : public juce::UnitTest
{
    HostTests() : juce::UnitTest("MurmurHost", "Murmur") {}

    void runTest() override
    {
        beginTest("Mix defaults to 1 (instrument only) and is host-only");
        {
            HostFixture f;
            expectEquals(mixOf(f.processor), 1.0f);
            expect(f.processor.getName() == "Murmur Host");
            expect(! f.processor.hasInstrument());

            MurmurProcessor plain;
            expect(plain.apvts.getParameter("mix") == nullptr, "plain Murmur has no Mix parameter");
        }

        beginTest("Mix round-trips through state with no instrument loaded");
        {
            HostFixture f;
            setMix(f.processor, 0.25f);
            f.processor.apvts.getParameter("sensitivity")->setValueNotifyingHost(0.8f);
            const auto block = save(f.processor);

            HostFixture g;
            expectEquals(mixOf(g.processor), 1.0f);
            g.processor.setStateInformation(block.getData(), (int) block.getSize());
            expectWithinAbsoluteError(mixOf(g.processor), 0.25f, 0.002f);
            expectWithinAbsoluteError(g.processor.apvts.getRawParameterValue("sensitivity")->load(), 0.8f, 0.002f);
            expect(! g.processor.hasInstrument());
            expect(g.processor.lastError().isEmpty());
        }

        beginTest("A saved instrument that is missing is reported and survives a re-save");
        {
            HostFixture f;
            // Save, then edit the saved tree's hosted child to name an instrument that is not there.
            const auto base = save(f.processor);
            juce::ValueTree root;
            if (auto xml = juce::AudioProcessor::getXmlFromBinary(base.getData(), (int) base.getSize()))
                root = juce::ValueTree::fromXml(*xml);
            expect(root.isValid(), "state envelope should decode as XML");

            if (root.isValid())
            {
                auto hosted = root.getChildWithName("hostedInstrument");
                if (! hosted.isValid())
                {
                    hosted = juce::ValueTree("hostedInstrument");
                    root.appendChild(hosted, nullptr);
                }
                hosted.setProperty("file", "C:\\definitely\\not\\here\\Ghost.vst3", nullptr);
                hosted.setProperty("state", "AAAA", nullptr);

                juce::MemoryBlock edited;
                juce::AudioProcessor::copyXmlToBinary(*root.createXml(), edited);

                HostFixture g;
                g.processor.setStateInformation(edited.getData(), (int) edited.getSize());
                expect(! g.processor.hasInstrument());
                expect(g.processor.lastError().startsWith("Instrument not found"),
                       "got: " + g.processor.lastError());

                // Saving again keeps the path, so the project does not lose the instrument.
                const auto again = save(g.processor);
                HostFixture h;
                h.processor.setStateInformation(again.getData(), (int) again.getSize());
                expect(h.processor.lastError().startsWith("Instrument not found"),
                       "re-saved state lost the missing instrument: " + h.processor.lastError());
            }
        }

        beginTest("With no instrument the voice passes through untouched");
        {
            HostFixture f;
            f.processor.setPlayConfigDetails(2, 2, 48000.0, 256);
            f.processor.prepareToPlay(48000.0, 256);

            juce::AudioBuffer<float> buffer(2, 256);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                    buffer.setSample(ch, i, 0.1f * std::sin(0.05f * (float) i));
            juce::AudioBuffer<float> original;
            original.makeCopyOf(buffer);

            juce::MidiBuffer midi;
            f.processor.processBlock(buffer, midi);

            bool same = true;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                    same = same && buffer.getSample(ch, i) == original.getSample(ch, i);
            expect(same, "audio must pass through unchanged when nothing is hosted");
        }

        beginTest("Reported latency is the engine's when nothing is hosted");
        {
            HostFixture f;
            f.processor.setPlayConfigDetails(2, 2, 48000.0, 256);
            f.processor.prepareToPlay(48000.0, 256);
            expectEquals(f.processor.getLatencySamples(), f.processor.getEngine().getLatencySamples());
        }
    }
};

static HostTests hostTests;
} // namespace murmur::tests
