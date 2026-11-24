// MCP server built into QIDI Studio. See McpServer.hpp.
#include "McpServer.hpp"
#include "McpHandoff.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Thread.hpp"
#include "libslic3r_version.h"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/beast/http.hpp>
#include <boost/log/trivial.hpp>
#include <boost/thread.hpp>
#include <openssl/rand.h>

#include <atomic>
#include <chrono>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

namespace Slic3r { namespace GUI { namespace Mcp {

static const char *k_protocol_versions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

static const char *k_instructions =
    "This server drives QIDI Studio, the slicer for QIDI 3D printers, running on this computer.\n"
    "Typical order: get_status -> load_model -> list_presets / select_preset -> get_config / set_config -> "
    "arrange or transform_object -> slice -> render_plate to check -> list_printers -> get_box_slots -> "
    "send_print with a slot for each filament.\n"
    "Paths are paths on this computer. Changes show in the QIDI Studio window and can be undone there with Ctrl/Cmd+Z "
    "(model edits) or by switching presets back. send_print starts a real print: confirm with the user first.";

// ---------------------------------------------------------------------------------------------------------------
// Config

static std::string generate_token()
{
    static const char hex[] = "0123456789abcdef";
    unsigned char     bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        // Fail closed: an empty token rejects every request.
        BOOST_LOG_TRIVIAL(error) << "mcp: CSPRNG unavailable, no token generated";
        return {};
    }
    std::string tok(2 * sizeof(bytes), '0');
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        tok[2 * i]     = hex[bytes[i] >> 4];
        tok[2 * i + 1] = hex[bytes[i] & 0x0F];
    }
    return tok;
}

Config Config::load()
{
    AppConfig *app_config = wxGetApp().app_config;
    Config     cfg;
    cfg.enabled = app_config->get_bool("mcp_server_enabled");
    if (app_config->has("mcp_server_port")) {
        try { cfg.port = std::stoi(app_config->get("mcp_server_port")); } catch (...) { cfg.port = k_default_port; }
    }
    cfg.token = app_config->get("mcp_server_token");
    if (cfg.token.empty()) {
        cfg.token = generate_token();
        app_config->set("mcp_server_token", cfg.token);
    }
    return cfg;
}

// ---------------------------------------------------------------------------------------------------------------
// ToolResult

static std::string dump_json(const nlohmann::json &j)
{
    return j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

ToolResult ToolResult::json(const nlohmann::json &j)
{
    ToolResult r;
    r.add_text(dump_json(j));
    return r;
}

ToolResult ToolResult::error(const std::string &message, const nlohmann::json &detail)
{
    ToolResult     r;
    nlohmann::json j = {{"error", message}};
    if (!detail.is_null()) j["detail"] = detail;
    r.add_text(dump_json(j));
    r.is_error = true;
    return r;
}

ToolResult &ToolResult::add_text(const std::string &text)
{
    content.push_back({{"type", "text"}, {"text", text}});
    return *this;
}

ToolResult &ToolResult::add_image(const std::string &bytes, const char *mime_type)
{
    std::string b64(beast::detail::base64::encoded_size(bytes.size()), '\0');
    b64.resize(beast::detail::base64::encode(&b64[0], bytes.data(), bytes.size()));
    content.push_back({{"type", "image"}, {"data", b64}, {"mimeType", mime_type}});
    return *this;
}

// ---------------------------------------------------------------------------------------------------------------
// GUI thread hand-off

// HttpListener::stop() stops the hand-off first: the GUI thread then waits for the running tool calls, so they must
// not wait for it.
static UiHandoff g_handoff([](std::function<void()> step) { wxGetApp().CallAfter(std::move(step)); });

nlohmann::json run_on_ui(std::function<nlohmann::json()> fn, int timeout_s) { return g_handoff.run(std::move(fn), timeout_s); }

void wait_until(std::function<bool()> done_on_ui, int timeout_s, int poll_ms, const std::string &still_busy)
{
    g_handoff.wait_until(std::move(done_on_ui), timeout_s, poll_ms, still_busy);
}

bool stopping() { return g_handoff.stopping(); }

// ---------------------------------------------------------------------------------------------------------------
// HTTP

class HttpListener
{
public:
    HttpListener(Server &server) : m_server(server) {}
    ~HttpListener() { stop(); }

    bool start(int port)
    {
        std::unique_ptr<net::io_context> ioc;
        std::unique_ptr<tcp::acceptor>   acceptor;
        try {
            ioc = std::make_unique<net::io_context>(1);
            tcp::endpoint endpoint(net::ip::make_address_v4("127.0.0.1"), static_cast<unsigned short>(port));
            acceptor = std::make_unique<tcp::acceptor>(*ioc);
            acceptor->open(endpoint.protocol());
#ifdef _WIN32
            acceptor->set_option(net::detail::socket_option::boolean<SOL_SOCKET, SO_EXCLUSIVEADDRUSE>(true));
#else
            acceptor->set_option(net::socket_base::reuse_address(true));
#endif
            acceptor->bind(endpoint);
            acceptor->listen(net::socket_base::max_listen_connections);
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "mcp: failed to listen on port " << port << ": " << e.what();
            return false;
        }
        // The acceptor must go before the io_context it is registered with.
        m_acceptor = std::move(acceptor);
        m_ioc      = std::move(ioc);
        m_pool     = std::make_unique<net::thread_pool>(4);
        m_running  = true;
        g_handoff.start();
        do_accept();
        m_thread = create_thread([this] {
            set_current_thread_name("mcp_http");
            m_ioc->run();
        });
        BOOST_LOG_TRIVIAL(info) << "mcp: listening on 127.0.0.1:" << port;
        return true;
    }

    void stop()
    {
        if (!m_running) return;
        m_running = false;
        g_handoff.stop(); // running tool calls and their printer requests end, instead of waiting for us
        if (m_ioc) m_ioc->stop();
        if (m_thread.joinable()) m_thread.join();
        if (m_pool) {
            m_pool->stop();
            m_pool->join();
        }
        m_pool.reset();
        m_acceptor.reset();
        m_ioc.reset();
        BOOST_LOG_TRIVIAL(info) << "mcp: stopped";
    }

    net::thread_pool &pool() { return *m_pool; }
    Server           &server() { return m_server; }

private:
    void do_accept();

    Server                           &m_server;
    std::atomic<bool>                 m_running { false };
    std::unique_ptr<net::io_context>  m_ioc;
    std::unique_ptr<tcp::acceptor>    m_acceptor;
    std::unique_ptr<net::thread_pool> m_pool;
    boost::thread                     m_thread;
};

// One HTTP connection: read a request, answer it on a worker thread, write, close.
class HttpSession : public std::enable_shared_from_this<HttpSession>
{
public:
    HttpSession(tcp::socket socket, HttpListener &listener) : m_stream(std::move(socket)), m_listener(listener) {}

    void run()
    {
        m_stream.expires_after(std::chrono::seconds(30));
        m_parser.body_limit(8 * 1024 * 1024);
        http::async_read(m_stream, m_buffer, m_parser, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) return;
            self->m_stream.expires_never(); // tool calls such as slice may take minutes
            // Tool calls block (GUI hand-off, slicing, printer requests): never on the io thread.
            net::post(self->m_listener.pool(), [self] { self->handle(); });
        });
    }

private:
    static bool local_origin(const std::string &origin)
    {
        // DNS-rebinding guard from the MCP spec: browsers send Origin, local MCP clients usually do not.
        if (origin.empty() || origin == "null") return origin.empty();
        for (const char *ok : {"http://127.0.0.1", "http://localhost", "https://127.0.0.1", "https://localhost"}) {
            const std::string p(ok);
            if (origin.compare(0, p.size(), p) == 0 && (origin.size() == p.size() || origin[p.size()] == ':'))
                return true;
        }
        return false;
    }

    void handle()
    {
        auto       &req = m_parser.get();
        auto        res = std::make_shared<http::response<http::string_body>>(http::status::ok, req.version());
        res->set(http::field::server, "qidistudio-mcp");
        res->keep_alive(false);
        auto set_json = [&res](http::status st, const nlohmann::json &j) {
            res->result(st);
            res->set(http::field::content_type, "application/json");
            res->body() = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        };

        try {
            std::string target(req.target());
            if (auto q = target.find('?'); q != std::string::npos) target.resize(q);
            auto              origin_sv = req[http::field::origin];
            const std::string origin(origin_sv.begin(), origin_sv.end());

            auto              auth_sv = req[http::field::authorization];
            const std::string auth(auth_sv.begin(), auth_sv.end());
            const std::string token = boost::istarts_with(auth, "Bearer ") ? auth.substr(7) : std::string();

            if (target != "/mcp" && target != "/mcp/") {
                set_json(http::status::not_found, {{"error", "not_found"}, {"detail", "the MCP endpoint is /mcp"}});
            } else if (!local_origin(origin)) {
                set_json(http::status::forbidden, {{"error", "origin_not_allowed"}});
            } else if (!m_listener.server().check_token(token)) {
                set_json(http::status::unauthorized, {{"error", "unauthorized"},
                                                     {"detail", "send the token from QIDI Studio Preferences as 'Authorization: Bearer <token>'"}});
            } else if (req.method() != http::verb::post) {
                // No server-initiated SSE stream and no sessions to delete.
                res->result(http::status::method_not_allowed);
                res->set(http::field::allow, "POST");
            } else {
                nlohmann::json msg = nlohmann::json::parse(req.body(), nullptr, false);
                if (msg.is_discarded()) {
                    set_json(http::status::bad_request,
                             {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "Parse error"}}}});
                } else {
                    nlohmann::json reply = m_listener.server().handle_jsonrpc(msg);
                    if (reply.is_null())
                        res->result(http::status::accepted); // notifications / responses only
                    else
                        set_json(http::status::ok, reply);
                }
            }
        } catch (const std::exception &e) {
            // The MCP server must never take the slicer down.
            BOOST_LOG_TRIVIAL(error) << "mcp: request failed: " << e.what();
            set_json(http::status::internal_server_error, {{"error", "internal"}, {"detail", e.what()}});
        }
        res->prepare_payload();

        // Back to the io thread for the write.
        net::post(m_stream.get_executor(), [self = shared_from_this(), res] {
            self->m_stream.expires_after(std::chrono::seconds(30));
            http::async_write(self->m_stream, *res, [self, res](beast::error_code, std::size_t) {
                beast::error_code ec;
                self->m_stream.socket().shutdown(tcp::socket::shutdown_send, ec);
            });
        });
    }

    beast::tcp_stream                       m_stream;
    beast::flat_buffer                      m_buffer;
    http::request_parser<http::string_body> m_parser;
    HttpListener                           &m_listener;
};

void HttpListener::do_accept()
{
    m_acceptor->async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (!m_running) return;
        if (!ec) std::make_shared<HttpSession>(std::move(socket), *this)->run();
        do_accept();
    });
}

// ---------------------------------------------------------------------------------------------------------------
// Server

Server &Server::instance()
{
    static Server s;
    return s;
}

Server::Server()
{
    register_slicer_tools(m_tools);
    register_printer_tools(m_tools);
}

Server::~Server() { stop(); }

void Server::apply_config()
{
    Config cfg = Config::load();
    const bool same = running() && cfg.port == m_cfg.port && cfg.enabled;
    {
        std::lock_guard<std::mutex> lock(m_cfg_mutex); // the token may have changed: check_token reads it
        m_cfg = cfg;
    }
    if (same) return;
    stop();
    if (!m_cfg.enabled) return;
    m_listener = std::make_unique<HttpListener>(*this);
    if (!m_listener->start(m_cfg.port)) m_listener.reset();
}

void Server::stop()
{
    if (m_listener) m_listener->stop();
    m_listener.reset();
}

std::string Server::claude_code_command() const
{
    return "claude mcp add --transport http qidistudio " + url() + " --header \"Authorization: Bearer " + m_cfg.token + "\"";
}

bool Server::check_token(const std::string &presented) const
{
    // Constant time: this is the authentication path.
    std::string expected;
    {
        std::lock_guard<std::mutex> lock(m_cfg_mutex);
        expected = m_cfg.token;
    }
    if (expected.empty() || presented.size() != expected.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < expected.size(); ++i)
        diff |= static_cast<unsigned char>(presented[i] ^ expected[i]);
    return diff == 0;
}

nlohmann::json Server::handle_jsonrpc(const nlohmann::json &msg)
{
    if (msg.is_array()) { // JSON-RPC batch (2025-03-26)
        nlohmann::json out = nlohmann::json::array();
        for (const auto &m : msg) {
            nlohmann::json r = handle_one(m);
            if (!r.is_null()) out.push_back(std::move(r));
        }
        return out.empty() ? nlohmann::json(nullptr) : out;
    }
    return handle_one(msg);
}

static nlohmann::json rpc_error(const nlohmann::json &id, int code, const std::string &message)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

nlohmann::json Server::handle_one(const nlohmann::json &msg)
{
    if (!msg.is_object() || !msg.contains("method") || !msg["method"].is_string()) {
        // A response from the client (we send no requests) or garbage.
        if (msg.is_object() && (msg.contains("result") || msg.contains("error"))) return nullptr;
        return rpc_error(msg.is_object() && msg.contains("id") ? msg["id"] : nlohmann::json(nullptr), -32600, "Invalid Request");
    }
    if (!msg.contains("id")) return nullptr; // notifications/initialized, notifications/cancelled, ...
    const std::string    method = msg["method"].get<std::string>();
    const nlohmann::json id     = msg["id"];
    const nlohmann::json params = msg.value("params", nlohmann::json::object());

    try {
        nlohmann::json result;
        if (method == "initialize") {
            std::string version = k_protocol_versions[0];
            if (params.contains("protocolVersion") && params["protocolVersion"].is_string()) {
                const std::string asked = params["protocolVersion"].get<std::string>();
                for (const char *v : k_protocol_versions)
                    if (asked == v) version = asked;
            }
            result = {{"protocolVersion", version},
                      {"capabilities", {{"tools", {{"listChanged", false}}}}},
                      {"serverInfo", {{"name", "qidistudio"}, {"title", "QIDI Studio"}, {"version", SLIC3R_VERSION}}},
                      {"instructions", k_instructions}};
        } else if (method == "ping") {
            result = nlohmann::json::object();
        } else if (method == "tools/list") {
            result = tools_list();
        } else if (method == "tools/call") {
            result = tools_call(params);
        } else {
            return rpc_error(id, -32601, "Method not found: " + method);
        }
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
    } catch (const std::exception &e) {
        return rpc_error(id, -32603, e.what());
    }
}

nlohmann::json Server::tools_list() const
{
    nlohmann::json tools = nlohmann::json::array();
    for (const Tool &t : m_tools) {
        nlohmann::json j = {{"name", t.name}, {"description", t.description}, {"inputSchema", t.input_schema}};
        j["annotations"] = {{"readOnlyHint", t.read_only}, {"destructiveHint", t.destructive}};
        tools.push_back(std::move(j));
    }
    return {{"tools", tools}};
}

nlohmann::json Server::tools_call(const nlohmann::json &params)
{
    if (!params.contains("name") || !params["name"].is_string())
        throw std::invalid_argument("tools/call needs a tool name");
    const std::string name = params["name"].get<std::string>();
    const nlohmann::json args = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] :
                                                                                                   nlohmann::json::object();
    const Tool *tool = nullptr;
    for (const Tool &t : m_tools)
        if (t.name == name) tool = &t;
    if (tool == nullptr) throw std::invalid_argument("Unknown tool: " + name);

    ToolResult r;
    try {
        r = tool->fn(args);
    } catch (const ToolError &e) {
        r = ToolResult::error(e.what(), e.detail);
    } catch (const nlohmann::json::exception &e) {
        r = ToolResult::error(std::string("bad argument: ") + e.what());
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "mcp: tool " << name << " failed: " << e.what();
        r = ToolResult::error(e.what());
    }
    return {{"content", r.content}, {"isError", r.is_error}};
}

}}} // namespace Slic3r::GUI::Mcp
