// MCP (Model Context Protocol) server built into QIDI Studio.
//
// Lets an AI assistant on this computer drive the slicer: load models, pick presets, change
// settings, slice, look at the plate, map QIDI Box slots and send the print to the printer.
//
// Transport: MCP "Streamable HTTP" (JSON responses only, no SSE stream) on 127.0.0.1.
// Every request needs the token from Preferences, as "Authorization: Bearer <token>".
//
// The HTTP server and the GUI-thread hand-off follow the Remote API of the OrcaSlicer MCP build
// (https://github.com/MaxEllis/OrcaSlicer, AGPL-3.0), reduced to what this fork needs.
#ifndef slic3r_GUI_McpServer_hpp_
#define slic3r_GUI_McpServer_hpp_

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

constexpr int k_default_port = 13140;

// Persisted in AppConfig. load() on the GUI thread only.
struct Config
{
    bool        enabled { false };
    int         port { k_default_port };
    std::string token;

    static Config load(); // makes and stores a token on first use
};

// A tool's answer. `content` is the MCP content array (text and image items).
struct ToolResult
{
    nlohmann::json content  = nlohmann::json::array();
    bool           is_error = false;

    static ToolResult json(const nlohmann::json &j);         // one text item holding the JSON
    static ToolResult error(const std::string &message, const nlohmann::json &detail = nullptr);
    ToolResult       &add_text(const std::string &text);
    ToolResult       &add_image(const std::string &bytes, const char *mime_type);
};

// Thrown by tools for a refusal the assistant should read (wrong argument, nothing loaded, ...).
struct ToolError : std::runtime_error
{
    nlohmann::json detail;
    explicit ToolError(const std::string &msg, nlohmann::json d = nullptr) : std::runtime_error(msg), detail(std::move(d)) {}
};

struct Tool
{
    std::string                                            name;
    std::string                                            description;
    nlohmann::json                                         input_schema;
    // Runs on a worker thread. Use run_on_ui() for anything that touches the GUI, model or presets.
    std::function<ToolResult(const nlohmann::json &args)> fn;
    bool                                                   read_only { false };
    bool                                                   destructive { false };
};

// The GUI thread hand-off (UiHandoff in McpHandoff.hpp tells the rules).
// Runs fn on the GUI thread and waits for it. Throws ToolError on timeout or when the server stops.
nlohmann::json run_on_ui(std::function<nlohmann::json()> fn, int timeout_s = 15);
// Asks done_on_ui on the GUI thread every poll_ms until it gives true. Throws ToolError(still_busy) after timeout_s.
void wait_until(std::function<bool()> done_on_ui, int timeout_s, int poll_ms, const std::string &still_busy);
// True while the server stops. Long work on a worker thread, for example a printer request, ends when it is true.
bool stopping();

// Tool registration (McpTools.cpp, McpPrinterTools.cpp).
void register_slicer_tools(std::vector<Tool> &tools);
void register_printer_tools(std::vector<Tool> &tools);

class HttpListener; // McpServer.cpp

class Server
{
public:
    static Server &instance();

    // GUI thread. Reads the config and starts or stops the listener to match it.
    void apply_config();
    void stop();

    bool        running() const { return m_listener != nullptr; }
    int         port() const { return m_cfg.port; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(m_cfg.port) + "/mcp"; }
    // A ready-to-paste command that adds this server to Claude Code for the folder it is run in (local scope).
    std::string claude_code_command() const;

    // Internal (HTTP sessions).
    bool check_token(const std::string &presented) const;
    // Handles one JSON-RPC message (or batch). Returns null for notifications only.
    nlohmann::json handle_jsonrpc(const nlohmann::json &msg);

private:
    Server();
    ~Server();
    nlohmann::json handle_one(const nlohmann::json &msg);
    nlohmann::json tools_list() const;
    nlohmann::json tools_call(const nlohmann::json &params);

    Config                        m_cfg;
    mutable std::mutex            m_cfg_mutex; // m_cfg is written on the GUI thread, the token read on HTTP workers
    std::unique_ptr<HttpListener> m_listener;  // set while the server runs
    std::vector<Tool>             m_tools;
};

}}} // namespace Slic3r::GUI::Mcp

#endif
