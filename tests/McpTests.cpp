// The MCP tool layer: argument validation, what each tool reports, and that a tool which
// changes state really changes it.
//
// These go through the server's JSON-RPC layer (handleLine), not through a tool's `run`
// directly, so the schema, the registration and the JSON-RPC envelope are exercised on the
// way past. A tool with malformed params, a duplicate name addTool refused, or one that never
// reached the table fails here, where calling `run` could not notice any of those. It is the
// same seam Keys's tests use (see ../Keys/tests/McpTests.cpp).
//
// The bridge under test is the one the processor already owns (`processor.mcp()`), so no
// socket, client or second server is involved. tests/DspTests.cpp owns the runner's main();
// this file is self-registering statics only.

#include "../src/PluginProcessor.h"
#include "../src/mcp/MurmurMcp.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <juce_events/juce_events.h>

namespace murmur::tests
{
namespace
{
    // A processor plus the message loop JUCE expects around one (the MCP bridge wants it too).
    constexpr double kRate = 44100.0;
    constexpr int kBlock = 512;

    struct Host
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        MurmurProcessor processor;
        juce::File folder;

        // Prepared like a host would, with takes going to a folder of its own.
        Host()
        {
            folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getNonexistentChildFile("murmur-mcp-test", "");
            processor.setTakesFolder(folder);
            processor.setRateAndBufferSizeDetails(kRate, kBlock);
            processor.prepareToPlay(kRate, kBlock);
        }
        ~Host() { folder.deleteRecursively(); }

        // Feeds a 440 Hz tone (A4) through processBlock for `seconds`.
        void feedTone(double seconds)
        {
            juce::AudioBuffer<float> buf(2, kBlock);
            juce::MidiBuffer midi;
            const int total = (int) (seconds * kRate);
            for (int pos = 0; pos < total; pos += kBlock)
            {
                const int n = std::min(kBlock, total - pos);
                buf.setSize(2, n, false, false, true);
                for (int i = 0; i < n; ++i)
                {
                    const double ph = 6.283185307 * 440.0 * (double) (pos + i) / kRate;
                    const float s = 0.4f * (float) (std::sin(ph) + 0.5 * std::sin(2 * ph));
                    buf.setSample(0, i, s);
                    buf.setSample(1, i, s);
                }
                processor.processBlock(buf, midi);
            }
        }

        // A known free take without recording: notes 60, 64, 67, half a second each.
        void installTake()
        {
            processor.editTake([](take::Take& t)
                               {
                                   t.notes.clear();
                                   for (int i = 0; i < 3; ++i)
                                   {
                                       take::TakeNote n;
                                       n.start = 0.5 * i;
                                       n.end = 0.5 * i + 0.5;
                                       n.sungMidi = (float) (60 + 4 * i) + 0.2f;
                                       n.detectedNote = n.targetNote = i == 0 ? 60 : i == 1 ? 64 : 67;
                                       n.velocity = 0.5f;
                                       t.notes.push_back(n);
                                   }
                               });
        }
    };

    // One JSON-RPC round trip. `error` comes back empty on success and carries the tool's own
    // message otherwise.
    juce::var call(MurmurProcessor& p, const juce::String& tool, const juce::var& toolArgs,
                   juce::String& error)
    {
        error.clear();

        auto* params = new juce::DynamicObject();
        params->setProperty("name", tool);
        params->setProperty("arguments", toolArgs);
        auto* req = new juce::DynamicObject();
        req->setProperty("jsonrpc", "2.0");
        req->setProperty("id", 1);
        req->setProperty("method", "tools/call");
        req->setProperty("params", juce::var(params));

        const auto responseLine = p.mcp()->handleLine(juce::JSON::toString(juce::var(req), true));
        const auto response = juce::JSON::parse(responseLine);

        if (response.hasProperty("error"))
        {
            error = response["error"]["message"].toString();
            if (error.isEmpty())
                error = "JSON-RPC error with no message: " + responseLine;
            return {};
        }

        // A tools/call result is a single text block holding the tool's own JSON.
        auto* content = response["result"]["content"].getArray();
        if (content == nullptr || content->isEmpty())
        {
            error = "malformed tools/call response: " + responseLine;
            return {};
        }
        return juce::JSON::parse((*content)[0]["text"].toString());
    }

    juce::var call(MurmurProcessor& p, const juce::String& tool, const juce::var& toolArgs)
    {
        juce::String ignored;
        return call(p, tool, toolArgs, ignored);
    }

    // tools/list, for the tests that want the registered table rather than one tool's answer.
    juce::var toolTable(MurmurProcessor& p)
    {
        auto* req = new juce::DynamicObject();
        req->setProperty("jsonrpc", "2.0");
        req->setProperty("id", 1);
        req->setProperty("method", "tools/list");
        const auto response = juce::JSON::parse(p.mcp()->handleLine(juce::JSON::toString(juce::var(req), true)));
        return response["result"]["tools"];
    }

    juce::var args(std::initializer_list<std::pair<const char*, juce::var>> kv)
    {
        auto* o = new juce::DynamicObject();
        for (const auto& entry : kv)
            o->setProperty(entry.first, entry.second);
        return juce::var(o);
    }

    // The entry for `drumClass` out of a calibration snapshot, or a void var when absent.
    // Returned by value because juce::UnitTest::expect records a failure and *returns*, it
    // does not abort: dereferencing a missing pointer after a failed expect would take the
    // whole binary down having printed nothing.
    juce::var classEntry(const juce::var& calibration, const juce::String& drumClass)
    {
        if (auto* classes = calibration["classes"].getArray())
            for (const auto& c : *classes)
                if (c["drumClass"].toString() == drumClass)
                    return c;
        return {};
    }
} // namespace

class McpTests : public juce::UnitTest
{
public:
    McpTests() : juce::UnitTest("Murmur MCP tools", "murmur") {}

    void runTest() override
    {
        beginTest("every registered tool has a name, a description and a usable schema");
        {
            Host h;
            // The table as the *server* holds it, not as buildTools() returns it. Held in a
            // named local: the var owns the array, so a temporary's getArray() would dangle.
            const auto table = toolTable(h.processor);
            auto* tools = table.getArray();
            if (tools == nullptr)
            {
                expect(false, "tools/list returned no array");
                return;
            }
            expect(tools->size() >= 10, "the table lost tools");
            juce::StringArray names;
            for (auto& t : *tools)
            {
                const auto toolName = t["name"].toString();
                expect(toolName.isNotEmpty(), "a tool has no name");
                expect(t["description"].toString().isNotEmpty(), "tool has no description: " + toolName);
                expect(! names.contains(toolName), "duplicate tool name: " + toolName);
                names.add(toolName);

                auto schema = t["inputSchema"];
                expect(schema["type"].toString() == "object", "tool has no object schema: " + toolName);
                if (auto* props = schema["properties"].getDynamicObject())
                    for (const auto& param : props->getProperties())
                    {
                        const auto where = toolName + "." + param.name.toString();
                        expect(param.value["type"].toString().isNotEmpty(), "param has no type: " + where);
                        expect(param.value["description"].toString().isNotEmpty(),
                               "param has no description: " + where);
                    }
            }
            // Named explicitly: docs/MCP.md documents these, and a rename would leave every
            // script written against the docs failing with "unknown tool".
            for (const char* n : { "get_state", "get_transport", "list_params", "set_params",
                                   "get_calibration", "start_calibration", "clear_calibration",
                                   "get_take", "edit_take", "take_transport" })
                expect(names.contains(n), juce::String("missing tool: ") + n);
        }

        beginTest("an unknown tool is an error, not a crash");
        {
            Host h;
            juce::String err;
            call(h.processor, "no_such_tool", args({}), err);
            expect(err.isNotEmpty(), "an unknown tool reported no error");
        }

        beginTest("the bridge answers on a loopback port");
        {
            Host h;
            expect(h.processor.mcp()->port() > 0, "the MCP server did not bind a port");
        }

        beginTest("get_state reports the defaults");
        {
            Host h;
            juce::String err;
            const auto s = call(h.processor, "get_state", args({}), err);
            expect(err.isEmpty(), "unexpected error: " + err);
            expectEquals(s["product"].toString(), juce::String("Murmur"));
            expectEquals(s["mode"].toString(), juce::String("Melody"));
            expectEquals(s["controls"]["scale"].toString(), juce::String("Major"));
            expect(s["lastNote"].isVoid(), "lastNote should be null before the first note");
            expectEquals((int) s["calibration"]["classes"].getArray()->size(), 4);
            expect(! (bool) s["hostPlaying"], "the host is not playing in a bare processor");
        }

        beginTest("set_params changes the parameter, and get_state sees it");
        {
            Host h;
            juce::String err;
            const auto r = call(h.processor, "set_params",
                                args({ { "values", args({ { "mode", "Drums" }, { "scaleLock", false },
                                                          { "quantize", 0.25 } }) } }),
                                err);
            expect(err.isEmpty(), "unexpected error: " + err);
            expectEquals(r["mode"].toString(), juce::String("Drums"));
            expect(h.processor.isDrumsMode(), "mode did not change");
            expect(h.processor.apvts.getRawParameterValue("scaleLock")->load() < 0.5f, "scaleLock still on");
            expectWithinAbsoluteError(h.processor.apvts.getRawParameterValue("quantize")->load(), 0.25f, 0.002f);
            expectEquals(call(h.processor, "get_state", args({}))["mode"].toString(), juce::String("Drums"));
        }

        beginTest("set_params with one unknown id applies nothing");
        {
            Host h;
            juce::String err;
            call(h.processor, "set_params",
                 args({ { "values", args({ { "mode", "Drums" }, { "no_such_param", 1 } }) } }), err);
            expect(err.contains("no_such_param"), "the unknown id was not named: " + err);
            expect(! h.processor.isDrumsMode(), "a valid id was applied despite the bad one");

            call(h.processor, "set_params", args({ { "values", "not an object" } }), err);
            expect(err.isNotEmpty(), "a non-object values was accepted");
        }

        beginTest("list_params covers every parameter with a range or a choice list");
        {
            Host h;
            const auto list = call(h.processor, "list_params", args({}));
            auto* arr = list.getArray();
            expect(arr != nullptr, "list_params returned no array");
            if (arr == nullptr)
                return;
            expectEquals((int) arr->size(), 8);
            for (const auto& p : *arr)
                expect(p.hasProperty("choices") || (p.hasProperty("min") && p.hasProperty("max")),
                       "parameter without a range: " + p["id"].toString());
        }

        beginTest("get_transport follows the inferred tempo while the host is stopped");
        {
            Host h;
            const auto t = call(h.processor, "get_transport", args({}));
            expectEquals(t["source"].toString(), juce::String("inferred"));
            expect(t["bpm"].isVoid(), "no inferred tempo yet, so bpm should be null");
            expect(! (bool) t["hostPlaying"]);
            expectEquals(t["grid"].toString(), juce::String("1/16"));
        }

        beginTest("start_calibration arms the engine for that class and clear_calibration reports it");
        {
            Host h;
            juce::String err;
            const auto armed = call(h.processor, "start_calibration", args({ { "drumClass", "snare" } }), err);
            expect(err.isEmpty(), "unexpected error: " + err);
            expect((bool) armed["calibrating"], "calibration did not start");
            expectEquals(armed["calibratingClass"].toString(), juce::String("snare"));
            expect(h.processor.getEngine().isCalibrating());

            const auto cal = call(h.processor, "get_calibration", args({}));
            expect((bool) cal["calibrating"]);
            expect(! (bool) classEntry(cal, "snare")["calibrated"], "no examples captured yet");
            expectEquals((int) classEntry(cal, "kick")["midiNote"], 36);
            expectEquals((int) classEntry(cal, "hatOpen")["midiNote"], 46);

            const auto cleared = call(h.processor, "clear_calibration", args({ { "drumClass", 1 } }), err);
            expect(err.isEmpty(), "unexpected error: " + err);
            expectEquals((int) classEntry(cleared, "snare")["examples"], 0);
        }

        beginTest("a drumClass that does not exist is rejected, not clamped to Kick");
        {
            Host h;
            juce::String err;
            for (const auto& bad : { juce::var("cowbell"), juce::var(4), juce::var(-1), juce::var() })
            {
                call(h.processor, "start_calibration", args({ { "drumClass", bad } }), err);
                expect(err.isNotEmpty(), "start_calibration accepted a bad drumClass");
                call(h.processor, "clear_calibration", args({ { "drumClass", bad } }), err);
                expect(err.isNotEmpty(), "clear_calibration accepted a bad drumClass");
            }
            expect(! h.processor.getEngine().isCalibrating(), "a rejected call still armed calibration");
        }

        beginTest("get_take on an empty processor reports no take");
        {
            Host h;
            juce::String err;
            const auto r = call(h.processor, "get_take", args({}), err);
            expect(err.isEmpty(), "unexpected error: " + err);
            expect(! (bool) r["hasTake"], "no take yet");
            expect(! (bool) r["recording"] && ! (bool) r["analysing"] && ! (bool) r["playing"]);
            expect(r["error"].isVoid(), "no error text");
            expect(r["anchor"].isVoid(), "no anchor");
            expectEquals((int) r["notes"].getArray()->size(), 0);
        }

        beginTest("get_take lists the notes with indexes, targets and names");
        {
            Host h;
            h.installTake();
            h.processor.editTake([](take::Take& t)
                                 {
                                     t.notes[1].targetNote = 65;
                                     t.anchorPpq = 8.0;
                                     t.anchorBpm = 90.0;
                                 });
            const auto r = call(h.processor, "get_take", args({}));
            expect((bool) r["hasTake"]);
            expectEquals((int) r["noteCount"], 3);
            expectEquals((double) r["anchor"]["ppq"], 8.0);
            expectEquals((double) r["anchor"]["bpm"], 90.0);
            auto* notes = r["notes"].getArray();
            expect(notes != nullptr && notes->size() == 3, "three notes");
            if (notes != nullptr && notes->size() == 3)
            {
                const auto& n = (*notes)[1];
                expectEquals((int) n["index"], 1);
                expectEquals((int) n["detectedNote"], 64);
                expectEquals((int) n["targetNote"], 65);
                expectEquals(n["targetName"].toString(), juce::String("F3"));
                expectWithinAbsoluteError((double) n["start"], 0.5, 1e-9);
                expectWithinAbsoluteError((double) n["end"], 1.0, 1e-9);
                expectWithinAbsoluteError((double) n["sungMidi"], 64.2, 1e-4);
                expectWithinAbsoluteError((double) n["velocity"], 0.5, 1e-6);
                expect(n["sungName"].toString().isNotEmpty() && n["detectedName"].toString().isNotEmpty());
            }
        }

        beginTest("edit_take shift moves by semitones, or by scale degrees with scaleLock on");
        {
            Host h;
            h.installTake();
            juce::String err;
            call(h.processor, "set_params", args({ { "values", args({ { "scaleLock", false } }) } }), err);
            auto r = call(h.processor, "edit_take", args({ { "op", "shift" }, { "index", 0 }, { "steps", 3 } }), err);
            expect(err.isEmpty(), "unexpected error: " + err);
            expectEquals((int) r["targetNote"], 63);
            expectEquals((int) h.processor.getTake().notes[0].targetNote, 63);

            // Locked: the target steps to the next scale note, whatever the scale.
            call(h.processor, "set_params", args({ { "values", args({ { "scaleLock", true } }) } }), err);
            const int root = (int) h.processor.apvts.getRawParameterValue("root")->load();
            const int scale = (int) h.processor.apvts.getRawParameterValue("scale")->load();
            r = call(h.processor, "edit_take", args({ { "op", "shift" }, { "index", 1 }, { "steps", 1 } }), err);
            expect(err.isEmpty(), "unexpected error: " + err);
            expectEquals((int) r["targetNote"], take::detail::nextInScale(64, 1, root, scale));
            expect(okstudio::scales::isInScale((int) r["targetNote"], root, scale), "landed in the scale");
        }

        beginTest("edit_take set, reset, delete, snap_to_scale and undo");
        {
            Host h;
            h.installTake();
            juce::String err;
            auto r = call(h.processor, "edit_take", args({ { "op", "set" }, { "index", 2 }, { "note", 70 } }), err);
            expect(err.isEmpty(), "unexpected error: " + err);
            expectEquals((int) r["targetNote"], 70);
            expectEquals(r["targetName"].toString(), juce::String("A#3"));

            r = call(h.processor, "edit_take", args({ { "op", "reset" }, { "index", 2 } }), err);
            expect(err.isEmpty(), "unexpected error: " + err);
            expectEquals((int) r["targetNote"], 67);
            expectEquals((int) h.processor.getTake().notes[2].targetNote, 67);

            call(h.processor, "edit_take", args({ { "op", "set" }, { "index", 0 }, { "note", 61 } }), err);
            r = call(h.processor, "edit_take", args({ { "op", "snap_to_scale" } }), err);
            expect(err.isEmpty(), "unexpected error: " + err);
            expectEquals((int) r["noteCount"], 3);
            const int root = (int) h.processor.apvts.getRawParameterValue("root")->load();
            const int scale = (int) h.processor.apvts.getRawParameterValue("scale")->load();
            expect(okstudio::scales::isInScale(h.processor.getTake().notes[0].targetNote, root, scale),
                   "note 0 is in the scale after snapping");

            r = call(h.processor, "edit_take", args({ { "op", "delete" }, { "index", 1 } }), err);
            expect(err.isEmpty(), "unexpected error: " + err);
            expectEquals((int) r["noteCount"], 2);
            expectEquals((int) h.processor.getTake().notes.size(), 2);

            r = call(h.processor, "edit_take", args({ { "op", "undo" } }), err); // brings the deleted note back
            expect(err.isEmpty(), "unexpected error: " + err);
            expectEquals((int) r["noteCount"], 3);
            expectEquals((int) h.processor.getTake().notes[1].targetNote, 64);
        }

        beginTest("edit_take rejects bad arguments and changes nothing");
        {
            Host h;
            h.installTake();
            const auto before = take::toValueTree(h.processor.getTake()).toXmlString();
            const int version = h.processor.getTakeVersion();
            juce::String err;

            const std::initializer_list<juce::var> bad = {
                args({}),                                                   // no op
                args({ { "op", "explode" } }),                              // unknown op
                args({ { "op", 5 } }),                                      // op not a string
                args({ { "op", "shift" }, { "steps", 1 } }),                // no index
                args({ { "op", "shift" }, { "index", 3 }, { "steps", 1 } }), // index past the end
                args({ { "op", "shift" }, { "index", -1 }, { "steps", 1 } }),
                args({ { "op", "shift" }, { "index", 1.5 }, { "steps", 1 } }), // fractional index
                args({ { "op", "shift" }, { "index", "1" }, { "steps", 1 } }), // index as text
                args({ { "op", "shift" }, { "index", 0 } }),                // no steps
                args({ { "op", "shift" }, { "index", 0 }, { "steps", "up" } }),
                args({ { "op", "set" }, { "index", 0 } }),                  // no note
                args({ { "op", "set" }, { "index", 0 }, { "note", 128 } }), // note out of range
                args({ { "op", "set" }, { "index", 0 }, { "note", -1 } }),
                args({ { "op", "set" }, { "note", 60 } }),                  // no index
                args({ { "op", "reset" } }),
                args({ { "op", "reset" }, { "index", 9 } }),
                args({ { "op", "delete" } }),
                args({ { "op", "delete" }, { "index", 3 } }),
            };
            int i = 0;
            for (const auto& a : bad)
            {
                ++i;
                call(h.processor, "edit_take", a, err);
                expect(err.isNotEmpty(), "edit_take accepted bad arguments #" + juce::String(i));
            }
            expectEquals(take::toValueTree(h.processor.getTake()).toXmlString(), before);
            expectEquals(h.processor.getTakeVersion(), version);

            Host fresh;
            call(fresh.processor, "edit_take", args({ { "op", "undo" } }), err);
            expect(err.isNotEmpty(), "undo with an empty history should fail");
            call(fresh.processor, "edit_take", args({ { "op", "shift" }, { "index", 0 }, { "steps", 1 } }), err);
            expect(err.isNotEmpty(), "shift on an empty take should fail");
        }

        beginTest("take_transport play and stop, with errors");
        {
            Host h;
            juce::String err;
            call(h.processor, "take_transport", args({ { "action", "play" } }), err);
            expect(err.isNotEmpty(), "play with no notes should fail");
            call(h.processor, "take_transport", args({ { "action", "stop" } }), err);
            expect(err.isNotEmpty(), "stop when not playing should fail");
            call(h.processor, "take_transport", args({ { "action", "stop_recording" } }), err);
            expect(err.isNotEmpty(), "stop_recording when not recording should fail");
            call(h.processor, "take_transport", args({ { "action", "rewind" } }), err);
            expect(err.isNotEmpty(), "unknown action should fail");
            call(h.processor, "take_transport", args({}), err);
            expect(err.isNotEmpty(), "missing action should fail");

            h.installTake(); // 1.5 s long
            for (const auto& bad : { juce::var(-1.0), juce::var(1.5), juce::var(99), juce::var("soon") })
            {
                call(h.processor, "take_transport", args({ { "action", "play" }, { "fromSeconds", bad } }), err);
                expect(err.isNotEmpty(), "play accepted a bad fromSeconds");
                expect(! h.processor.isTakePlaying(), "a rejected play still started");
            }

            auto r = call(h.processor, "take_transport", args({ { "action", "play" }, { "fromSeconds", 0.5 } }), err);
            expect(err.isEmpty(), "unexpected error: " + err);
            expect(h.processor.isTakePlaying(), "playing");
            expect((bool) r["playing"]);
            expectEquals((int) r["noteCount"], 3);
            call(h.processor, "take_transport", args({ { "action", "play" } }), err);
            expect(err.isNotEmpty(), "play while playing should fail");
            expect((bool) call(h.processor, "get_take", args({}))["playing"], "get_take sees it playing");

            r = call(h.processor, "take_transport", args({ { "action", "stop" } }), err);
            expect(err.isEmpty(), "unexpected error: " + err);
            expect(! h.processor.isTakePlaying(), "stopped");
            expect(! (bool) r["playing"]);
        }

        beginTest("take_transport record and stop_recording make a take from the input");
        {
            Host h;
            juce::String err;
            auto r = call(h.processor, "take_transport", args({ { "action", "record" } }), err);
            expect(err.isEmpty(), "unexpected error: " + err);
            expect((bool) r["recording"], "recording");
            call(h.processor, "take_transport", args({ { "action", "record" } }), err);
            expect(err.isNotEmpty(), "record while recording should fail");

            h.feedTone(1.0);
            r = call(h.processor, "get_take", args({}));
            expect((bool) r["recording"] && (double) r["secondsRecorded"] > 0.9, "get_take sees the recording");
            call(h.processor, "edit_take", args({ { "op", "snap_to_scale" } }), err);
            expect(err.isNotEmpty(), "editing while recording should fail");
            call(h.processor, "take_transport", args({ { "action", "play" } }), err);
            expect(err.isNotEmpty(), "play while recording should fail");

            r = call(h.processor, "take_transport", args({ { "action", "stop_recording" } }), err);
            expect(err.isEmpty(), "unexpected error: " + err);
            expect(! (bool) r["recording"] && ! (bool) r["analysing"], "analysis finished");
            expectEquals((int) r["noteCount"], 1);
            r = call(h.processor, "get_take", args({}));
            expect((bool) r["hasTake"]);
            expectEquals((int) r["notes"][0]["targetNote"], 69); // 440 Hz
            expectEquals(r["notes"][0]["targetName"].toString(), juce::String("A3"));
            h.processor.clearTake();
        }
    }
};

static McpTests mcpTests;
} // namespace murmur::tests
