#include "MurmurMcp.h"
#include "../PluginProcessor.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <utility>

namespace murmur
{
namespace
{
    using Engine = dsp::MurmurEngine;

    const char* const drumClassNames[] = { "kick", "snare", "hatClosed", "hatOpen" };
    constexpr int numDrumClasses = 4;

    // Parses the `drumClass` argument (a name from drumClassNames, or the index 0..3).
    // Rejects anything else: a typo that fell back to Kick would calibrate the wrong sound
    // and report success.
    bool readDrumClass(const juce::var& args, int& cls, juce::String& error)
    {
        const auto v = args.getProperty("drumClass", juce::var());
        if (v.isString())
        {
            for (int i = 0; i < numDrumClasses; ++i)
                if (v.toString().equalsIgnoreCase(drumClassNames[i]))
                {
                    cls = i;
                    return true;
                }
        }
        else if (v.isInt() || v.isInt64() || v.isDouble())
        {
            cls = (int) v;
            if (cls >= 0 && cls < numDrumClasses)
                return true;
        }
        error = "drumClass must be one of kick, snare, hatClosed, hatOpen (or 0..3)";
        return false;
    }

    juce::var noteOrNull(int note)
    {
        if (note < 0)
            return {};
        auto* o = new juce::DynamicObject();
        o->setProperty("note", note);
        o->setProperty("name", juce::MidiMessage::getMidiNoteName(note, true, true, 3));
        return juce::var(o);
    }

    juce::var paramText(MurmurProcessor& p, const char* id)
    {
        auto* param = p.apvts.getParameter(id);
        return param != nullptr ? juce::var(param->getCurrentValueAsText()) : juce::var();
    }

    juce::var calibrationSnapshot(MurmurProcessor& p)
    {
        auto& engine = p.getEngine();
        juce::Array<juce::var> classes;
        for (int i = 0; i < numDrumClasses; ++i)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("drumClass", drumClassNames[i]);
            o->setProperty("midiNote", engine.drumNotes()[(size_t) i]);
            const int n = engine.getNumCalibrationExamples((Engine::DrumClass) i);
            o->setProperty("examples", n);
            // Zero examples means the class still answers from the built-in defaults.
            o->setProperty("calibrated", n > 0);
            classes.add(juce::var(o));
        }
        auto* o = new juce::DynamicObject();
        o->setProperty("calibrating", engine.isCalibrating());
        const int cc = engine.getCalibratingClass();
        o->setProperty("calibratingClass", cc >= 0 ? juce::var(drumClassNames[cc]) : juce::var());
        o->setProperty("classes", classes);
        return juce::var(o);
    }
} // namespace

MurmurMcp::MurmurMcp(MurmurProcessor& p)
    : processor(p), server("murmur", JucePlugin_VersionString)
{
    for (auto& t : buildTools())
        server.addTool(std::move(t));
    server.start();
}

MurmurMcp::~MurmurMcp()
{
    server.stop(); // message thread; no request can be mid-flight into `processor` after this
}

std::vector<okstudio::mcp::Tool> MurmurMcp::buildTools()
{
    std::vector<okstudio::mcp::Tool> tools;
    tools.push_back(toolGetState());
    tools.push_back(toolGetTransport());
    tools.push_back(toolListParams());
    tools.push_back(toolSetParams());
    tools.push_back(toolGetCalibration());
    tools.push_back(toolStartCalibration());
    tools.push_back(toolClearCalibration());
    return tools;
}

okstudio::mcp::Tool MurmurMcp::toolGetState()
{
    okstudio::mcp::Tool t;
    t.name = "get_state";
    t.description = "Snapshot of Murmur's current state: product and version, the mode "
                     "(Melody or Drums), every control's current value as text, the live "
                     "voice analysis (inputLevel 0..1, lastNote: the last note or drum hit "
                     "emitted, or null before the first), the free tempo Murmur has inferred "
                     "from your timing (inferredBpm, 0 while it is still listening, with its "
                     "confidence), the host transport, and the drum calibration. Call this "
                     "first to orient before changing anything.";
    t.run = [this](const juce::var&, juce::String&) -> juce::var
    {
        auto& engine = processor.getEngine();
        auto* o = new juce::DynamicObject();
        o->setProperty("product", "Murmur");
        o->setProperty("version", JucePlugin_VersionString);
        o->setProperty("mode", paramText(processor, "mode"));

        auto* controls = new juce::DynamicObject();
        for (const char* id : { "sensitivity", "quantize", "swing", "grid", "root", "scale", "scaleLock" })
            controls->setProperty(id, paramText(processor, id));
        o->setProperty("controls", juce::var(controls));

        o->setProperty("inputLevel", (double) engine.getInputLevel());
        o->setProperty("lastNote", noteOrNull(engine.getLastNote()));
        o->setProperty("inferredBpm", engine.getInferredBpm());
        o->setProperty("inferredBpmConfidence", (double) engine.getInferredBpmConfidence());
        o->setProperty("hostPlaying", processor.isHostPlaying());
        o->setProperty("hostBpm", processor.getHostBpm());
        o->setProperty("latencySamples", engine.getLatencySamples());
        o->setProperty("calibration", calibrationSnapshot(processor));
        return juce::var(o);
    };
    return t;
}

okstudio::mcp::Tool MurmurMcp::toolGetTransport()
{
    okstudio::mcp::Tool t;
    t.name = "get_transport";
    t.description = "Which clock Murmur is quantizing to right now. While the host transport "
                     "plays, the grid follows the host tempo (hostBpm). While it is stopped "
                     "there is no tempo to follow, so Murmur infers a free tempo from the "
                     "inter-onset intervals of what you sing (inferredBpm, 0 while it is still "
                     "listening). `source` is \"host\" or \"inferred\" accordingly, and `bpm` "
                     "is the tempo the grid is using (null while an inferred tempo is not "
                     "available yet).";
    t.run = [this](const juce::var&, juce::String&) -> juce::var
    {
        auto& engine = processor.getEngine();
        const bool playing = processor.isHostPlaying();
        const double inferred = engine.getInferredBpm();
        auto* o = new juce::DynamicObject();
        o->setProperty("hostPlaying", playing);
        o->setProperty("hostBpm", processor.getHostBpm());
        o->setProperty("inferredBpm", inferred);
        o->setProperty("inferredBpmConfidence", (double) engine.getInferredBpmConfidence());
        o->setProperty("source", playing ? "host" : "inferred");
        o->setProperty("bpm", playing ? juce::var(processor.getHostBpm())
                                      : (inferred > 0.0 ? juce::var(inferred) : juce::var()));
        o->setProperty("grid", paramText(processor, "grid"));
        return juce::var(o);
    };
    return t;
}

okstudio::mcp::Tool MurmurMcp::toolListParams()
{
    okstudio::mcp::Tool t;
    t.name = "list_params";
    t.description = "Every automatable parameter Murmur exposes: its id, display name, "
                     "current value as text, and either its choice list (combo-box "
                     "params) or its numeric min/max (everything else). Use this to see "
                     "what set_params can change and what values are legal before calling it.";
    t.run = [this](const juce::var&, juce::String&) -> juce::var
    {
        juce::Array<juce::var> list;
        for (auto* param : processor.getParameters())
        {
            auto* rap = dynamic_cast<juce::RangedAudioParameter*>(param);
            if (rap == nullptr)
                continue;
            auto* obj = new juce::DynamicObject();
            obj->setProperty("id", rap->getParameterID());
            obj->setProperty("name", rap->getName(100));
            obj->setProperty("value", rap->getCurrentValueAsText());
            if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(param))
            {
                juce::Array<juce::var> choices;
                for (auto& c : choice->choices)
                    choices.add(c);
                obj->setProperty("choices", choices);
            }
            else
            {
                const auto range = rap->getNormalisableRange();
                obj->setProperty("min", range.start);
                obj->setProperty("max", range.end);
            }
            list.add(juce::var(obj));
        }
        return juce::var(list);
    };
    return t;
}

okstudio::mcp::Tool MurmurMcp::toolSetParams()
{
    okstudio::mcp::Tool t;
    t.name = "set_params";
    t.description = "Set one or more parameters at once, by id (see list_params). A "
                     "string value is parsed the way typing it into the control would "
                     "(e.g. a scale name); a number is the parameter's real-world value, "
                     "not normalized; true/false sets a toggle. Every id is validated "
                     "before anything changes: if any id is unknown, the whole call "
                     "fails and nothing is applied. Returns each parameter's new text value.";
    t.params = { { "values", "object", "Map of parameter id -> new value (string, number, or boolean).", true } };
    t.run = [this](const juce::var& args, juce::String& error) -> juce::var
    {
        auto* values = args.getProperty("values", juce::var()).getDynamicObject();
        if (values == nullptr)
        {
            error = "values must be an object of paramID -> value";
            return {};
        }

        // Validate every id before touching anything.
        juce::StringArray unknown;
        for (const auto& prop : values->getProperties())
            if (processor.apvts.getParameter(prop.name.toString()) == nullptr)
                unknown.add(prop.name.toString());
        if (! unknown.isEmpty())
        {
            error = "unknown parameter id(s): " + unknown.joinIntoString(", ");
            return {};
        }

        auto* result = new juce::DynamicObject();
        for (const auto& prop : values->getProperties())
        {
            auto* param = processor.apvts.getParameter(prop.name.toString());
            const juce::var& v = prop.value;
            float normalized;
            if (v.isBool())
                normalized = ((bool) v) ? 1.0f : 0.0f;
            else if (v.isString())
                normalized = param->getValueForText(v.toString());
            else
                normalized = param->convertTo0to1((float) v);

            param->beginChangeGesture();
            param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, normalized));
            param->endChangeGesture();
            result->setProperty(prop.name, param->getCurrentValueAsText());
        }
        return juce::var(result);
    };
    return t;
}

okstudio::mcp::Tool MurmurMcp::toolGetCalibration()
{
    okstudio::mcp::Tool t;
    t.name = "get_calibration";
    t.description = "The drum calibration: for each class (kick, snare, hatClosed, hatOpen) its "
                     "output MIDI note, how many of your own examples it holds, and whether it "
                     "is calibrated. A class with zero examples still classifies, on built-in "
                     "defaults that will not suit most voices. `calibrating` is true while "
                     "Murmur is waiting for the next hits, and `calibratingClass` names the class.";
    t.run = [this](const juce::var&, juce::String&) -> juce::var { return calibrationSnapshot(processor); };
    return t;
}

okstudio::mcp::Tool MurmurMcp::toolStartCalibration()
{
    okstudio::mcp::Tool t;
    t.name = "start_calibration";
    t.description = "Start calibrating one drum class, exactly as the Calibrate button does: the "
                     "next three onsets Murmur hears become examples of that class, then "
                     "calibration ends. This only arms it; the sound has to come from whoever "
                     "is at the microphone. Poll get_calibration to see it finish.";
    t.params = { { "drumClass", "string", "kick, snare, hatClosed or hatOpen (or the index 0..3).", true } };
    t.run = [this](const juce::var& args, juce::String& error) -> juce::var
    {
        int cls = 0;
        if (! readDrumClass(args, cls, error))
            return {};
        processor.getEngine().beginCalibration((Engine::DrumClass) cls);
        return calibrationSnapshot(processor);
    };
    return t;
}

okstudio::mcp::Tool MurmurMcp::toolClearCalibration()
{
    okstudio::mcp::Tool t;
    t.name = "clear_calibration";
    t.description = "Throw away your own examples for one drum class, exactly as its Clear "
                     "button does. The class falls back to the built-in defaults.";
    t.params = { { "drumClass", "string", "kick, snare, hatClosed or hatOpen (or the index 0..3).", true } };
    t.run = [this](const juce::var& args, juce::String& error) -> juce::var
    {
        int cls = 0;
        if (! readDrumClass(args, cls, error))
            return {};
        processor.getEngine().clearCalibration((Engine::DrumClass) cls);
        return calibrationSnapshot(processor);
    };
    return t;
}
} // namespace murmur
