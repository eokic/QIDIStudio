// A LAN printer (see GLOSSARY.md): a QIDI printer that QIDI Studio talks to directly over the local network,
// through its Moonraker and QIDI's additions to it (the Box, timelapse). The MCP printer tools and the public
// QIDIDeviceApi use this module; the Print dialog (SelectMachine.cpp) keeps its own copy of the start-print commands.
//
// It is not built on upstream OctoPrint (OctoPrint.cpp): OctoPrint::send_command_to_printer joins the JSON by hand
// with no escaping (the Box slot command has quotes), returns only a bool and puts the error text in the log, sets
// only a connect timeout, and OctoPrint::upload reads the current plate from the GUI.
//
// No GUI types and no curl here: every request goes through a LanTransport, so tests give recorded printer replies.
#ifndef slic3r_Utils_LanPrinter_hpp_
#define slic3r_Utils_LanPrinter_hpp_

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

struct LanRequest
{
    std::string method = "GET"; // GET or POST
    std::string url;
    std::string api_key;        // sent as X-Api-Key when not empty
    std::string json_body;      // POST body with Content-Type: application/json, when not empty
    std::vector<std::pair<std::string, std::string>> form; // multipart fields, when not empty
    std::string file_path, file_name;                     // multipart field "file", when file_path is not empty
    long        timeout_s  = 10;
    size_t      size_limit = 0; // largest reply body in bytes; 0 = no limit
};

struct LanReply
{
    unsigned    status = 0; // HTTP status; 0 when no answer came
    std::string body;
    std::string error;      // the transport's error text; empty when an answer came
};

// The seam: sends one request and waits for the reply. Production: lan_http_transport() (LanPrinterHttp.cpp, curl).
using LanTransport = std::function<LanReply(const LanRequest &)>;
// With a stop check: when it gives true, a request that waits ends within about 1 s (curl asks about once a second
// while no data comes), and a later request does not go out. Both get the error "stopped".
LanTransport lan_http_transport(std::function<bool()> stop = {});

// what() is a message for the user, in English, with the printer's name.
class LanPrinterError : public std::runtime_error
{
public:
    enum class Kind {
        NoAnswer, // no answer, a timeout, or an HTTP error with no message from the printer
        Refused,  // the printer answered with its own error message
        BadReply, // an answer that is not what the printer should send
    };
    LanPrinterError(Kind kind, const std::string &message, std::string detail = {}, unsigned http_status = 0)
        : std::runtime_error(message), kind(kind), detail(std::move(detail)), http_status(http_status)
    {}
    Kind        kind;
    std::string detail;      // the transport error or the printer's message alone
    unsigned    http_status; // 0 when no answer came
};

struct LanHeater
{
    long temp   = 0;
    long target = 0;
};

struct LanPrintStatus
{
    bool                     has_print_stats = false;
    std::string              state;   // standby, printing, paused, complete, cancelled, error
    std::string              file, message;
    int                      printing_s = 0;
    std::optional<double>    layer;   // current layer, when the print reports one
    std::optional<int>       progress_pct;
    std::optional<LanHeater> extruder, bed;
};

struct LanConsoleLine
{
    double      time = 0; // on the printer's clock
    std::string type;     // "command" or "response"
    std::string message;
};

struct LanWebcam
{
    std::string name, snapshot_url;
    bool        enabled = true;
};

struct LanBoxSlot
{
    bool        loaded         = false;
    bool        qidi           = false; // QIDI filament, else generic
    int         filament_index = 0;     // index in the printer's filament list
    std::string type;                   // empty when the printer does not report it
    std::string colour;                 // hex colour, empty when not known
};

struct LanBox
{
    int                     box_count          = 0;
    int                     auto_reload_detect = 0;
    std::vector<LanBoxSlot> slots;               // 16 Box slots (Box 1 is 0..3), then the external spool
    std::string             filament_list_error; // set when the printer's filament list could not be read
};

// One plate to print, and what the target printer's preset says it supports.
struct LanStartPrint
{
    std::string              file_path;      // the .gcode.3mf on this computer
    std::string              file_name;      // its name on the printer
    int                      plate = 1;      // 1-based plate in the file
    std::vector<int>         used_filaments; // 1-based filaments that the plate uses
    std::vector<std::string> filament_types; // type of each filament from its preset (index 0 = filament 1)
    bool                     use_box = false;
    std::vector<int>         box_slots;      // Box slot (1-based) of each filament (index 0 = filament 1)
    bool                     bed_leveling = true;
    std::optional<bool>      timelapse, cooler; // not set: the printer keeps its setting
    bool                     box_printer        = false;
    bool                     supports_timelapse = false;
    bool                     supports_cooler    = false;
};

// "PLA", "PETG", ... from a preset's or a slot's filament type, to compare them.
std::string normalize_filament_type(const std::string &type);
// "Box 2 slot 3" for the 0-based slot index 6.
std::string box_slot_label(int index);

class LanPrinter
{
public:
    // name: for messages. host: the address as QIDI Studio stores it ("192.168.1.20", "http://192.168.1.20:7125/").
    LanPrinter(std::string name, std::string host, std::string api_key, LanTransport transport);

    const std::string &name() const { return m_name; }

    // Each call throws LanPrinterError when the printer does not answer or refuses.
    LanPrintStatus              status();
    std::vector<LanConsoleLine> console(int count); // the last lines, oldest first
    void                        gcode(const std::string &line, long timeout_s = 30);
    void                        print_action(const std::string &action, long timeout_s); // pause, resume, cancel
    std::vector<LanWebcam>      webcams();
    // The picture behind a webcam's snapshot URL, at most size_limit bytes.
    std::string                 snapshot(const std::string &snapshot_url, size_t size_limit);
    int                         box_count();
    LanBox                      box();
    // The body of a GET of path on the printer.
    std::string                 get(const std::string &path);

    // Checks the Box mapping, sets the print options and uploads the file, which starts the print. Returns the warnings.
    // Throws std::invalid_argument, before anything is sent, when the Box mapping does not fit what is loaded.
    std::vector<std::string> start_print(const LanStartPrint &job);

private:
    std::string base_url() const;
    std::string bare_host() const; // no scheme, port or path: QIDI's own endpoints are on port 80
    LanReply    send(LanRequest req, const std::string &what);
    void        post(const std::string &path, const std::string &json_body, long timeout_s, const std::string &what);

    std::string  m_name, m_host, m_api_key;
    LanTransport m_transport;
};

} // namespace Slic3r

#endif
