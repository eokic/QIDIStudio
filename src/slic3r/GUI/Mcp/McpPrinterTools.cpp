// MCP tools for the printer side: printers set up in QIDI Studio, their status, the QIDI Box slots,
// Box actions and sending a sliced plate to print. The printer requests are in LanPrinter (slic3r/Utils);
// these tools read the arguments, package the plate on the GUI thread and shape the replies.
// A LanPrinterError reaches the assistant as a tool error with its message (McpServer: Server::tools_call).
#include "McpArgs.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Jobs/PrintJob.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/LanPrinter.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>

namespace Slic3r { namespace GUI { namespace Mcp {

using json = nlohmann::json;
namespace fs = boost::filesystem;

// ---------------------------------------------------------------------------------------------------------------
// Printers

struct PrinterHost
{
    std::string name;   // physical printer name in QIDI Studio
    std::string host;   // print_host as entered
    std::string apikey;
    std::string preset; // printer model it was set up with ("Q2"), stored as preset_name
    // A stop of the MCP server ends a request that waits for the printer: the GUI thread waits for the tool calls.
    LanPrinter lan() const { return LanPrinter(name, host, apikey, lan_http_transport(&stopping)); }
};

// The network printers set up in QIDI Studio.
static std::vector<PrinterHost> printers()
{
    auto out = std::make_shared<std::vector<PrinterHost>>();
    run_on_ui([out] {
        for (const PhysicalPrinter &pp : wxGetApp().preset_bundle->physical_printers) {
            PrinterHost p{pp.name, pp.config.opt_string("print_host"), pp.config.opt_string("printhost_apikey"),
                          pp.config.opt_string("preset_name")};
            if (!p.host.empty()) out->push_back(std::move(p));
        }
        return json();
    });
    return *out;
}

// ---------------------------------------------------------------------------------------------------------------
// Arguments: each one makes its schema entry (registration, below) and reads its value (the tools).

static const StringArg  k_printer{"printer", "Printer name from list_printers (optional when only one is set up)"};
static const EnumArg    k_box_action{"action", "", {"load", "unload", "eject", "read_rfid"}};
static const IntArg     k_box_slot{"slot", "Box slot; 1-4 are Box 1", 1, 16};
static const IntArg     k_print_plate{"plate", "Plate, 1-based; not given or 0: the current plate", 0, INT_MAX, 0};
static const IntListArg k_box_slots{"box_slots", "The Box slot (from get_box_slots) of each filament, in order; 0: a filament this plate does not use", 0, 16};
static const BoolArg    k_use_box{"use_box", "Print from the Box (default: true when box_slots is given)"};
static const BoolArg    k_bed_leveling{"bed_leveling", "Auto bed leveling before the print", true};
static const BoolArg    k_timelapse{"timelapse", "Not given: the printer keeps its setting"};
static const BoolArg    k_cooler{"cooler", "Polar cooler, on printers that have it. Not given: the printer keeps its setting"};
static const BoolArg    k_confirm{"confirm", "true only after the user agreed", std::nullopt, true};

static PrinterHost pick_printer(const json &args)
{
    const std::string              want = k_printer.read(args);
    const std::vector<PrinterHost> list = printers();
    if (list.empty())
        throw ToolError("No network printer is set up in QIDI Studio. Add the printer on the Device tab (or with the "
                        "connection button next to the printer preset) first.");
    if (want.empty()) {
        if (list.size() == 1) return list.front();
        json names = json::array();
        for (const PrinterHost &p : list) names.push_back(p.name);
        throw ToolError("Several printers are set up: name one with 'printer'.", names);
    }
    for (const PrinterHost &p : list)
        if (boost::iequals(p.name, want) || p.host == want) return p;
    throw ToolError("No printer named '" + want + "'. Call list_printers.");
}

// ---------------------------------------------------------------------------------------------------------------
// Tools

static ToolResult tool_list_printers(const json &)
{
    json list = json::array();
    for (const PrinterHost &p : printers()) list.push_back({{"name", p.name}, {"host", p.host}, {"preset", p.preset}});
    json out = {{"printers", list}};
    if (list.empty())
        out["note"] = "No network printer is set up. Add the Q2 in QIDI Studio (Device tab, or the connection button next to the printer preset).";
    return ToolResult::json(out);
}

static ToolResult tool_printer_status(const json &args)
{
    const PrinterHost    p   = pick_printer(args);
    const LanPrintStatus st  = p.lan().status();
    json                 out = {{"printer", p.name}};
    if (st.has_print_stats) {
        out["state"]      = st.state;
        out["file"]       = st.file;
        out["printing_s"] = st.printing_s;
        if (!st.message.empty()) out["message"] = st.message;
    }
    if (st.progress_pct) out["progress_pct"] = *st.progress_pct;
    if (st.extruder) out["extruder"] = {{"temp", st.extruder->temp}, {"target", st.extruder->target}};
    if (st.bed) out["heater_bed"] = {{"temp", st.bed->temp}, {"target", st.bed->target}};
    return ToolResult::json(out);
}

static ToolResult tool_get_box_slots(const json &args)
{
    const PrinterHost             p   = pick_printer(args);
    const LanBox                  box = p.lan().box();
    const std::vector<LanBoxSlot> slots(box.slots.begin(), box.slots.begin() + std::min<size_t>(box.box_count * 4, box.slots.size()));
    // Match each loaded slot to a compatible Generic system preset of the same type.
    json presets = run_on_ui([slots] {
        json m = json::array();
        for (const LanBoxSlot &s : slots) {
            std::string match;
            if (s.loaded && !s.type.empty()) // an empty slot, or one of a type the printer does not report, has no match
                for (const Preset &pr : wxGetApp().preset_bundle->filaments.get_presets()) {
                    if (!pr.is_visible || !pr.is_compatible || !pr.is_system || !boost::icontains(pr.name, "Generic")) continue;
                    const auto *t = pr.config.option<ConfigOptionStrings>("filament_type");
                    if (t && !t->values.empty() && normalize_filament_type(t->values.front()) == normalize_filament_type(s.type)) {
                        match = pr.name;
                        break;
                    }
                }
            m.push_back(match);
        }
        return m;
    });
    json list = json::array();
    for (size_t i = 0; i < slots.size(); ++i) {
        const LanBoxSlot &s = slots[i];
        json j = {{"slot", i + 1}, {"label", box_slot_label(int(i))}, {"loaded", s.loaded}};
        if (s.loaded) {
            j["type"]   = s.type; // empty: not known (see filament_list_error)
            j["vendor"] = s.qidi ? "QIDI" : "generic";
            if (!s.colour.empty()) j["colour"] = s.colour;
            if (!presets[i].get<std::string>().empty()) j["matching_preset"] = presets[i];
        }
        list.push_back(std::move(j));
    }
    json out = {{"printer", p.name}, {"boxes", slots.size() / 4}, {"slots", list},
                {"note", "slot numbers count across boxes: 1-4 are Box 1, 5-8 Box 2, and so on. "
                         "Pass them to send_print as box_slots, one per filament."}};
    if (!box.filament_list_error.empty())
        out["filament_list_error"] = "The printer's filament list could not be read, so slot types can be missing: " + box.filament_list_error;
    return ToolResult::json(out);
}

static ToolResult tool_box_action(const json &args)
{
    static const std::map<std::string, std::string> scripts = {
        {"load", "LOAD_FILAMENT_CONTROL"}, {"unload", "UNLOAD_FILAMENT_CONTROL"}, {"eject", "EJECT_FILAMENT_CONTROL"}, {"read_rfid", "READ_RFID"}};
    const std::string action = k_box_action.read(args);
    const int         slot   = k_box_slot.read(args);
    const auto        it     = scripts.find(action);
    const PrinterHost p = pick_printer(args);
    p.lan().gcode(it->second + " SLOT=" + std::to_string(slot - 1), 180);
    return ToolResult::json({{"printer", p.name}, {"done", action}, {"slot", slot}, {"label", box_slot_label(slot - 1)}});
}

// A 4K camera picture, as JPEG or PNG, is smaller than its raw RGB frame. The limit stops a stream URL from filling memory.
static constexpr size_t k_max_camera_picture = 3840 * 2160 * 3;

static ToolResult tool_camera_snapshot(const json &args)
{
    const PrinterHost p   = pick_printer(args);
    LanPrinter        lan = p.lan();
    std::string       name, url;
    for (const LanWebcam &c : lan.webcams())
        if (c.enabled) { name = c.name; url = c.snapshot_url; break; }
    if (url.empty()) throw ToolError("The printer '" + p.name + "' has no camera with a snapshot URL.");
    std::string picture;
    try {
        picture = lan.snapshot(url, k_max_camera_picture);
    } catch (const LanPrinterError &e) {
        throw ToolError(std::string(e.what()) +
                        (e.http_status >= 500 ? ". The camera service on the printer (crowsnest) may have failed to start: restart it, or the printer." :
                                                "."));
    }
    const char *mime = boost::starts_with(picture, "\xFF\xD8") ? "image/jpeg" : boost::starts_with(picture, "\x89PNG") ? "image/png" : nullptr;
    if (!mime) throw ToolError("The camera sent something that is not a JPEG or PNG picture.");
    ToolResult res = ToolResult::json({{"printer", p.name}, {"camera", name}});
    res.add_image(picture, mime);
    return res;
}

// ---------------------------------------------------------------------------------------------------------------
// Pause, resume and cancel: Moonraker's print endpoints, as QIDI Studio's Device tab uses them (QIDIDeviceWebSocket:
// printer.print.<action>). They run the printer's PAUSE, RESUME and CANCEL_PRINT macros; on the Q2, cancel first
// stops a heating wait.

// Klipper runs the macro after the print's current command, and bed leveling alone took about 6 minutes in a test print.
static constexpr long k_print_action_timeout_s = 600;

// Runs the action when the print is in one of the `from` states. The printer's RESUME macro can refuse with a
// console line only ("Please load filament"), so when the state did not change, the reply gives the console lines
// written since the request.
static json run_print_action(const PrinterHost &p, const std::string &action, std::initializer_list<const char *> from,
                             long timeout_s = k_print_action_timeout_s)
{
    LanPrinter        lan    = p.lan();
    const std::string before = lan.status().state;
    if (std::none_of(from.begin(), from.end(), [&before](const char *s) { return before == s; }))
        throw ToolError("Cannot " + action + ": the print state of '" + p.name + "' is '" + before + "'.");
    const std::vector<LanConsoleLine> last  = lan.console(1);
    const double                      since = last.empty() ? 0.0 : last.back().time;
    try {
        lan.print_action(action, timeout_s);
    } catch (const LanPrinterError &e) {
        // After a timeout or a lost connection the printer can still run the action: give the state it is in now.
        std::string now;
        try { now = lan.status().state; } catch (const LanPrinterError &) {}
        throw ToolError(std::string(e.what()) + (now.empty() ? "" : " The print state is now '" + now + "'."));
    }
    const std::string after = lan.status().state;
    json              out   = {{"printer", p.name}, {"state", after}};
    if (after != before) {
        out["done"] = action;
    } else {
        json said = json::array();
        for (const LanConsoleLine &m : lan.console(20))
            if (m.time > since && m.type == "response") said.push_back(m.message);
        out["note"]         = "The print is still " + before + " after the " + action + " request. printer_said has the printer's "
                              "console lines since the request. Call printer_status to check again before you repeat the request.";
        out["printer_said"] = said;
    }
    return out;
}

static ToolResult tool_pause_print(const json &args) { return ToolResult::json(run_print_action(pick_printer(args), "pause", {"printing"})); }

static ToolResult tool_resume_print(const json &args) { return ToolResult::json(run_print_action(pick_printer(args), "resume", {"paused"})); }

// True when layers print. The Q2 preset sets the layer to 0 after PRINT_START (heating, bed leveling), and to 1 at
// the first layer. Klipper clears the layer when a new print starts.
static bool printing_layers(const PrinterHost &p)
{
    try {
        const LanPrintStatus s = p.lan().status();
        return s.state == "printing" && s.layer && *s.layer >= 1;
    } catch (const LanPrinterError &e) {
        if (e.kind == LanPrinterError::Kind::BadReply) return false; // cancel at once
        throw;
    }
}

// A pause during layers took 3.3 s in a test. A longer pause waits behind a printer step, for example a heating wait
// between layers. After this time, the tool sends the cancel all the same. On the Q2, a cancel stops a heating wait.
static constexpr long k_pause_first_timeout_s = 60;

// The printer's CANCEL_PRINT does not pull the filament back. The hot nozzle leaks while it moves away and cools.
// PAUSE pulls the filament back 5 mm and parks the nozzle over the waste chute. While layers print, the tool pauses
// first. During heating and bed leveling, it cancels at once, because a pause waits until that step ends.
static ToolResult tool_cancel_print(const json &args)
{
    if (!k_confirm.read_if_given(args).value_or(false))
        throw ToolError("cancel_print ends the print for good. Ask the user, then call again with confirm: true.");
    const PrinterHost p            = pick_printer(args);
    bool              paused_first = false;
    std::string       pause_error;
    if (printing_layers(p)) {
        try {
            paused_first = run_print_action(p, "pause", {"printing"}, k_pause_first_timeout_s).contains("done");
            if (!paused_first) pause_error = "The print did not pause.";
        } catch (const std::exception &e) { // a ToolError or a LanPrinterError
            pause_error = e.what(); // Cancel all the same: the cancel checks the state again and gives its own error.
        }
    }
    json out            = run_print_action(p, "cancel", {"printing", "paused"});
    out["paused_first"] = paused_first;
    if (!pause_error.empty()) out["pause_error"] = pause_error;
    return ToolResult::json(out);
}

static std::string safe_file_name(std::string name)
{
    for (char &c : name)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    boost::trim(name);
    return name.empty() ? std::string("print") : name;
}

static ToolResult tool_send_print(const json &args)
{
    if (!k_confirm.read_if_given(args).value_or(false))
        throw ToolError("send_print starts a real print. Ask the user, then call again with confirm: true.");
    const PrinterHost p     = pick_printer(args);
    const int                 plate     = k_print_plate.read(args);
    const std::vector<int>    box_slots = k_box_slots.read(args); // per filament, 1-based slot
    const bool                use_box   = k_use_box.read_if_given(args).value_or(!box_slots.empty());
    const bool                leveling  = k_bed_leveling.read(args);
    const std::optional<bool> timelapse = k_timelapse.read_if_given(args); // not given: the printer keeps its setting
    const std::optional<bool> cooler    = k_cooler.read_if_given(args);

    // 1. Package the sliced plate as QIDI Studio's own Print dialog does.
    json pkg = run_on_ui(
        [plate, target_model = p.preset] {
            Plater        *plater = wxGetApp().plater();
            PartPlateList &pl     = plater->get_partplate_list();
            const int      idx    = plate == 0 ? pl.get_curr_plate_index() : plate - 1;
            if (idx < 0 || idx >= pl.get_plate_count()) throw ToolError("no such plate");
            if (idx != pl.get_curr_plate_index()) plater->select_plate(idx);
            PartPlate *pp = pl.get_curr_plate();
            if (!pp->is_slice_result_valid()) throw ToolError("Plate " + std::to_string(idx + 1) + " is not sliced (or changed since). Call slice first.");
            const DynamicPrintConfig &printer = wxGetApp().preset_bundle->printers.get_edited_preset().config;
            if (!printer.has("is_support_3mf") || !printer.opt_bool("is_support_3mf"))
                throw ToolError("This printer preset does not take .gcode.3mf prints; use export_gcode instead.");
            if (plater->send_gcode(idx) < 0) throw ToolError("QIDI Studio could not package the print. Slice again.");
            PrintPrepareData data;
            plater->get_print_job_data(&data);
            if (data._3mf_path.empty() || !fs::exists(data._3mf_path)) throw ToolError("the print file was not written");
            // Our own copy, so a later slice cannot replace the file mid-upload.
            fs::path dir = fs::temp_directory_path() / "qidistudio-mcp";
            fs::create_directories(dir);
            const std::string file = safe_file_name(into_u8(plater->get_project_name())) + ".gcode.3mf";
            fs::path copy = dir / file;
            fs::copy_file(data._3mf_path, copy, fs::copy_option::overwrite_if_exists);
            json used = json::array();
            for (int f : pp->get_used_filaments()) used.push_back(f);
            json types = json::array();
            PresetBundle *b = wxGetApp().preset_bundle;
            for (const std::string &fp : b->filament_presets) {
                const Preset *pr = b->filaments.find_preset(fp, false);
                const auto   *t  = pr ? pr->config.option<ConfigOptionStrings>("filament_type") : nullptr;
                types.push_back(t && !t->values.empty() ? t->values.front() : "");
            }
            const bool box_printer = printer.has("is_support_multi_box") && printer.opt_bool("is_support_multi_box");
            // Timelapse and the cooler follow the target printer's model, found as the Print dialog finds it
            // (SelectMachineDialog::update_option_opts): the first printer preset whose printer_model contains the
            // physical printer's model. With none, the selected preset.
            auto alnum_lower = [](const std::string &s) {
                std::string out;
                for (unsigned char c : s)
                    if (std::isalnum(c)) out += char(std::tolower(c));
                return out;
            };
            const DynamicPrintConfig *tc = &printer;
            if (const std::string model = alnum_lower(target_model); !model.empty())
                for (const Preset &pr : b->printers.get_presets())
                    if (pr.config.has("printer_model") && alnum_lower(pr.config.opt_string("printer_model")).find(model) != std::string::npos) {
                        tc = &pr.config;
                        break;
                    }
            auto supports = [tc](const char *key) { return tc->has(key) && tc->opt_bool(key); };
            return json{{"path", copy.string()},  {"file", file},
                        {"plate", idx + 1},       {"used_filaments", used},
                        {"filament_types", types}, {"box_printer", box_printer},
                        {"supports_timelapse", supports("is_support_timelapse")},
                        {"supports_cooler", supports("is_support_polar_cooler")}};
        },
        180);

    // 2. Check the Box mapping, set the print options and upload, which starts the print.
    LanStartPrint job;
    job.file_path          = pkg["path"].get<std::string>();
    job.file_name          = pkg["file"].get<std::string>();
    job.plate              = pkg["plate"].get<int>();
    job.used_filaments     = pkg["used_filaments"].get<std::vector<int>>();
    job.filament_types     = pkg["filament_types"].get<std::vector<std::string>>();
    job.use_box            = use_box;
    job.box_slots          = box_slots;
    job.bed_leveling       = leveling;
    job.timelapse          = timelapse;
    job.cooler             = cooler;
    job.box_printer        = pkg["box_printer"].get<bool>();
    job.supports_timelapse = pkg["supports_timelapse"].get<bool>();
    job.supports_cooler    = pkg["supports_cooler"].get<bool>();
    std::vector<std::string> warnings;
    try {
        warnings = p.lan().start_print(job);
    } catch (const std::invalid_argument &e) { // a Box mapping that does not fit; nothing was sent
        throw ToolError(e.what());
    }

    json out = {{"printer", p.name}, {"sent", job.file_name}, {"plate", job.plate}, {"use_box", use_box}, {"bed_leveling", leveling}};
    if (use_box) {
        json map = json::array();
        for (int f : job.used_filaments)
            map.push_back({{"filament", f}, {"slot", box_slots[f - 1]}, {"label", box_slot_label(box_slots[f - 1] - 1)}});
        out["box_mapping"] = map;
    }
    if (!warnings.empty()) out["warnings"] = warnings;
    out["note"] = "The printer starts the print now. Use printer_status to follow it.";
    return ToolResult::json(out);
}

// ---------------------------------------------------------------------------------------------------------------

void register_printer_tools(std::vector<Tool> &t)
{
    t.push_back({"list_printers", "Network printers set up in QIDI Studio (name, address, printer preset).", input_schema(),
                 tool_list_printers, true});
    t.push_back({"printer_status", "Live state of a printer: idle/printing/paused, file, progress, temperatures.",
                 input_schema({k_printer}), tool_printer_status, true});
    t.push_back({"get_box_slots",
                 "What is loaded in each QIDI Box slot (type, colour, QIDI or generic) and the filament preset that matches it.",
                 input_schema({k_printer}), tool_get_box_slots, true});
    t.push_back({"camera_snapshot", "A picture from the printer camera, to check the bed, the first layer or a running print.",
                 input_schema({k_printer}), tool_camera_snapshot, true});
    t.push_back({"box_action", "Load, unload or eject the filament of a Box slot, or re-read its RFID tag.",
                 input_schema({k_printer, k_box_action, k_box_slot}),
                 tool_box_action});
    t.push_back({"send_print",
                 "Send the sliced plate to the printer and START the print. Ask the user first and pass confirm: true. "
                 "box_slots gives the Box slot (1-based, from get_box_slots) for each filament in order, e.g. [3, 1] prints "
                 "filament 1 from slot 3 and filament 2 from slot 1. Without box_slots the Box is not used.",
                 input_schema({k_printer, k_print_plate, k_box_slots, k_use_box, k_bed_leveling, k_timelapse, k_cooler, k_confirm}),
                 tool_send_print, false, true});
    t.push_back({"pause_print",
                 "Pause the running print. The printer parks the toolhead and turns the nozzle heater off. The printer does "
                 "this after its current command, so it can take minutes during heating or bed leveling.",
                 input_schema({k_printer}), tool_pause_print});
    t.push_back({"resume_print", "Resume a paused print. The printer first heats the nozzle again, so this can take minutes.",
                 input_schema({k_printer}), tool_resume_print});
    t.push_back({"cancel_print",
                 "End the running or paused print for good; it cannot be resumed. While layers print, the printer first "
                 "pauses (pulls the filament back and parks the nozzle over the waste chute), so the nozzle does not "
                 "leak. Prints from the Q1 Pro and X 3 Series presets give no layer number, so they cancel at once. "
                 "Ask the user first and pass confirm: true.",
                 input_schema({k_printer, k_confirm}), tool_cancel_print, false,
                 true});
}

}}} // namespace Slic3r::GUI::Mcp
