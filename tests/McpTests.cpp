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
#include <juce_events/juce_events.h>

namespace murmur::tests
{
namespace
{
    // A processor plus the message loop JUCE expects around one (the MCP bridge wants it too).
    struct Host
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        MurmurProcessor processor;
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
            expect(tools->size() >= 7, "the table lost tools");
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
                                   "get_calibration", "start_calibration", "clear_calibration" })
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
    }
};

static McpTests mcpTests;
} // namespace murmur::tests
