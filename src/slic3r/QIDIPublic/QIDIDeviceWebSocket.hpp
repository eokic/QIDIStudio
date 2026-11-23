// Public stand-in for QIDI's unpublished src/slic3r/QIDI/QIDIDeviceWebSocket.
// Used when QDT_RELEASE_TO_PUBLIC=0.
//
// Keeps one Moonraker websocket (ws://<printer>:7125/websocket) per LAN printer for the Device
// tab. It is rebuilt from the websocket code QIDI Studio had inside QDSDeviceManager before
// QIDI moved it to the private module (commit c07a89e), with the interface the manager uses now.
#ifndef slic3r_QIDIPublic_QIDIDeviceWebSocket_hpp_
#define slic3r_QIDIPublic_QIDIDeviceWebSocket_hpp_

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI {

class QDSDevice;

class QIDIDeviceWebSocket
{
public:
    QIDIDeviceWebSocket();
    ~QIDIDeviceWebSocket();

    QIDIDeviceWebSocket(const QIDIDeviceWebSocket &) = delete;
    QIDIDeviceWebSocket &operator=(const QIDIDeviceWebSocket &) = delete;

    // Called from the connection's own thread. Status is "connected" or "offline".
    std::function<void(const std::string &device_id, const std::string &status)>     on_status_changed;
    std::function<void(const std::string &device_id, const nlohmann::json &message)> on_message_received;

    // Replaces any connection the device already has. False if the device or its URL is unusable.
    bool connect(const std::string &device_id, const std::shared_ptr<QDSDevice> &device);
    void stopConnection(const std::string &device_id);

    void sendSubscribeMessage(const std::string &device_id);
    void getAllErrorList(const std::string &device_id);
    // printer.gcode.script
    void sendCommand(const std::string &device_id, const std::string &script);
    // {"method": method, "params": {param_name: value}}
    bool sendCommand(const std::string &device_id, const std::string &param_name, const std::string &value, const std::string &method);
    // {"method": method, "params": params}
    bool sendCommand(const std::string &device_id, const nlohmann::json &params, const std::string &method);
    // printer.print.<action_type> (pause, resume, cancel, ...)
    void sendActionCommand(const std::string &device_id, const std::string &action_type);

    std::vector<std::string> getConnectionIds();

private:
    struct Connection;

    std::shared_ptr<Connection> find(const std::string &device_id);
    bool send(const std::string &device_id, const nlohmann::json &msg);

    std::mutex                                                   m_mutex;
    std::unordered_map<std::string, std::shared_ptr<Connection>> m_connections;
};

}} // namespace Slic3r::GUI

#endif
