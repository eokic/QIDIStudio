#include "LanPrinter.hpp"

#include <boost/algorithm/string.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>

namespace Slic3r {

using json = nlohmann::json;

namespace {

constexpr int k_box_slots = 16; // up to 4 Boxes with 4 slots; the external spool comes after them

// Klipper reports a Box slot's runout sensor per slot; the query asks for all of them.
std::string box_query_path()
{
    std::string path = "/printer/objects/query?save_variables=variables&multi_color_controller";
    for (int i = 0; i < k_box_slots; ++i)
        path += "&box_stepper%20slot" + std::to_string(i) + "=runout_button";
    return path;
}

json variables_of(const json &status)
{
    if (status.contains("save_variables") && status["save_variables"].is_object())
        return status["save_variables"].value("variables", json::object());
    return json::object();
}

int box_count_of(const json &status)
{
    const json vars  = variables_of(status);
    int        count = vars.value("box_count", 0);
    if (status.contains("multi_color_controller") && status["multi_color_controller"].is_object()) {
        const json &mcc = status["multi_color_controller"];
        if (mcc.contains("box_count") && mcc["box_count"].is_number())
            count = mcc["box_count"].get<int>();
        else if (mcc.contains("box") && mcc["box"].is_object() && mcc["box"].contains("box_count") && mcc["box"]["box_count"].is_number())
            count = mcc["box"]["box_count"].get<int>();
    }
    return std::max(0, std::min(count, k_box_slots / 4));
}

int int_value(const json &obj, const std::string &key, int fallback = 0)
{
    if (!obj.is_object() || !obj.contains(key))
        return fallback;
    const json &v = obj[key];
    if (v.is_number())
        return v.get<int>();
    if (v.is_string()) {
        try {
            return std::stoi(v.get<std::string>());
        } catch (...) {}
    }
    return fallback;
}

// officiall_filas_list.cfg (sic): [colordict] index = hex colour, [filaN] filament = type name.
void parse_filament_list(const std::string &content, std::map<int, std::string> &colors, std::map<int, std::string> &types)
{
    std::istringstream in(content);
    std::string        line, section;
    while (std::getline(in, line)) {
        boost::trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line[0] == '[') {
            section = line.size() > 2 && line.back() == ']' ? line.substr(1, line.size() - 2) : std::string();
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = boost::trim_copy(line.substr(0, eq)), value = boost::trim_copy(line.substr(eq + 1));
        try {
            if (section == "colordict")
                colors[std::stoi(key)] = value;
            else if (key == "filament" && section.size() > 4 && section.compare(0, 4, "fila") == 0)
                types[std::stoi(section.substr(4))] = value;
        } catch (...) {}
    }
}

bool ok(const LanReply &r) { return r.error.empty() && r.status >= 200 && r.status < 300; }

// "connection refused", or "HTTP 404" when the answer was an HTTP error.
std::string failure_text(const LanReply &r)
{
    if (!r.error.empty())
        return r.error + (r.status ? " (HTTP " + std::to_string(r.status) + ")" : std::string());
    return "HTTP " + std::to_string(r.status);
}

} // namespace

std::string normalize_filament_type(const std::string &type)
{
    const std::string u = boost::to_upper_copy(boost::trim_copy(type));
    for (const char *k : {"PETG", "PLA", "ABS", "TPU", "ASA", "PVA"})
        if (u.find(k) != std::string::npos)
            return k;
    if (u.find("PA") != std::string::npos || u.find("NYLON") != std::string::npos)
        return "PA";
    if (u.find("PC") != std::string::npos)
        return "PC";
    return u;
}

std::string box_slot_label(int index) { return "Box " + std::to_string(index / 4 + 1) + " slot " + std::to_string(index % 4 + 1); }

LanPrinter::LanPrinter(std::string name, std::string host, std::string api_key, LanTransport transport)
    : m_name(std::move(name)), m_host(std::move(host)), m_api_key(std::move(api_key)), m_transport(std::move(transport))
{}

std::string LanPrinter::base_url() const
{
    std::string url = boost::trim_copy(m_host);
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    if (!boost::istarts_with(url, "http://") && !boost::istarts_with(url, "https://"))
        url = "http://" + url;
    return url;
}

std::string LanPrinter::bare_host() const
{
    std::string h = boost::trim_copy(m_host);
    for (const char *p : {"http://", "https://"})
        if (boost::istarts_with(h, p))
            h = h.substr(std::strlen(p));
    if (auto s = h.find('/'); s != std::string::npos)
        h.resize(s);
    if (auto c = h.find(':'); c != std::string::npos)
        h.resize(c);
    return h;
}

// Sends req. Throws NoAnswer, or Refused with the printer's own message when its error reply has one.
LanReply LanPrinter::send(LanRequest req, const std::string &what)
{
    LanReply r = m_transport(req);
    if (ok(r))
        return r;
    std::string message;
    const json  j = json::parse(r.body, nullptr, false);
    if (j.is_object() && j.contains("error") && j["error"].is_object() && j["error"].contains("message") && j["error"]["message"].is_string())
        message = j["error"]["message"].get<std::string>();
    if (!message.empty())
        throw LanPrinterError(LanPrinterError::Kind::Refused, "The printer '" + m_name + "' refused " + what + ": " + message, message,
                              r.status);
    throw LanPrinterError(LanPrinterError::Kind::NoAnswer,
                          "The printer '" + m_name + "' did not answer " + what + ": " + failure_text(r) + ". Is it on and on the network?",
                          failure_text(r), r.status);
}

std::string LanPrinter::get(const std::string &path)
{
    LanRequest req;
    req.url     = base_url() + path;
    req.api_key = m_api_key;
    return send(std::move(req), "'" + path.substr(0, path.find('?')) + "'").body;
}

void LanPrinter::post(const std::string &path, const std::string &json_body, long timeout_s, const std::string &what)
{
    LanRequest req;
    req.method    = "POST";
    req.url       = base_url() + path;
    req.api_key   = m_api_key;
    req.json_body = json_body;
    req.timeout_s = timeout_s;
    send(std::move(req), what);
}

// Parses a Moonraker reply and returns what read() gives. A reply that is not JSON, or JSON of another shape, is a
// BadReply, so a type error in a printer's answer is not taken for a bad tool argument.
template<class Read> static auto read_reply(const std::string &name, const std::string &body, const std::string &what, Read read)
{
    try {
        const json j = json::parse(body);
        return read(j);
    } catch (const json::exception &e) {
        throw LanPrinterError(LanPrinterError::Kind::BadReply,
                              "The printer '" + name + "' sent an answer to " + what + " that QIDI Studio cannot read.", e.what());
    }
}

LanPrintStatus LanPrinter::status()
{
    const std::string path = "/printer/objects/query?print_stats&display_status&extruder=temperature,target"
                             "&heater_bed=temperature,target&virtual_sdcard=progress,is_active";
    return read_reply(m_name, get(path), "the status query", [](const json &j) {
        const json    &st = j.at("result").at("status");
        LanPrintStatus s;
        if (st.contains("print_stats")) {
            const json &ps    = st["print_stats"];
            s.has_print_stats = true;
            s.state           = ps.value("state", "");
            s.file            = ps.value("filename", "");
            s.message         = ps.value("message", "");
            s.printing_s      = int(ps.value("print_duration", 0.0));
            const json layer  = ps.value(json::json_pointer("/info/current_layer"), json());
            if (layer.is_number())
                s.layer = layer.get<double>();
        }
        if (st.contains("virtual_sdcard"))
            s.progress_pct = int(std::lround(100.0 * st["virtual_sdcard"].value("progress", 0.0)));
        for (auto [key, heater] : {std::pair{"extruder", &s.extruder}, std::pair{"heater_bed", &s.bed}})
            if (st.contains(key))
                *heater = LanHeater{std::lround(st[key].value("temperature", 0.0)), std::lround(st[key].value("target", 0.0))};
        return s;
    });
}

std::vector<LanConsoleLine> LanPrinter::console(int count)
{
    return read_reply(m_name, get("/server/gcode_store?count=" + std::to_string(count)), "the console query", [](const json &j) {
        std::vector<LanConsoleLine> lines;
        for (const json &m : j.value(json::json_pointer("/result/gcode_store"), json::array()))
            lines.push_back({m.value("time", 0.0), m.value("type", ""), m.value("message", "")});
        return lines;
    });
}

void LanPrinter::gcode(const std::string &line, long timeout_s)
{
    post("/printer/gcode/script", json{{"script", line}}.dump(), timeout_s, "'" + line + "'");
}

void LanPrinter::print_action(const std::string &action, long timeout_s) { post("/printer/print/" + action, "{}", timeout_s, "'" + action + "'"); }

std::vector<LanWebcam> LanPrinter::webcams()
{
    return read_reply(m_name, get("/server/webcams/list"), "the webcam list", [](const json &j) {
        std::vector<LanWebcam> cams;
        for (const json &c : j.at("result").value("webcams", json::array()))
            cams.push_back({c.value("name", ""), c.value("snapshot_url", ""), c.value("enabled", true)});
        return cams;
    });
}

// A relative snapshot URL is on the printer's web server (port 80), as on QIDI Studio's device page
// (QDSPrinterWebView.cpp: <printer>/webcam/?action=snapshot). An absolute URL on localhost is written as the printer
// sees itself, so it gets the printer's host.
std::string LanPrinter::snapshot(const std::string &snapshot_url, size_t size_limit)
{
    std::string url = snapshot_url;
    if (!boost::istarts_with(url, "http://") && !boost::istarts_with(url, "https://")) {
        url = "http://" + bare_host() + (!url.empty() && url.front() == '/' ? "" : "/") + url;
    } else {
        const size_t start = url.find("://") + 3;
        const bool   ipv6  = start < url.size() && url[start] == '[';
        size_t       end   = ipv6 ? url.find(']', start) : url.find_first_of(":/?#", start);
        end                = end == std::string::npos ? url.size() : end + (ipv6 ? 1 : 0);
        const std::string host = url.substr(start, end - start);
        if (boost::iequals(host, "localhost") || boost::starts_with(host, "127.") || host == "[::1]")
            url.replace(start, end - start, bare_host());
    }
    LanRequest req;
    req.url        = url; // the camera needs no API key, and an absolute URL can point to another host
    req.size_limit = size_limit;
    const LanReply r = m_transport(req);
    if (!ok(r))
        throw LanPrinterError(LanPrinterError::Kind::NoAnswer, "The camera did not send a picture: " + failure_text(r), failure_text(r), r.status);
    return r.body;
}

int LanPrinter::box_count()
{
    return read_reply(m_name, get(box_query_path()), "the Box query", [](const json &j) { return box_count_of(j.at("result").at("status")); });
}

LanBox LanPrinter::box()
{
    std::vector<int> colour_index(k_box_slots + 1, -1); // older firmware: an index into the filament list's colours
    LanBox box = read_reply(m_name, get(box_query_path()), "the Box query", [&colour_index](const json &j) {
        const json &status = j.at("result").at("status");
        const json  vars   = variables_of(status);
        LanBox      b;
        b.box_count = box_count_of(status);
        b.auto_reload_detect =
            status.contains("multi_color_controller") && status["multi_color_controller"].is_object() ?
                int_value(status["multi_color_controller"], "auto_reload_detect", int_value(vars, "auto_reload_detect")) :
                int_value(vars, "auto_reload_detect");
        b.slots.resize(k_box_slots + 1);
        for (int i = 0; i < k_box_slots + 1; ++i) {
            const std::string s    = std::to_string(i);
            LanBoxSlot       &slot = b.slots[i];
            if (i < k_box_slots) {
                const std::string key = "box_stepper slot" + s;
                if (i < b.box_count * 4 && status.contains(key) && status[key].contains("runout_button") &&
                    status[key]["runout_button"].is_number())
                    slot.loaded = status[key]["runout_button"].get<int>() == 0;
            } else {
                // External spool: there is no sensor to read, so it counts as loaded when it has a filament set.
                slot.loaded = vars.contains("filament_slot" + s);
            }
            slot.qidi           = int_value(vars, "vendor_slot" + s) == 1;
            slot.filament_index = int_value(vars, "filament_slot" + s);
            // Newer firmware stores the material name and a hex colour list; older firmware stores list indexes.
            const json colour = vars.value("color_slot" + s, json());
            if (colour.is_array() && !colour.empty() && colour[0].is_string())
                slot.colour = colour[0].get<std::string>();
            else if (colour.is_number())
                colour_index[i] = colour.get<int>();
            if (i < k_box_slots && status.contains("multi_color_controller")) {
                const json::json_pointer type_ptr("/multi_color_controller/box/box" + std::to_string(i / 4) + "_filament_info_" +
                                                  std::to_string(i % 4) + "_filament_type");
                if (status.contains(type_ptr) && status[type_ptr].is_string())
                    slot.type = status[type_ptr].get<std::string>();
            }
        }
        return b;
    });

    // Older firmware: the type and the colour are indexes into the printer's filament list.
    std::map<int, std::string> colors, types;
    try {
        parse_filament_list(get("/server/files/config/officiall_filas_list.cfg"), colors, types);
    } catch (const LanPrinterError &e) {
        box.filament_list_error = e.detail;
    }
    for (size_t i = 0; i < box.slots.size(); ++i) {
        LanBoxSlot &slot = box.slots[i];
        if (slot.type.empty())
            if (auto it = types.find(slot.filament_index); it != types.end())
                slot.type = it->second;
        if (slot.colour.empty())
            if (auto it = colors.find(colour_index[i]); it != colors.end())
                slot.colour = it->second;
    }
    return box;
}

std::vector<std::string> LanPrinter::start_print(const LanStartPrint &job)
{
    std::vector<std::string> warnings;

    // 1. Check the Box mapping against what is loaded, before anything is sent.
    if (job.use_box) {
        const LanBox b     = box();
        const int    slots = b.box_count * 4;
        if (slots == 0)
            throw std::invalid_argument("use_box is on but the printer reports no QIDI Box");
        for (int f : job.used_filaments) {
            if (f < 1 || f > int(job.box_slots.size()) || job.box_slots[f - 1] < 1)
                throw std::invalid_argument("Filament " + std::to_string(f) +
                                            " is used on this plate but has no Box slot. Give box_slots with one slot per filament "
                                            "(see get_box_slots).");
            const int s = job.box_slots[f - 1];
            if (s > slots)
                throw std::invalid_argument("Box slot " + std::to_string(s) + " does not exist");
            const LanBoxSlot &bs = b.slots[size_t(s - 1)];
            if (!bs.loaded)
                throw std::invalid_argument(box_slot_label(s - 1) + " is empty (mapped to filament " + std::to_string(f) + ")");
            const std::string want = f <= int(job.filament_types.size()) ? normalize_filament_type(job.filament_types[f - 1]) : "";
            if (want.empty())
                continue;
            if (bs.type.empty())
                warnings.push_back(box_slot_label(s - 1) + ": " +
                                   (b.filament_list_error.empty() ?
                                        std::string("the printer does not report its filament type") :
                                        "the printer's filament list could not be read (" + b.filament_list_error + ")") +
                                   ", so it is not checked against " + want + " (filament " + std::to_string(f) + ")");
            else if (normalize_filament_type(bs.type) != want)
                warnings.push_back("Filament " + std::to_string(f) + " is " + want + " but " + box_slot_label(s - 1) + " holds " + bs.type);
        }
    }

    // 2. Print options, the same commands as QIDI Studio's Print dialog in LAN mode (SelectMachine.cpp).
    if (job.box_printer || job.use_box) {
        gcode(std::string("SAVE_VARIABLE VARIABLE=enable_box VALUE=") + (job.use_box ? "1" : "0"));
        if (job.use_box)
            for (int f : job.used_filaments)
                // SAVE_VARIABLE reads VALUE as a Python literal, so the slot name keeps its own quotes.
                gcode("SAVE_VARIABLE VARIABLE=value_t" + std::to_string(f - 1) + " VALUE=\"'slot" + std::to_string(job.box_slots[f - 1] - 1) +
                      "'\"");
    }
    gcode(job.bed_leveling ? "G31" : "G32");
    // The Print dialog sends timelapse and the cooler only when the printer's preset supports them.
    if (job.timelapse && !job.supports_timelapse) {
        if (*job.timelapse)
            warnings.push_back("This printer has no timelapse, so timelapse was not set");
    } else if (job.timelapse) {
        LanRequest req;
        req.method    = "POST";
        req.url       = "http://" + bare_host() + "/machine/timelapse/settings?enabled=" + (*job.timelapse ? "true" : "false");
        req.api_key   = m_api_key;
        req.json_body = "{}";
        const LanReply r = m_transport(req);
        if (!ok(r))
            warnings.push_back("could not set timelapse: " + failure_text(r));
    }
    if (job.cooler && !job.supports_cooler) {
        if (*job.cooler)
            warnings.push_back("This printer has no polar cooler, so the cooler was not set");
    } else if (job.cooler) {
        gcode(std::string("SAVE_VARIABLE VARIABLE=enable_polar_cooler VALUE=") + (*job.cooler ? "1" : "0"));
    }

    // 3. Upload, which starts the print.
    LanRequest req;
    req.method    = "POST";
    req.url       = base_url() + "/server/files/upload";
    req.api_key   = m_api_key;
    req.form      = {{"root", "gcodes"}, {"print", "true"}, {"plateindex", std::to_string(job.plate)}};
    req.file_path = job.file_path;
    req.file_name = job.file_name;
    req.timeout_s = 600;
    send(std::move(req), "the upload of " + job.file_name);
    return warnings;
}

} // namespace Slic3r
