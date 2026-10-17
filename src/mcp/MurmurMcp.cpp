#include "MurmurMcp.h"
#include "../PluginProcessor.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
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

    juce::String noteName(int midi) { return juce::MidiMessage::getMidiNoteName(midi, true, true, 3); }

    // A whole number in [lo, hi] from `args[name]`. A missing, non-numeric, fractional or
    // out-of-range value is an error, never a clamp (a bad index must not edit a neighbour).
    bool readInt(const juce::var& args, const char* name, int lo, int hi, int& out, juce::String& error)
    {
        const auto v = args.getProperty(name, juce::var());
        if (v.isInt() || v.isInt64() || (v.isDouble() && (double) v == std::floor((double) v)))
        {
            const auto n = (juce::int64) v;
            if (n >= lo && n <= hi)
            {
                out = (int) n;
                return true;
            }
        }
        error = juce::String(name) + " must be a whole number from " + juce::String(lo) + " to " + juce::String(hi);
        return false;
    }

    juce::var takeNoteJson(int index, const take::TakeNote& n)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("index", index);
        o->setProperty("start", n.start);
        o->setProperty("end", n.end);
        o->setProperty("sungMidi", (double) n.sungMidi);
        o->setProperty("sungName", noteName((int) std::lround(n.sungMidi)));
        o->setProperty("detectedNote", n.detectedNote);
        o->setProperty("detectedName", noteName(n.detectedNote));
        o->setProperty("targetNote", n.targetNote);
        o->setProperty("targetName", noteName(n.targetNote));
        o->setProperty("velocity", (double) n.velocity);
        o->setProperty("drift", (double) n.drift);
        return juce::var(o);
    }

    juce::var takeStateJson(MurmurProcessor& p)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("recording", p.getCapture().isRecording());
        o->setProperty("secondsRecorded", p.getCapture().isRecording() ? p.getCapture().secondsRecorded() : 0.0);
        o->setProperty("analysing", p.isAnalysingTake());
        o->setProperty("playing", p.isTakePlaying());
        o->setProperty("playheadSeconds", p.isTakePlaying() ? p.getTakePlayheadSeconds() : 0.0);
        o->setProperty("noteCount", (int) p.getTake().notes.size());
        return juce::var(o);
    }

    double takeLength(const take::Take& t)
    {
        double d = 0.0;
        for (const auto& n : t.notes)
            d = std::max(d, n.end);
        return d;
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
    tools.push_back(toolGetTake());
    tools.push_back(toolEditTake());
    tools.push_back(toolTakeTransport());
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
        o->setProperty("product", processor.productName());
        o->setProperty("version", JucePlugin_VersionString);
        o->setProperty("mode", paramText(processor, "mode"));

        auto* controls = new juce::DynamicObject();
        for (const char* id : { "sensitivity", "quantize", "swing", "grid", "root", "scale", "scaleLock" })
            controls->setProperty(id, paramText(processor, id));
        if (processor.apvts.getParameter("mix") != nullptr) // Murmur Host only
            controls->setProperty("mix", paramText(processor, "mix"));
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

okstudio::mcp::Tool MurmurMcp::toolGetTake()
{
    okstudio::mcp::Tool t;
    t.name = "get_take";
    t.description = "The Take Editor's take: `hasTake`, the state flags (`recording` with "
                     "`secondsRecorded`, `analysing`, `playing` with `playheadSeconds`), `error` "
                     "(text, or null), the `anchor` ({ppq, bpm} when the take was recorded while "
                     "the host played, else null for a free take) and `notes`: for each note its "
                     "`index`, `start` and `end` (seconds from the take start), `sungMidi` (what "
                     "was sung, fractional), `detectedNote`, `targetNote` (what it plays as, the "
                     "only thing edit_take changes), `velocity` (0..1), `drift` (1 keeps the "
                     "sung wobble, 0 flattens it to the note), plus the note names. Also "
                     "`hasAudio` (false when the take's WAV is missing: MIDI plays, the voice "
                     "does not) and `renderingVoice` (the corrected voice is being re-rendered). "
                     "Use the `index` with edit_take.";
    t.run = [this](const juce::var&, juce::String&) -> juce::var
    {
        const auto& take = processor.getTake();
        auto* o = new juce::DynamicObject();
        o->setProperty("hasTake", ! take.notes.empty() || take.audioPath.isNotEmpty());
        const auto state = takeStateJson(processor);
        for (const char* k : { "recording", "secondsRecorded", "analysing", "playing", "playheadSeconds", "noteCount" })
            o->setProperty(k, state[k]);
        const auto err = processor.getTakeError();
        o->setProperty("error", err.isEmpty() ? juce::var() : juce::var(err));
        o->setProperty("hasAudio", processor.hasTakeAudio());
        o->setProperty("renderingVoice", processor.isRenderingVoice());
        if (take.anchorPpq.has_value())
        {
            auto* a = new juce::DynamicObject();
            a->setProperty("ppq", *take.anchorPpq);
            a->setProperty("bpm", take.anchorBpm);
            o->setProperty("anchor", juce::var(a));
        }
        else
        {
            o->setProperty("anchor", juce::var());
        }
        juce::Array<juce::var> notes;
        for (size_t i = 0; i < take.notes.size(); ++i)
            notes.add(takeNoteJson((int) i, take.notes[i]));
        o->setProperty("notes", notes);
        return juce::var(o);
    };
    return t;
}

okstudio::mcp::Tool MurmurMcp::toolEditTake()
{
    okstudio::mcp::Tool t;
    t.name = "edit_take";
    t.description = "Edit the take's notes, exactly as the editor does; each call is one undo "
                     "step. `op` is one of: shift (move note `index` by `steps` semitones, or by "
                     "scale degrees while the scaleLock control is on, as dragging on the roll "
                     "does), set (play note `index` as MIDI note `note`, 0..127), reset (back to "
                     "the detected note, drift back to 1), drift (set note `index`'s `drift`, 0..1: "
                     "1 keeps the sung wobble, 0 pins the note to its centre), delete (remove "
                     "note `index`), snap_to_scale (every note to the Root and Scale controls) "
                     "and undo (the last edit). An unknown op, a missing or out-of-range index, "
                     "a missing `steps`, an out-of-range `note`, a missing or out-of-range "
                     "`drift`, or editing while recording or analysing is an error and changes "
                     "nothing. Returns the updated note (shift, set, reset, drift), or the "
                     "remaining noteCount (delete, snap_to_scale, undo). Edits re-render the "
                     "voice in the background (get_take `renderingVoice`).";
    t.params = { { "op", "string", "shift, set, reset, drift, delete, snap_to_scale or undo.", true },
                 { "index", "integer", "Note index from get_take (shift, set, reset, drift, delete).", false },
                 { "steps", "integer", "Semitones, or scale degrees with scaleLock on; negative is down (shift).", false },
                 { "note", "integer", "MIDI note number 0..127 (set).", false },
                 { "drift", "number", "0..1: 1 keeps the sung wobble, 0 flattens it (drift).", false } };
    t.run = [this](const juce::var& args, juce::String& error) -> juce::var
    {
        const auto opVar = args.getProperty("op", juce::var());
        const auto op = opVar.isString() ? opVar.toString() : juce::String();
        const bool needsIndex = op == "shift" || op == "set" || op == "reset" || op == "drift" || op == "delete";
        if (! needsIndex && op != "snap_to_scale" && op != "undo")
        {
            error = "op must be one of shift, set, reset, drift, delete, snap_to_scale, undo";
            return {};
        }
        if (processor.getCapture().isRecording() || processor.isAnalysingTake())
        {
            error = "the take cannot be edited while it is recording or being analysed";
            return {};
        }

        // Validate every argument before anything changes.
        const int count = (int) processor.getTake().notes.size();
        int index = 0, steps = 0, note = 0;
        double drift = 1.0;
        if (needsIndex && ! readInt(args, "index", 0, count - 1, index, error))
        {
            error += " (the take has " + juce::String(count) + (count == 1 ? " note)" : " notes)");
            return {};
        }
        if (op == "shift" && ! readInt(args, "steps", -127, 127, steps, error))
            return {};
        if (op == "set" && ! readInt(args, "note", 0, 127, note, error))
            return {};
        if (op == "drift")
        {
            const auto v = args.getProperty("drift", juce::var());
            if (! (v.isInt() || v.isInt64() || v.isDouble()) || ! ((double) v >= 0.0 && (double) v <= 1.0))
            {
                error = "drift must be a number from 0 to 1";
                return {};
            }
            drift = (double) v;
        }
        if (op == "undo" && ! processor.canUndoTake())
        {
            error = "nothing to undo";
            return {};
        }

        const int root = (int) processor.apvts.getRawParameterValue("root")->load();
        const int scale = (int) processor.apvts.getRawParameterValue("scale")->load();
        const bool lock = processor.apvts.getRawParameterValue("scaleLock")->load() > 0.5f;

        if (op == "shift")
            processor.editTake([=](take::Take& tk) { take::shiftNote(tk, index, steps, lock, root, scale); });
        else if (op == "set")
            processor.editTake([=](take::Take& tk) { tk.notes[(size_t) index].targetNote = note; });
        else if (op == "reset")
            processor.editTake([=](take::Take& tk) { take::resetNote(tk, index); });
        else if (op == "drift")
            processor.editTake([=](take::Take& tk) { tk.notes[(size_t) index].drift = (float) drift; });
        else if (op == "delete")
            processor.editTake([=](take::Take& tk) { take::deleteNote(tk, index); });
        else if (op == "snap_to_scale")
            processor.editTake([=](take::Take& tk) { take::snapAllToScale(tk, root, scale); });
        else
            processor.undoTake();

        if (needsIndex && op != "delete")
            return takeNoteJson(index, processor.getTake().notes[(size_t) index]);
        auto* o = new juce::DynamicObject();
        o->setProperty("op", op);
        o->setProperty("noteCount", (int) processor.getTake().notes.size());
        return juce::var(o);
    };
    return t;
}

okstudio::mcp::Tool MurmurMcp::toolTakeTransport()
{
    okstudio::mcp::Tool t;
    t.name = "take_transport";
    t.description = "Record, stop and play the take, like the editor's Record and Play buttons. "
                     "`action` is record (start capturing the audio input; the sound has to come "
                     "from whoever is at the microphone), stop_recording (ends the capture and "
                     "waits for the analysis, up to 30 seconds, then returns the noteCount), play "
                     "(the take's notes as MIDI, from `fromSeconds`, default 0) or stop. An action "
                     "that is not possible now (record while recording or analysing, "
                     "stop_recording when not recording, play with no notes or while recording, "
                     "play while playing, stop when not playing, a fromSeconds past the end) is "
                     "an error. Returns the transport state: recording, secondsRecorded, "
                     "analysing, playing, playheadSeconds, noteCount.";
    t.params = { { "action", "string", "record, stop_recording, play or stop.", true },
                 { "fromSeconds", "number", "Where play starts, in seconds from the take start (default 0).", false } };
    t.run = [this](const juce::var& args, juce::String& error) -> juce::var
    {
        const auto actionVar = args.getProperty("action", juce::var());
        const auto action = actionVar.isString() ? actionVar.toString() : juce::String();
        const bool recording = processor.getCapture().isRecording();
        const auto& take = processor.getTake();

        if (action == "record")
        {
            if (recording || processor.isAnalysingTake())
                error = recording ? "already recording" : "the last take is still being analysed";
            else if (! processor.startRecording())
                error = "could not start recording: audio is not running";
        }
        else if (action == "stop_recording")
        {
            if (! recording)
            {
                error = "not recording";
            }
            else
            {
                processor.stopRecording();
                if (! processor.waitForTakeAnalysis(30000))
                    error = "the analysis did not finish in 30 seconds; poll get_take";
            }
        }
        else if (action == "play")
        {
            double from = 0.0;
            const auto f = args.getProperty("fromSeconds", juce::var());
            if (! f.isVoid())
            {
                if (! (f.isInt() || f.isInt64() || f.isDouble()) || (double) f < 0.0 || (double) f >= takeLength(take))
                    error = "fromSeconds must be a number from 0 up to the take's end (" + juce::String(takeLength(take), 2) + " s)";
                else
                    from = (double) f;
            }
            if (error.isEmpty())
            {
                if (recording || processor.isAnalysingTake())
                    error = "cannot play while recording or analysing";
                else if (take.notes.empty())
                    error = "the take has no notes to play";
                else if (processor.isTakePlaying())
                    error = "already playing";
                else
                    processor.playTake(from);
            }
        }
        else if (action == "stop")
        {
            if (! processor.isTakePlaying())
                error = "not playing";
            else
                processor.stopTake();
        }
        else
        {
            error = "action must be one of record, stop_recording, play, stop";
        }

        if (error.isNotEmpty())
            return {};
        return takeStateJson(processor);
    };
    return t;
}
} // namespace murmur
