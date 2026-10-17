#pragma once

#include <okstudio/Mcp.h>
#include <vector>

namespace murmur
{
class MurmurProcessor;

// Embeds the kit's MCP server (okstudio::mcp::Server) so Claude Code, or any other local MCP
// client, can read and drive Murmur: read and set parameters, read the live voice analysis
// (input level, last note or hit, inferred free tempo, host transport) and the drum
// calibration, start or clear a calibration, and read, edit, record and play back the Take
// Editor's take. See docs/MCP.md.
//
// Every tool's `run` lambda is called on the MESSAGE THREAD (the server marshals it there),
// the same thread the editor lives on, so tool bodies call straight into MurmurProcessor and
// its APVTS exactly the way the UI does. The values they read from the engine are the same
// atomics the editor's 30 Hz timer reads.
//
// Unlike Keys, Murmur schedules nothing: it makes no sound and plays no notes on request, so
// this bridge has no timer and no pending queues.
class MurmurMcp
{
public:
    explicit MurmurMcp(MurmurProcessor& processor);
    ~MurmurMcp();

    // One JSON-RPC line in, the response line out, straight through to the embedded server.
    // This is the seam `tests/McpTests.cpp` drives: no socket, no client, no second server,
    // and the schema, the registration and the JSON-RPC layer are all under test alongside
    // the tool body.
    juce::String handleLine(const juce::String& line) { return server.handleLine(line); }

    // The loopback port this instance answers on, or 0 if the bind failed. It is the only
    // thing that tells one Murmur from another over MCP: every instance in a Live set shares
    // a process, so the discovery file's pid is identical across all of them.
    int port() const { return server.getPort(); }

private:
    // The tool table, built but not registered; the constructor hands it straight to the
    // server. Private, because handleLine above is the seam.
    std::vector<okstudio::mcp::Tool> buildTools();

    okstudio::mcp::Tool toolGetState();
    okstudio::mcp::Tool toolGetTransport();
    okstudio::mcp::Tool toolListParams();
    okstudio::mcp::Tool toolSetParams();
    okstudio::mcp::Tool toolGetCalibration();
    okstudio::mcp::Tool toolStartCalibration();
    okstudio::mcp::Tool toolClearCalibration();
    okstudio::mcp::Tool toolGetTake();
    okstudio::mcp::Tool toolEditTake();
    okstudio::mcp::Tool toolTakeTransport();

    MurmurProcessor& processor;
    okstudio::mcp::Server server;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MurmurMcp)
};
} // namespace murmur
