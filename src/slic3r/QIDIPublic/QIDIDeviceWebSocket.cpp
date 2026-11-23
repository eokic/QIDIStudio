#include "QIDIDeviceWebSocket.hpp"

#include "slic3r/GUI/QDSDeviceManager.hpp"

#include <boost/log/trivial.hpp>

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace Slic3r { namespace GUI {

using json     = nlohmann::json;
using WsClient = websocketpp::client<websocketpp::config::asio_client>;

struct QIDIDeviceWebSocket::Connection
{
    WsClient                    client;
    websocketpp::connection_hdl hdl;
    std::thread                 thread;
    std::atomic<bool>           open { false };
    std::atomic<bool>           stopping { false };
    std::mutex                  send_mutex;
};

namespace {

// Printer objects the Device tab reads (QDSDevice::updateByJsonData). Klipper leaves out objects a
// printer doesn't have, so the list also keeps the older names QIDI subscribed to.
json subscribe_objects()
{
    json objects = json::object();
    for (const char *name : { "gcode_move", "exclude_object", "filament_switch_sensor filament_switch_sensor",
                              "fan_generic chamber_circulation_fan", "fan_generic cooling_fan",
                              "fan_generic auxiliary_cooling_fan", "output_pin polar_cooler", "output_pin caselight",
                              "print_stats_manager", "print_stats", "display_status", "toolhead", "heater_bed",
                              "extruder", "heater_generic chamber", "save_variables", "multi_color_controller",
                              "data" })
        objects[name] = nullptr;
    for (int box = 1; box <= 4; ++box)
        objects["aht20_f heater_box" + std::to_string(box)] = nullptr;
    for (int slot = 0; slot <= 16; ++slot)
        objects["box_stepper slot" + std::to_string(slot)] = nullptr;
    return objects;
}

int request_id(const std::string &device_id) { return std::atoi(device_id.c_str()); }

} // namespace

QIDIDeviceWebSocket::QIDIDeviceWebSocket() = default;

QIDIDeviceWebSocket::~QIDIDeviceWebSocket()
{
    for (const std::string &id : getConnectionIds())
        stopConnection(id);
}

std::shared_ptr<QIDIDeviceWebSocket::Connection> QIDIDeviceWebSocket::find(const std::string &device_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_connections.find(device_id);
    if (it == m_connections.end() || it->second->stopping)
        return nullptr;
    return it->second;
}

std::vector<std::string> QIDIDeviceWebSocket::getConnectionIds()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> ids;
    ids.reserve(m_connections.size());
    for (const auto &kv : m_connections)
        ids.push_back(kv.first);
    return ids;
}

bool QIDIDeviceWebSocket::connect(const std::string &device_id, const std::shared_ptr<QDSDevice> &device)
{
    if (!device) {
        BOOST_LOG_TRIVIAL(error) << "[WS] connect: device " << device_id << " not found";
        return false;
    }
    stopConnection(device_id);

    auto conn = std::make_shared<Connection>();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connections[device_id] = conn;
    }

    std::weak_ptr<Connection> weak = conn;
    auto report = [this, device_id, weak](const char *status) {
        auto c = weak.lock();
        if (!c || c->stopping)
            return;
        if (on_status_changed)
            on_status_changed(device_id, status);
    };

    try {
        conn->client.init_asio();
        conn->client.clear_access_channels(websocketpp::log::alevel::all);
        conn->client.clear_error_channels(websocketpp::log::elevel::all);

        conn->client.set_open_handler([this, device_id, weak, report](websocketpp::connection_hdl) {
            auto c = weak.lock();
            if (!c)
                return;
            c->open = true;
            BOOST_LOG_TRIVIAL(info) << "[WS] device " << device_id << " connected";
            report("connected");
            // Same pacing QIDI used: subscribe, then ask for the error list.
            c->client.set_timer(100, [this, device_id](const websocketpp::lib::error_code &ec) {
                if (ec)
                    return;
                sendSubscribeMessage(device_id);
            });
            c->client.set_timer(200, [this, device_id](const websocketpp::lib::error_code &ec) {
                if (ec)
                    return;
                getAllErrorList(device_id);
            });
        });
        conn->client.set_message_handler([this, device_id, weak](websocketpp::connection_hdl, WsClient::message_ptr msg) {
            auto c = weak.lock();
            if (!c || c->stopping)
                return;
            json message;
            try {
                message = json::parse(msg->get_payload());
            } catch (const std::exception &) {
                return;
            }
            try {
                if (on_message_received)
                    on_message_received(device_id, message);
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(warning) << "[WS] device " << device_id << " message handler: " << e.what();
            }
        });
        conn->client.set_close_handler([device_id, weak, report](websocketpp::connection_hdl) {
            if (auto c = weak.lock())
                c->open = false;
            BOOST_LOG_TRIVIAL(info) << "[WS] device " << device_id << " disconnected";
            report("offline");
        });
        conn->client.set_fail_handler([device_id, weak, report](websocketpp::connection_hdl) {
            if (auto c = weak.lock())
                c->open = false;
            BOOST_LOG_TRIVIAL(info) << "[WS] device " << device_id << " connection failed";
            report("offline");
        });

        websocketpp::lib::error_code ec;
        WsClient::connection_ptr     con = conn->client.get_connection(device->m_url, ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(error) << "[WS] device " << device_id << " bad URL " << device->m_url << ": " << ec.message();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_connections.erase(device_id);
            }
            if (on_status_changed)
                on_status_changed(device_id, "offline");
            return false;
        }
        conn->hdl = con->get_handle();
        conn->client.connect(con);
        conn->thread = std::thread([conn, device_id]() {
            try {
                conn->client.run();
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(error) << "[WS] device " << device_id << " client thread: " << e.what();
            }
            conn->open = false;
        });
        BOOST_LOG_TRIVIAL(info) << "[WS] connecting to " << device->m_name << " at " << device->m_url;
        return true;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "[WS] device " << device_id << " connect: " << e.what();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_connections.erase(device_id);
        }
        if (on_status_changed)
            on_status_changed(device_id, "offline");
        return false;
    }
}

void QIDIDeviceWebSocket::stopConnection(const std::string &device_id)
{
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_connections.find(device_id);
        if (it == m_connections.end())
            return;
        conn = it->second;
        m_connections.erase(it);
    }
    conn->stopping = true;

    try {
        websocketpp::lib::error_code ec;
        if (conn->open)
            conn->client.close(conn->hdl, websocketpp::close::status::going_away, "Connection stopped", ec);
        conn->client.stop();
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "[WS] device " << device_id << " stop: " << e.what();
    }

    if (conn->thread.joinable()) {
        // A handler on the connection's own thread may end up here: it can't wait for itself.
        if (conn->thread.get_id() == std::this_thread::get_id())
            conn->thread.detach();
        else
            conn->thread.join();
    }
    BOOST_LOG_TRIVIAL(info) << "[WS] device " << device_id << " connection closed";
}

bool QIDIDeviceWebSocket::send(const std::string &device_id, const json &msg)
{
    std::shared_ptr<Connection> conn = find(device_id);
    if (!conn || !conn->open)
        return false;
    try {
        std::lock_guard<std::mutex>  lock(conn->send_mutex);
        websocketpp::lib::error_code ec;
        conn->client.send(conn->hdl, msg.dump(), websocketpp::frame::opcode::text, ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(warning) << "[WS] device " << device_id << " send failed: " << ec.message();
            return false;
        }
        return true;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "[WS] device " << device_id << " send: " << e.what();
        return false;
    }
}

void QIDIDeviceWebSocket::sendSubscribeMessage(const std::string &device_id)
{
    send(device_id, { { "jsonrpc", "2.0" },
                      { "id", request_id(device_id) },
                      { "method", "printer.objects.subscribe" },
                      { "params", { { "objects", subscribe_objects() } } } });
}

void QIDIDeviceWebSocket::getAllErrorList(const std::string &device_id)
{
    sendCommand(device_id, "method", "get_all_error_list", "server.extensions.request");
}

void QIDIDeviceWebSocket::sendCommand(const std::string &device_id, const std::string &script)
{
    send(device_id, { { "jsonrpc", "2.0" },
                      { "id", request_id(device_id) },
                      { "method", "printer.gcode.script" },
                      { "params", { { "script", script } } } });
}

bool QIDIDeviceWebSocket::sendCommand(const std::string &device_id, const std::string &param_name, const std::string &value,
                                      const std::string &method)
{
    return send(device_id, { { "jsonrpc", "2.0" },
                             { "id", request_id(device_id) },
                             { "method", method },
                             { "params", { { param_name, value } } } });
}

bool QIDIDeviceWebSocket::sendCommand(const std::string &device_id, const json &params, const std::string &method)
{
    return send(device_id, { { "jsonrpc", "2.0" }, { "id", request_id(device_id) }, { "method", method }, { "params", params } });
}

void QIDIDeviceWebSocket::sendActionCommand(const std::string &device_id, const std::string &action_type)
{
    send(device_id, { { "jsonrpc", "2.0" }, { "id", request_id(device_id) }, { "method", "printer.print." + action_type } });
}

}} // namespace Slic3r::GUI
