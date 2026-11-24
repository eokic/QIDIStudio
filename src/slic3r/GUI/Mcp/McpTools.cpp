// MCP tools for the slicer side: status, presets, settings, models, plates, slicing, pictures, export.
// Every tool touches the GUI, model or presets only through run_on_ui() (the GUI thread).
#include "McpArgs.hpp"

#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r_version.h"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ParamsDialog.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/Tab.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <miniz.h>

#include <climits>
#include <cmath>
#include <cstdlib>

namespace Slic3r { namespace GUI { namespace Mcp {

using json = nlohmann::json;
namespace fs = boost::filesystem;

// ---------------------------------------------------------------------------------------------------------------
// Helpers

static constexpr double k_rad2deg = 57.29577951308232;

static double round_to(double v, double step) { return std::round(v / step) * step; }

static Preset::Type preset_type(const std::string &s)
{
    if (s == "printer") return Preset::TYPE_PRINTER;
    if (s == "print" || s == "process") return Preset::TYPE_PRINT;
    if (s == "filament") return Preset::TYPE_FILAMENT;
    throw ToolError("type must be 'printer', 'print' (process) or 'filament'");
}

static PresetCollection &collection(Preset::Type t)
{
    PresetBundle *b = wxGetApp().preset_bundle;
    return t == Preset::TYPE_PRINTER ? b->printers : t == Preset::TYPE_PRINT ? b->prints : b->filaments;
}

static Plater *plater_or_throw()
{
    Plater *p = wxGetApp().plater();
    if (p == nullptr) throw ToolError("QIDI Studio is still starting");
    if (p->is_loading_project()) throw ToolError("QIDI Studio is loading a project; try again in a moment");
    return p;
}

static json vec3(const Vec3d &v, double step = 0.01)
{
    return json::array({round_to(v.x(), step), round_to(v.y(), step), round_to(v.z(), step)});
}

static json object_json(Plater *plater, int idx)
{
    ModelObject *mo = plater->model().objects[idx];
    json         j  = {{"index", idx}, {"name", mo->name}, {"parts", mo->volumes.size()}, {"instances", mo->instances.size()}};
    if (!mo->instances.empty()) {
        const ModelInstance *mi = mo->instances.front();
        BoundingBoxf3        bb = mo->instance_bounding_box(0);
        j["size_mm"]            = vec3(bb.size(), 0.01);
        j["position_mm"]        = vec3(mi->get_offset(), 0.01);
        j["rotation_deg"]       = vec3(mi->get_rotation() * k_rad2deg, 0.1);
        j["scale"]              = vec3(mi->get_scaling_factor(), 0.0001);
        j["plate"]              = plater->get_partplate_list().find_instance(idx, 0) + 1; // 1-based, 0 = off any plate
    }
    if (const ConfigOption *ext = mo->config.option("extruder"))
        j["filament"] = ext->getInt(); // 0 = the default (filament 1)
    return j;
}

static int object_index(Plater *plater, const json &args)
{
    const int n = int(plater->model().objects.size());
    if (args.contains("object") && args["object"].is_string()) {
        const std::string name = args["object"].get<std::string>();
        for (int i = 0; i < n; ++i)
            if (plater->model().objects[i]->name == name) return i;
        throw ToolError("No object named '" + name + "'. Call list_objects for the names.");
    }
    const int idx = arg_int(args, "object", -1);
    if (idx < 0 || idx >= n)
        throw ToolError("Pick an object by its index (0 to " + std::to_string(n - 1) + ") or name. Call list_objects first.");
    return idx;
}

static json filament_slots_json()
{
    PresetBundle   *b     = wxGetApp().preset_bundle;
    json            slots = json::array();
    const ConfigOptionStrings *colours = b->project_config.option<ConfigOptionStrings>("filament_colour");
    for (size_t i = 0; i < b->filament_presets.size(); ++i) {
        json j = {{"filament", i + 1}, {"preset", b->filament_presets[i]}};
        if (colours && i < colours->values.size()) j["colour"] = colours->values[i];
        if (const Preset *p = b->filaments.find_preset(b->filament_presets[i], false)) {
            if (const auto *t = p->config.option<ConfigOptionStrings>("filament_type"); t && !t->values.empty())
                j["type"] = t->values.front();
            if (const auto *bt = p->config.option<ConfigOptionInts>("box_temperature"); bt && !bt->values.empty())
                j["box_temperature"] = bt->values.front();
        }
        slots.push_back(std::move(j));
    }
    return slots;
}

static json status_json()
{
    Plater       *plater = plater_or_throw();
    PresetBundle *b      = wxGetApp().preset_bundle;
    PartPlateList &plates = plater->get_partplate_list();
    json objects = json::array();
    for (int i = 0; i < int(plater->model().objects.size()); ++i) objects.push_back(object_json(plater, i));
    return {{"app", SLIC3R_APP_NAME},
            {"version", SLIC3R_VERSION},
            {"project", into_u8(plater->get_project_name())},
            {"project_file", into_u8(plater->get_project_filename())},
            {"presets",
             {{"printer", b->printers.get_selected_preset_name()},
              {"print", b->prints.get_selected_preset_name()},
              {"filaments", filament_slots_json()}}},
            {"unsaved_setting_changes",
             {{"print", b->prints.current_dirty_options(true)},
              {"filament", b->filaments.current_dirty_options(true)},
              {"printer", b->printers.current_dirty_options(true)}}},
            {"plates", {{"count", plates.get_plate_count()}, {"current", plates.get_curr_plate_index() + 1}}},
            {"current_plate_sliced", plates.get_curr_plate()->is_slice_result_valid()},
            {"slicing", plater->is_background_process_slicing()},
            {"objects", objects}};
}

// The one refusal for a filament slot that the project does not have.
static void check_filament_exists(int slot) // 1-based
{
    const size_t n = wxGetApp().preset_bundle->filament_presets.size();
    if (slot < 1 || size_t(slot) > n)
        throw ToolError("There is no filament " + std::to_string(slot) + ": the project has " + std::to_string(n) +
                        ". Use set_filament_count for more.");
}

// A filament slot whose preset can change.
static void check_filament_slot(int slot) // 1-based
{
    PresetBundle *b = wxGetApp().preset_bundle;
    check_filament_exists(slot);
    // A mixed filament only holds a copy of another filament's preset name.
    if (b->is_mixed_filament(size_t(slot - 1)))
        throw ToolError("Filament " + std::to_string(slot) + " is a mix of other filaments. Change those filaments.");
}

// The settings of a checked filament slot (1-based). Only the Filament tab's preset has unsaved edits (find_preset
// returns its edited copy); with 2 or more filaments the tab can show a preset that the slot does not use.
static const DynamicPrintConfig &filament_config(int slot)
{
    PresetBundle *b = wxGetApp().preset_bundle;
    const Preset *p = b->filaments.find_preset(b->filament_presets[size_t(slot - 1)]);
    return (p != nullptr ? *p : b->filaments.get_edited_preset()).config;
}

// The config that holds a key, and its preset type (TYPE_INVALID: the project config).
static std::pair<const DynamicPrintConfig *, Preset::Type> config_holding(const std::string &key, int slot = 1)
{
    PresetBundle *b = wxGetApp().preset_bundle;
    if (b->prints.get_edited_preset().config.has(key)) return {&b->prints.get_edited_preset().config, Preset::TYPE_PRINT};
    if (b->filaments.get_edited_preset().config.has(key)) return {&filament_config(slot), Preset::TYPE_FILAMENT};
    if (b->printers.get_edited_preset().config.has(key)) return {&b->printers.get_edited_preset().config, Preset::TYPE_PRINTER};
    if (b->project_config.has(key)) return {&b->project_config, Preset::TYPE_INVALID};
    return {nullptr, Preset::TYPE_INVALID};
}

static const char *type_name(Preset::Type t)
{
    return t == Preset::TYPE_PRINT ? "print" : t == Preset::TYPE_FILAMENT ? "filament" : t == Preset::TYPE_PRINTER ? "printer" : "project";
}

static json describe_key(const std::string &key, bool with_help, int slot)
{
    json j = {{"key", key}};
    auto [cfg, type] = config_holding(key, slot);
    if (cfg) {
        j["value"] = cfg->opt_serialize(key);
        j["in"]    = type_name(type);
    }
    if (const ConfigOptionDef *def = print_config_def.get(key)) {
        if (!def->label.empty()) j["label"] = def->label;
        if (!def->category.empty()) j["category"] = def->category;
        if (!def->sidetext.empty()) j["unit"] = def->sidetext;
        if (with_help) {
            std::string tip = def->tooltip;
            if (tip.size() > 400) tip = tip.substr(0, 400) + "...";
            if (!tip.empty()) j["help"] = tip;
            if (!def->enum_values.empty()) j["allowed"] = def->enum_values;
            if (def->min != ConfigOptionDef::min_default) j["min"] = def->min;
            if (def->max != ConfigOptionDef::max_default) j["max"] = def->max;
        }
    }
    return j;
}

// A JSON value as the config's string form.
static std::string to_config_string(const json &v)
{
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "1" : "0";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) {
        std::string s = std::to_string(v.get<double>());
        if (s.find('.') != std::string::npos) {
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
        }
        return s;
    }
    if (v.is_array()) {
        std::string out;
        for (size_t i = 0; i < v.size(); ++i) out += (i ? "," : "") + to_config_string(v[i]);
        return out;
    }
    throw ToolError("unsupported value: " + v.dump());
}

// QIDI Studio's settings window checks a number before the config parses it: the parsers read "abc" in a list as 0,
// and "247abc" or "247.5" for a whole number as 247.
static bool is_valid_number_text(const std::string &text, ConfigOptionType type)
{
    const bool whole   = type == coInt || type == coInts;
    const bool percent = type == coPercent || type == coPercents || type == coFloatOrPercent || type == coFloatsOrPercents;
    if (!whole && !percent && type != coFloat && type != coFloats) return true; // bools and enums parse strictly; points are left
    std::vector<std::string> items{text}; // a single number with a comma ("0,2") is not split: its parser reads 0
    if (type & coVectorType) boost::split(items, text, boost::is_any_of(","));
    for (std::string item : items) {
        boost::trim(item);
        if (item == "nil") continue; // the parser rejects it where a setting has no "nil"
        if (percent && !item.empty() && item.back() == '%') item.pop_back();
        char        *end = nullptr;
        const double v   = whole ? double(std::strtoll(item.c_str(), &end, 10)) : std::strtod(item.c_str(), &end);
        if (item.empty() || *end != '\0' || !std::isfinite(v) || (whole && std::fabs(v) > INT_MAX)) return false;
    }
    return true;
}

static std::string encode_png(const ThumbnailData &data)
{
    size_t png_size = 0;
    void  *png = tdefl_write_image_to_png_file_in_memory_ex((const void *) data.pixels.data(), data.width, data.height, 4,
                                                           &png_size, MZ_DEFAULT_LEVEL, 1);
    if (png == nullptr) throw ToolError("could not encode the picture");
    std::string out((const char *) png, png_size);
    mz_free(png);
    return out;
}

// The warnings that the QIDI Studio window shows for the current plate, read from the same state: the slicing step
// warnings of the print and of each object (Plater::priv::on_slicing_update), the validation warning
// (process_validation_warning), and the G-code path conflict and G-code checks of the slice result.
// The 3D view's own checks (GLCanvas3D::EWarning) are not here.
static json slice_warnings(const Print &print, GCodeProcessorResult *res)
{
    json warnings  = json::array();
    auto add_steps = [&warnings](const PrintStateBase::StateWithWarnings &state, const char *source, const ModelObject *mo) {
        for (const PrintStateBase::Warning &w : state.warnings) {
            if (!w.current) continue; // the GUI shows only current warnings
            json j = {{"source", source},
                      {"level", w.level == PrintStateBase::WarningLevel::CRITICAL ? "serious_warning" : "warning"},
                      {"message", w.message}};
            if (mo) j["object"] = mo->name;
            warnings.push_back(std::move(j));
        }
    };
    for (int s = 0; s < psCount; ++s) add_steps(print.step_state_with_warnings(PrintStep(s)), "print", nullptr);
    for (const PrintObject *po : print.objects())
        for (int s = 0; s < posCount; ++s) add_steps(po->step_state_with_warnings(PrintObjectStep(s)), "object", po->model_object());

    StringObjectException vw;
    if (print.validate(&vw).string.empty() && !vw.string.empty()) {
        json j = {{"source", "validate"}, {"level", "warning"}, {"message", boost::algorithm::trim_copy(vw.string)}};
        const auto        *po = dynamic_cast<const PrintObjectBase *>(vw.object);
        const ModelObject *mo = po ? po->model_object() : dynamic_cast<const ModelObject *>(vw.object);
        if (mo) j["object"] = mo->name;
        if (!vw.opt_key.empty()) j["setting"] = vw.opt_key;
        warnings.push_back(std::move(j));
    }

    if (res == nullptr) return warnings;
    if (const auto &c = res->conflict_result) // GLCanvas3D::_set_warning_notification, EWarning::GCodeConflict
        warnings.push_back({{"source", "gcode"},
                            {"level", "serious_warning"},
                            {"message", (boost::format(_u8L("Conflicts of gcode paths have been found at layer %d. Please separate the "
                                                            "conflicted objects farther (%s <-> %s).")) %
                                         (c->layer + 1) % c->_objName1 % c->_objName2).str()},
                            {"objects", json::array({c->_objName1, c->_objName2})}});
    for (GCodeProcessorResult::SliceWarning w : res->warnings) { // a copy: get_slice_warning_string takes a non-const reference
        std::string text = into_u8(Plater::get_slice_warning_string(w));
        if (w.msg == NOT_GENERATE_TIMELAPSE) // the print dialog's text (SelectMachineDialog::has_timelapse_warning)
            text = w.error_code == "10014001" ? _u8L("When enable spiral vase mode, machines with I3 structure will not generate timelapse videos.") :
                   w.error_code == "10014002" ? _u8L("The current printer does not support timelapse in Traditional Mode when printing By-Object.") :
                                                "";
        // GCodeProcessor documents 0 tip, 1 warning, 2 error; its bed temperature check sets 3.
        warnings.push_back({{"source", "gcode"},
                            {"level", w.level == 2 ? "error" : w.level == 0 ? "note" : "warning"},
                            {"message", text.empty() ? w.msg : text},
                            {"key", w.msg},
                            {"code", w.error_code}});
    }
    return warnings;
}

static json slice_stats(Plater *plater)
{
    PartPlateList &plates = plater->get_partplate_list();
    PartPlate     *plate  = plates.get_curr_plate();
    json           j      = {{"plate", plates.get_curr_plate_index() + 1}};
    const PrintStatistics &ps = plates.get_current_fff_print().print_statistics();
    j["estimated_time"]      = ps.estimated_normal_print_time;
    j["filament_total_m"]    = round_to(ps.total_used_filament / 1000.0, 0.01);
    j["filament_total_g"]    = round_to(ps.total_weight, 0.01);
    if (ps.total_cost > 0) j["cost"] = round_to(ps.total_cost, 0.01);
    if (GCodeProcessorResult *res = plate->get_slice_result()) {
        j["estimated_time_s"]  = int(std::lround(res->print_statistics.modes[0].time));
        j["filament_changes"]  = res->print_statistics.total_filament_changes;
        PresetBundle *b        = wxGetApp().preset_bundle;
        json          per      = json::array();
        for (const auto &[ext, volume] : res->print_statistics.total_volumes_per_extruder) {
            json f = {{"filament", ext + 1}, {"volume_cm3", round_to(volume / 1000.0, 0.01)}};
            if (ext < b->filament_presets.size()) {
                f["preset"] = b->filament_presets[ext];
                if (const Preset *p = b->filaments.find_preset(b->filament_presets[ext], false))
                    if (const auto *d = p->config.option<ConfigOptionFloats>("filament_density"); d && !d->values.empty())
                        f["weight_g"] = round_to(volume / 1000.0 * d->values.front(), 0.01);
            }
            per.push_back(std::move(f));
        }
        j["per_filament"] = per;
    }
    j["warnings"] = slice_warnings(plates.get_current_fff_print(), plate->get_slice_result());
    return j;
}

// ---------------------------------------------------------------------------------------------------------------
// Arguments: each one makes its schema entry (Registration, below) and reads its value (the tools).

static const Arg           k_object{"object", {{"description", "Object index (from list_objects) or exact name"},
                                               {"type", json::array({"integer", "string"})}}, true};
static const EnumArg       k_preset_type{"type", "print is also called process", {"printer", "print", "process", "filament"}};
static const StringArg     k_preset_search{"search", "Only names containing this text"};
static const BoolArg       k_include_incompatible{"include_incompatible", "Also presets that do not fit the selected printer", false};
static const StringArg     k_preset_name{"name", "Preset name, from list_presets", true};
static const IntArg        k_preset_filament{"filament", "Filament slot, 1-based (filament presets only, default 1)", 1, INT_MAX, 0};
static const StringListArg k_config_keys{"keys", "Setting keys"};
static const StringArg     k_config_search{"search", "Settings whose key or label contains this word"};
static const IntArg        k_filament{"filament", "Filament slot, 1-based", 1, INT_MAX, 1};
static const Arg           k_changes{"changes", {{"type", "object"}, {"description", "Setting key -> new value"}}, true};
static const StringArg     k_model_path{"path", "A model file on this computer"};
static const StringListArg k_model_paths{"paths", "Several model files on this computer"};
static const VecArg        k_position{"position", "[x,y] in mm on the bed (a third number is not used)", 2};
static const VecArg        k_move{"move", "[dx,dy] or [dx,dy,dz] in mm", 2};
static const VecArg        k_rotate{"rotate", "[x,y,z] in degrees about the world axes", 3};
static const VecArg        k_scale{"scale", "A factor, or [x,y,z]; each above 0 and at most 100", 3, true};
static const IntArg        k_object_filament{"filament", "Filament slot, 1-based", 1};
static const IntArg        k_filament_count{"count", "Number of filament slots", 1, 16};
static const IntArg        k_colour_filament{"filament", "Filament slot, 1-based", 1};
static const StringArg     k_colour{"colour", "#RRGGBB", true};
static const IntArg        k_select_plate{"plate", "Plate, 1-based", 1};
static const IntArg        k_plate{"plate", "Plate, 1-based; not given or 0: the current plate", 0, INT_MAX, 0};
static const IntArg        k_slice_timeout{"timeout_s", "Seconds to wait for the slicing", 10, 3600, 900};
static const EnumArg       k_view{"view", "", {"iso", "top", "front", "left", "right", "rear", "bottom"}, "iso"};
static const IntArg        k_render_size{"size", "Pixels", 128, 1600, 768};
static const StringArg     k_gcode_path{"path", "A file on this computer, ending in .gcode", true, ".gcode"};
static const StringArg     k_project_path{"path", "A file ending in .3mf; not given: the project's own file", false, ".3mf"};

// ---------------------------------------------------------------------------------------------------------------
// Tools

static ToolResult tool_get_status(const json &)
{
    return ToolResult::json(run_on_ui([] { return status_json(); }));
}

static ToolResult tool_list_presets(const json &args)
{
    const Preset::Type type       = preset_type(k_preset_type.read(args));
    const bool         all        = k_include_incompatible.read(args);
    std::string        search     = boost::algorithm::to_lower_copy(k_preset_search.read(args));
    return ToolResult::json(run_on_ui([type, all, search] {
        PresetCollection &c        = collection(type);
        const std::string selected = c.get_selected_preset_name();
        json              list     = json::array();
        size_t            skipped  = 0;
        for (const Preset &p : c.get_presets()) {
            if (p.is_default || (!all && (!p.is_visible || !p.is_compatible))) continue;
            if (!search.empty() && boost::algorithm::to_lower_copy(p.name).find(search) == std::string::npos) continue;
            if (list.size() >= 300) { ++skipped; continue; }
            json j = {{"name", p.name}, {"kind", p.is_system ? "system" : p.is_user() ? "user" : "project"}};
            if (p.name == selected) j["selected"] = true;
            if (!p.is_compatible) j["compatible"] = false;
            list.push_back(std::move(j));
        }
        json out = {{"type", type_name(type)}, {"selected", selected}, {"presets", list}};
        if (skipped) out["more_not_shown"] = skipped;
        return out;
    }));
}

static ToolResult tool_select_preset(const json &args)
{
    const Preset::Type type = preset_type(k_preset_type.read(args));
    const std::string  name = k_preset_name.read(args);
    const int          slot = k_preset_filament.read(args); // 1-based filament slot, 0 = not given (filament 1)
    if (slot != 0 && type != Preset::TYPE_FILAMENT) throw ToolError("'filament' only applies to type 'filament'");
    return ToolResult::json(run_on_ui(
        [type, name, slot] {
            Plater           *plater = plater_or_throw();
            PresetBundle     *b      = wxGetApp().preset_bundle;
            PresetCollection &c      = collection(type);
            const Preset     *preset = c.find_preset(name, false, true);
            if (preset == nullptr) throw ToolError("No " + std::string(type_name(type)) + " preset named '" + name + "'. Use list_presets.");
            if (!preset->is_compatible && type != Preset::TYPE_PRINTER)
                throw ToolError("'" + name + "' is not compatible with the selected printer");

            // With 2 or more filaments the Filament tab no longer feeds the project, so a slot is changed the way the
            // sidebar's filament list does it (Plater::priv::on_select_preset).
            if (type == Preset::TYPE_FILAMENT && (slot > 1 || plater->sidebar().is_multifilament())) {
                const size_t idx = size_t(std::max(slot, 1) - 1);
                check_filament_slot(int(idx) + 1);
                const std::string before = b->filament_presets[idx];
                b->set_filament_preset(idx, name);
                if (!plater->on_filament_change(idx)) {
                    b->set_filament_preset(idx, before);
                    throw ToolError("QIDI Studio refused that filament for slot " + std::to_string(slot));
                }
                plater->update_project_dirty_from_presets();
                b->export_selections(*wxGetApp().app_config);
                auto &combos = plater->sidebar().combos_filament();
                if (idx < combos.size()) combos[idx]->update();
                plater->on_config_change(b->full_config());
                return json{{"selected", name}, {"filament", idx + 1}, {"before", before}, {"filaments", filament_slots_json()}};
            }

            // Switching a preset with unsaved edits would open a "keep or discard?" dialog nobody can answer
            // remotely, so the edits are dropped and reported.
            json discarded = json::array();
            const std::pair<PresetCollection *, const char *> colls[] = {
                {&b->prints, "print"}, {&b->filaments, "filament"}, {&b->printers, "printer"}};
            for (const auto &c2 : colls) {
                if (c2.first->current_is_dirty()) {
                    c2.first->discard_current_changes();
                    discarded.push_back(c2.second);
                }
            }
            const std::string before = c.get_selected_preset_name();
            Tab *tab = wxGetApp().get_tab(type);
            if (tab == nullptr) throw ToolError("the settings tab is not ready");
            if (!tab->select_preset(name, false, "", true, true)) throw ToolError("QIDI Studio did not switch the preset");
            json out = {{"selected", name}, {"before", before}};
            if (!discarded.empty()) out["dropped_unsaved_changes_in"] = discarded;
            if (type == Preset::TYPE_PRINTER) {
                out["print"]     = b->prints.get_selected_preset_name();
                out["filaments"] = filament_slots_json();
            }
            return out;
        },
        60));
}

static ToolResult tool_get_config(const json &args)
{
    const std::vector<std::string> keys   = k_config_keys.read(args);
    const std::string              search = boost::algorithm::to_lower_copy(k_config_search.read(args));
    const int                      slot   = k_filament.read(args);
    return ToolResult::json(run_on_ui([keys, search, slot] {
        plater_or_throw();
        check_filament_slot(slot);
        PresetBundle *b = wxGetApp().preset_bundle;
        json          out;
        if (!keys.empty()) {
            json list = json::array();
            for (const std::string &k : keys) {
                json d = describe_key(k, true, slot);
                if (!d.contains("value") && !d.contains("label")) d["error"] = "unknown setting";
                list.push_back(std::move(d));
            }
            out["settings"] = list;
        } else if (!search.empty()) {
            json list = json::array();
            for (const auto &[key, def] : print_config_def.options) {
                if (config_holding(key).first == nullptr) continue;
                const std::string hay = boost::algorithm::to_lower_copy(key + " " + def.label + " " + def.full_label + " " + def.category);
                if (hay.find(search) == std::string::npos) continue;
                list.push_back(describe_key(key, false, slot));
                if (list.size() >= 60) break;
            }
            out["settings"] = list;
            out["note"]     = "Call get_config with keys for help text, allowed values and limits.";
        } else {
            // Overview: what changed from the saved presets, plus the usual first questions.
            static const char *common[] = {"layer_height", "initial_layer_print_height", "wall_loops", "top_shell_layers",
                                           "bottom_shell_layers", "sparse_infill_density", "sparse_infill_pattern",
                                           "enable_support", "support_type", "brim_type", "seam_position",
                                           "enable_prime_tower", "curr_bed_type", "nozzle_temperature",
                                           "chamber_temperatures", "box_temperature", "print_sequence"};
            json list = json::array();
            for (const char *k : common)
                if (config_holding(k).first) list.push_back(describe_key(k, false, slot));
            // The unsaved filament edits belong to the Filament tab's preset, which this filament may not use.
            const bool tab_has_slot = b->filament_presets[size_t(slot - 1)] == b->filaments.get_selected_preset_name();
            out["common"]  = list;
            out["changed"] = {{"print", b->prints.current_dirty_options(true)},
                              {"filament", tab_has_slot ? b->filaments.current_dirty_options(true) : std::vector<std::string>()},
                              {"printer", b->printers.current_dirty_options(true)}};
            out["note"] = "Pass keys for exact settings or search for a word (e.g. 'wall', 'support', 'speed').";
        }
        return out;
    }));
}

static ToolResult tool_set_config(const json &args)
{
    if (!args.contains("changes") || !args["changes"].is_object() || args["changes"].empty())
        throw ToolError("changes must be an object of setting key -> new value, e.g. {\"wall_loops\": 3}");
    const json changes = args["changes"];
    const int  slot    = k_filament.read(args);
    return ToolResult::json(run_on_ui([changes, slot] {
        Plater       *plater = plater_or_throw();
        PresetBundle *b      = wxGetApp().preset_bundle;
        check_filament_slot(slot);
        struct Target { Preset::Type type; DynamicPrintConfig cfg; bool touched = false; };
        Target targets[3] = {{Preset::TYPE_PRINT, b->prints.get_edited_preset().config},
                             {Preset::TYPE_FILAMENT, filament_config(slot)},
                             {Preset::TYPE_PRINTER, b->printers.get_edited_preset().config}};
        DynamicPrintConfig project = b->project_config;
        bool               project_touched = false;
        json applied = json::array(), errors = json::object();
        for (const auto &[key, value] : changes.items()) {
            auto [holder, type] = config_holding(key);
            if (holder == nullptr) { errors[key] = "unknown setting (use get_config with search)"; continue; }
            DynamicPrintConfig *cfg = &project;
            for (Target &t : targets) if (t.type == type) cfg = &t.cfg;
            const std::string before = cfg->opt_serialize(key);
            try {
                CNumericLocalesSetter numbers; // the UI language sets the decimal separator of strtod and std::to_string
                const std::string     text = to_config_string(value);
                if (!is_valid_number_text(text, cfg->option(key)->type())) throw ToolError("not a number: " + text);
                cfg->set_deserialize_strict(key, text);
            } catch (const std::exception &e) {
                errors[key] = std::string("rejected: ") + e.what();
                continue;
            }
            const std::string after = cfg->opt_serialize(key);
            if (after == before) continue;
            if (cfg == &project) project_touched = true;
            for (Target &t : targets) if (cfg == &t.cfg) t.touched = true;
            applied.push_back({{"key", key}, {"in", type_name(type)}, {"before", before}, {"after", after}});
        }
        // Only the Filament tab's preset takes edits, so show the preset of this filament there first. With 2 or more
        // filaments the tab does not change the filament slots (Sidebar::update_presets).
        json              dropped = json::array();
        const std::string name    = b->filament_presets[size_t(slot - 1)];
        if (targets[1].touched && name != b->filaments.get_selected_preset_name()) {
            // While the settings window is open, the tab changes the filament it was opened for.
            if (ParamsDialog *d = wxGetApp().params_dialog(); d != nullptr && d->IsShown())
                throw ToolError("Close the settings window in QIDI Studio first");
            if (b->filaments.current_is_dirty()) {
                b->filaments.discard_current_changes();
                dropped.push_back("filament");
            }
            // A preset that is not found or hidden selects another one.
            if (Tab *tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT)) tab->select_preset(name, false, "", true, true);
            if (b->filaments.get_selected_preset_name() != name)
                throw ToolError("QIDI Studio did not open the settings of filament " + std::to_string(slot));
        }
        // The GUI's own path: dirty markers, panel refresh, slice invalidation.
        for (Target &t : targets) {
            if (!t.touched) continue;
            Tab *tab = wxGetApp().get_tab(t.type);
            if (tab == nullptr) { errors["_" + std::string(type_name(t.type))] = "settings tab not ready"; continue; }
            tab->load_config(t.cfg);
            plater->on_config_change(t.cfg);
        }
        if (project_touched) {
            b->project_config.apply(project);
            plater->on_config_change(b->project_config);
        }
        json out = {{"applied", applied}};
        if (!dropped.empty()) out["dropped_unsaved_changes_in"] = dropped;
        if (!errors.empty()) out["errors"] = errors;
        if (!applied.empty())
            out["note"] = "Changes are unsaved edits of the current presets (shown in orange in QIDI Studio). Slice again to see their effect.";
        return out;
    }));
}

static ToolResult tool_load_model(const json &args)
{
    std::vector<std::string> paths = k_model_paths.read(args);
    if (const std::string p = k_model_path.read(args); !p.empty()) paths.push_back(p);
    if (paths.empty()) throw ToolError("give path (one file) or paths (several)");
    for (const std::string &p : paths) {
        if (!fs::exists(fs::path(p))) throw ToolError("File not found on this computer: " + p);
        const std::string ext = boost::algorithm::to_lower_copy(fs::path(p).extension().string());
        if (ext != ".stl" && ext != ".3mf" && ext != ".obj" && ext != ".step" && ext != ".stp" && ext != ".amf")
            throw ToolError("Unsupported file type " + ext + " (use .stl, .3mf, .obj, .step, .stp, .amf)");
    }
    return ToolResult::json(run_on_ui(
        [paths] {
            Plater    *plater = plater_or_throw();
            AppConfig *cfg    = wxGetApp().app_config;
            // No STEP mesh dialog during a remote load: use the stored defaults.
            struct RestoreStepDialog
            {
                AppConfig *cfg;
                bool       was_on;
                ~RestoreStepDialog() { cfg->set_bool("enable_step_mesh_setting", was_on); }
            } restore { cfg, cfg->get_bool("enable_step_mesh_setting") };
            cfg->set_bool("enable_step_mesh_setting", false);
            std::vector<size_t> idxs;
            {
                Plater::TakeSnapshot snapshot(plater, "Import Object");
                std::vector<fs::path> files;
                for (const std::string &p : paths) files.emplace_back(p);
                idxs = plater->load_files(files, LoadStrategy::LoadModel);
            }
            if (idxs.empty()) throw ToolError("QIDI Studio could not load the file(s)");
            json objects = json::array();
            for (size_t i : idxs) objects.push_back(object_json(plater, int(i)));
            return json{{"loaded", objects},
                        {"note", "Models are placed automatically. Use arrange, auto_orient or transform_object to adjust."}};
        },
        180));
}

static ToolResult tool_list_objects(const json &)
{
    return ToolResult::json(run_on_ui([] {
        Plater *plater  = plater_or_throw();
        json    objects = json::array();
        for (int i = 0; i < int(plater->model().objects.size()); ++i) objects.push_back(object_json(plater, i));
        return json{{"objects", objects}};
    }));
}

static ToolResult tool_transform_object(const json &args)
{
    auto read_vec = [&args](const VecArg &arg, bool &present) -> Vec3d {
        const auto v = arg.read(args);
        present      = v.has_value();
        return present ? Vec3d((*v)[0], (*v)[1], (*v)[2]) : Vec3d::Zero();
    };
    bool        has_pos = false, has_move = false, has_rot = false, has_scale = false;
    const Vec3d pos   = read_vec(k_position, has_pos); // its z is not used: the object stays on the bed
    const Vec3d move  = read_vec(k_move, has_move);
    const Vec3d rot   = read_vec(k_rotate, has_rot);
    const Vec3d scale = read_vec(k_scale, has_scale);
    if (!has_pos && !has_move && !has_rot && !has_scale)
        throw ToolError("give position [x,y] in mm, move [dx,dy,dz] in mm, rotate [x,y,z] in degrees, and/or scale");
    if (has_scale && (scale.minCoeff() <= 0.0 || scale.maxCoeff() > 100.0)) throw ToolError("scale factors must be between 0 and 100");
    const json a = args;
    return ToolResult::json(run_on_ui([=] {
        Plater      *plater = plater_or_throw();
        const int    idx    = object_index(plater, a);
        ModelObject *mo     = plater->model().objects[idx];
        if (mo->instances.empty()) throw ToolError("that object has no instance on the plate");
        ModelInstance *mi = mo->instances.front();
        Plater::TakeSnapshot snapshot(plater, "Transform Object");
        if (has_rot) {
            // Rotate about the world axes, X then Y then Z, on top of the current rotation.
            Transform3d r = Transform3d::Identity();
            r.rotate(Eigen::AngleAxisd(rot.z() / k_rad2deg, Vec3d::UnitZ()));
            r.rotate(Eigen::AngleAxisd(rot.y() / k_rad2deg, Vec3d::UnitY()));
            r.rotate(Eigen::AngleAxisd(rot.x() / k_rad2deg, Vec3d::UnitX()));
            const Transform3d m = r * mi->get_matrix(true, false, true); // rotation only
            mi->set_rotation(Geometry::extract_euler_angles(m.linear()));
        }
        if (has_scale) mi->set_scaling_factor(scale);
        // Rotating or scaling can push the part into the bed or lift it: put it back on the bed before the move, so
        // that a move's dz counts from the bed. A move or a position alone keeps the height.
        if (has_rot || has_scale) {
            mo->invalidate_bounding_box();
            mo->ensure_on_bed();
        }
        if (has_move) mi->set_offset(mi->get_offset() + move);
        if (has_pos) {
            Vec3d off = mi->get_offset();
            off.x() = pos.x(), off.y() = pos.y();
            mi->set_offset(off);
        }
        // changed_object() drops a lifted part back onto the bed, but keeps a sunk one.
        mo->invalidate_bounding_box();
        plater->get_partplate_list().notify_instance_update(idx, 0);
        plater->changed_object(idx);
        if (ObjectList *ol = wxGetApp().obj_list()) ol->update_plate_values_for_items();
        return json{{"object", object_json(plater, idx)}};
    }));
}

static ToolResult tool_delete_object(const json &args)
{
    const json a = args;
    return ToolResult::json(run_on_ui([a] {
        Plater           *plater = plater_or_throw();
        const int         idx    = object_index(plater, a);
        const std::string name   = plater->model().objects[idx]->name;
        const size_t      before = plater->model().objects.size();
        ObjectList       *ol     = wxGetApp().obj_list();
        if (ol == nullptr) throw ToolError("object list not ready");
        // The GUI's own path: it removes the object from the model and its row from the object list.
        // Plater::delete_object_from_model alone leaves the row, and the next load reads past the objects.
        ol->delete_from_model_and_list(itObject, idx, -1);
        if (plater->model().objects.size() == before) throw ToolError("QIDI Studio did not delete the object");
        return json{{"deleted", name}, {"remaining", plater->model().objects.size()}};
    }));
}

static ToolResult tool_set_object_filament(const json &args)
{
    const int  filament = k_object_filament.read(args);
    const json a        = args;
    return ToolResult::json(run_on_ui([a, filament] {
        Plater   *plater = plater_or_throw();
        const int idx    = object_index(plater, a);
        const int count  = int(wxGetApp().preset_bundle->filament_presets.size());
        check_filament_exists(filament);
        ModelObject *mo = plater->model().objects[idx];
        Plater::TakeSnapshot snapshot(plater, "Change Filaments");
        mo->config.set("extruder", filament);
        for (ModelVolume *v : mo->volumes)
            if (v->config.has("extruder")) v->config.set("extruder", filament);
        plater->changed_object(idx);
        if (ObjectList *ol = wxGetApp().obj_list()) ol->update_objects_list_filament_column(size_t(count));
        return json{{"object", object_json(plater, idx)}};
    }));
}

static ToolResult tool_set_filament_count(const json &args)
{
    const int count = k_filament_count.read(args);
    return ToolResult::json(run_on_ui([count] {
        Plater       *plater = plater_or_throw();
        Sidebar      &sb     = plater->sidebar();
        PresetBundle *b      = wxGetApp().preset_bundle;
        json          out;
        int           now = int(b->filament_presets.size());
        while (now < count) { sb.add_filament(); int n = int(b->filament_presets.size()); if (n == now) break; now = n; }
        while (now > count) {
            // Removing the only filament with the Filament tab's preset switches the tab, which would ask "keep or
            // discard?" about its unsaved edits. Nobody can answer that remotely, so the edits are dropped.
            if (b->is_the_only_edited_filament(unsigned(now - 1)) && b->filaments.current_is_dirty()) {
                b->filaments.discard_current_changes();
                out["dropped_unsaved_changes_in"] = json::array({"filament"});
            }
            sb.delete_filament(size_t(now - 1));
            int n = int(b->filament_presets.size());
            if (n == now) break;
            now = n;
        }
        out["filaments"] = filament_slots_json();
        return out;
    }));
}

static ToolResult tool_set_filament_colour(const json &args)
{
    const int         slot   = k_colour_filament.read(args);
    std::string       colour = k_colour.read(args);
    if (colour.size() == 6) colour = "#" + colour;
    if (colour.size() != 7 || colour[0] != '#') throw ToolError("colour must look like #RRGGBB");
    return ToolResult::json(run_on_ui([slot, colour] {
        Plater       *plater = plater_or_throw();
        PresetBundle *b      = wxGetApp().preset_bundle;
        check_filament_exists(slot);
        auto *colours = b->project_config.option<ConfigOptionStrings>("filament_colour");
        if (colours == nullptr || int(colours->values.size()) < slot) throw ToolError("no colour list in this project");
        colours->values[size_t(slot - 1)] = boost::algorithm::to_upper_copy(colour);
        auto &combos = plater->sidebar().combos_filament();
        if (size_t(slot - 1) < combos.size()) combos[size_t(slot - 1)]->update();
        plater->on_config_change(b->project_config);
        return json{{"filaments", filament_slots_json()}};
    }));
}

// Waits until the plater's job worker (arrange, orient) is idle.
static void wait_for_jobs(int timeout_s)
{
    wait_until([] { return wxGetApp().plater()->get_ui_job_worker().is_idle(); }, timeout_s, 300,
               "still busy after " + std::to_string(timeout_s) + " s");
}

static ToolResult tool_arrange(const json &)
{
    run_on_ui([] {
        Plater *plater = plater_or_throw();
        if (plater->model().objects.empty()) throw ToolError("nothing on the plate");
        plater->arrange();
        return json();
    });
    wait_for_jobs(120);
    return tool_list_objects(json::object());
}

static ToolResult tool_auto_orient(const json &)
{
    run_on_ui([] {
        Plater *plater = plater_or_throw();
        if (plater->model().objects.empty()) throw ToolError("nothing on the plate");
        plater->orient();
        return json();
    });
    wait_for_jobs(300);
    return tool_list_objects(json::object());
}

static ToolResult tool_list_plates(const json &)
{
    return ToolResult::json(run_on_ui([] {
        Plater        *plater = plater_or_throw();
        PartPlateList &pl     = plater->get_partplate_list();
        json           plates = json::array();
        for (int i = 0; i < pl.get_plate_count(); ++i) {
            PartPlate *p = pl.get_plate(i);
            json       objs = json::array();
            for (const auto &[obj, inst] : p->get_obj_and_inst_set())
                if (obj >= 0 && obj < int(plater->model().objects.size())) objs.push_back(plater->model().objects[obj]->name);
            plates.push_back({{"plate", i + 1}, {"name", p->get_plate_name()}, {"objects", objs},
                              {"sliced", p->is_slice_result_valid()}, {"current", i == pl.get_curr_plate_index()}});
        }
        return json{{"plates", plates}};
    }));
}

static ToolResult tool_select_plate(const json &args)
{
    const int plate = k_select_plate.read(args);
    return ToolResult::json(run_on_ui([plate] {
        Plater *plater = plater_or_throw();
        if (plate < 1 || plate > plater->get_partplate_list().get_plate_count()) throw ToolError("no such plate");
        plater->select_plate(plate - 1);
        return json{{"current", plate}};
    }));
}

static ToolResult tool_slice(const json &args)
{
    const int plate     = k_plate.read(args);
    const int timeout_s = k_slice_timeout.read(args);
    // Start on the GUI thread.
    json started = run_on_ui([plate] {
        Plater *plater = plater_or_throw();
        if (plater->is_background_process_slicing()) return json{{"state", "slicing"}};
        PartPlateList &pl = plater->get_partplate_list();
        if (plate != 0) {
            if (plate < 1 || plate > pl.get_plate_count()) throw ToolError("no such plate");
            if (plate - 1 != pl.get_curr_plate_index()) plater->select_plate(plate - 1);
        }
        if (pl.get_curr_plate()->empty()) throw ToolError("plate " + std::to_string(pl.get_curr_plate_index() + 1) + " is empty");
        // Re-validate first: an earlier failure stays latched on the plate otherwise.
        plater->update(false, true);
        if (pl.get_curr_plate()->is_slice_result_valid()) return json{{"state", "done"}};
        plater->reslice();
        if (plater->is_background_process_slicing()) return json{{"state", "slicing"}};
        if (pl.get_curr_plate()->is_slice_result_valid()) return json{{"state", "done"}};
        std::string why;
        try {
            StringObjectException err = pl.get_current_fff_print().validate();
            plater->post_process_string_object_exception(err);
            why = boost::algorithm::trim_copy(err.string);
        } catch (const std::exception &e) { why = e.what(); }
        throw ToolError("This plate can't be sliced" + (why.empty() ? std::string(" (an object may be off the plate)") : ": " + why));
    });
    // Wait for the background slicing to finish.
    if (started["state"] == "slicing")
        wait_until([] { return !wxGetApp().plater()->is_background_process_slicing(); }, timeout_s, 500,
                   "Slicing is still running after " + std::to_string(timeout_s) + " s. Call slice again to keep waiting.");
    return ToolResult::json(run_on_ui([] {
        Plater *plater = wxGetApp().plater();
        if (!plater->get_partplate_list().get_curr_plate()->is_slice_result_valid())
            throw ToolError("Slicing failed or was cancelled. Look at the notifications in the QIDI Studio window.");
        return slice_stats(plater);
    }));
}

static ToolResult tool_render_plate(const json &args)
{
    static const std::map<std::string, Camera::ViewAngleType> views = {
        {"iso", Camera::ViewAngleType::Iso},     {"top", Camera::ViewAngleType::Top},   {"front", Camera::ViewAngleType::Front},
        {"left", Camera::ViewAngleType::Left},   {"right", Camera::ViewAngleType::Right}, {"rear", Camera::ViewAngleType::Rear},
        {"bottom", Camera::ViewAngleType::Bottom}};
    const std::string view  = k_view.read(args);
    const int         plate = k_plate.read(args);
    const int         size  = k_render_size.read(args);
    const int         angle = int(views.at(view));
    auto              png   = std::make_shared<std::string>();
    json info = run_on_ui(
        [plate, size, angle, png] {
            Plater        *plater = plater_or_throw();
            PartPlateList &pl     = plater->get_partplate_list();
            const int      idx    = plate == 0 ? pl.get_curr_plate_index() : plate - 1;
            if (idx < 0 || idx >= pl.get_plate_count()) throw ToolError("no such plate");
            ThumbnailData data = plater->mcp_render_plate(idx, unsigned(size), unsigned(size), angle);
            if (!data.is_valid()) throw ToolError("could not render the plate (is the 3D view available?)");
            *png = encode_png(data);
            return json{{"plate", idx + 1}};
        },
        30);
    ToolResult r;
    r.add_text("Plate " + std::to_string(info["plate"].get<int>()) + ", " + view + " view of the 3D editor (models, not toolpaths).");
    r.add_image(*png, "image/png");
    return r;
}

static ToolResult tool_export_gcode(const json &args)
{
    // The file at path is replaced: k_gcode_path takes only a .gcode path, which is not the project or a model.
    const std::string path = k_gcode_path.read(args);
    run_on_ui(
        [path] {
            Plater *plater = plater_or_throw();
            if (!plater->get_partplate_list().get_curr_plate()->is_slice_result_valid())
                throw ToolError("slice the plate first");
            if (plater->is_export_gcode_scheduled()) throw ToolError("Another export is still running. Try again in a moment.");
            // Remove the old file, so that a file at path after the export is the new one.
            boost::system::error_code ec;
            fs::remove(path, ec);
            if (ec) throw ToolError("Cannot replace " + path + ": " + ec.message());
            plater->export_gcode(fs::path(path));
            return json();
        },
        120);
    // The file is written in the background.
    wait_until([] { return !wxGetApp().plater()->is_export_gcode_scheduled(); }, 120, 200, "The export is still running after 120 s.");
    if (!fs::exists(path)) throw ToolError("QIDI Studio did not write the G-code. Look at the notifications in the QIDI Studio window.");
    return ToolResult::json({{"exported", path}});
}

static ToolResult tool_save_project(const json &args)
{
    const std::string path = k_project_path.read(args);
    return ToolResult::json(run_on_ui(
        [path] {
            Plater *plater = plater_or_throw();
            fs::path out = path.empty() ? fs::path(into_u8(plater->get_project_filename(".3mf"))) : fs::path(path);
            if (out.empty()) throw ToolError("This project has no file yet: give a path ending in .3mf");
            if (plater->export_3mf(out) < 0) throw ToolError("saving failed");
            return json{{"saved", out.string()}};
        },
        120));
}

// ---------------------------------------------------------------------------------------------------------------
// Registration

void register_slicer_tools(std::vector<Tool> &t)
{
    t.push_back({"get_status",
                 "Overview of QIDI Studio right now: selected printer, print (process) and filament presets per slot, "
                 "unsaved setting changes, plates, objects with size/position, and whether the plate is sliced.",
                 input_schema(), tool_get_status, true});
    t.push_back({"list_presets", "List presets of one type. By default only those compatible with the selected printer.",
                 input_schema({k_preset_type, k_preset_search, k_include_incompatible}), tool_list_presets, true});
    t.push_back({"select_preset",
                 "Select a printer, print (process) or filament preset. For multi-colour prints pass filament (1-based slot) "
                 "to change one filament slot. Switching drops unsaved edits of the presets (reported in the reply).",
                 input_schema({k_preset_type, k_preset_name, k_preset_filament}),
                 tool_select_preset});
    t.push_back({"get_config",
                 "Read settings. With keys: exact values plus help text, allowed values and limits. With search: settings "
                 "whose key or label contains the word. With neither: common settings and what was changed. Filament settings "
                 "are those of filament (default 1).",
                 input_schema({k_config_keys, k_config_search, k_filament}),
                 tool_get_config, true});
    t.push_back({"set_config",
                 "Change settings, e.g. {\"changes\": {\"wall_loops\": 3, \"sparse_infill_density\": \"20%\"}}. Each key goes "
                 "to the preset that owns it (print, filament or printer) as an unsaved edit, like editing in the GUI. "
                 "Filament settings change the preset of filament (default 1), and so every filament with that preset. "
                 "Only one filament preset holds unsaved edits: those of another filament's preset are dropped "
                 "(reported in the reply).",
                 input_schema({k_changes, k_filament}),
                 tool_set_config});
    t.push_back({"load_model", "Add models from files on this computer (.stl, .3mf, .obj, .step, .stp, .amf) to the plate.",
                 input_schema({k_model_path, k_model_paths}),
                 tool_load_model});
    t.push_back({"list_objects", "Objects on the plates with index, size, position, rotation, scale, plate and filament.",
                 input_schema(), tool_list_objects, true});
    t.push_back({"transform_object",
                 "Move, rotate or scale one object. position [x,y] in mm on the bed (a third number is not used), move "
                 "[dx,dy,dz] in mm, rotate [x,y,z] in degrees about the world axes (added to the current rotation), scale a "
                 "factor or [x,y,z]. After rotating or scaling, the object is put back on the bed; then a move dz below 0 "
                 "sinks it into the bed. QIDI Studio drops a lifted object back onto the bed.",
                 input_schema({k_object, k_position, k_move, k_rotate, k_scale}),
                 tool_transform_object});
    t.push_back({"delete_object", "Remove one object from the project.", input_schema({k_object}),
                 tool_delete_object, false, true});
    t.push_back({"set_object_filament", "Print a whole object with one filament slot (1-based).",
                 input_schema({k_object, k_object_filament}),
                 tool_set_object_filament});
    t.push_back({"set_filament_count", "Set how many filament slots the project uses (for multi-colour prints with the QIDI Box).",
                 input_schema({k_filament_count}), tool_set_filament_count});
    t.push_back({"set_filament_colour", "Set the display colour of a filament slot (#RRGGBB), e.g. to match the spool in the Box.",
                 input_schema({k_colour_filament, k_colour}),
                 tool_set_filament_colour});
    t.push_back({"arrange", "Arrange all objects on the plates (QIDI Studio's Arrange). Waits until done.", input_schema(), tool_arrange});
    t.push_back({"auto_orient", "Turn objects to good print orientations (QIDI Studio's Auto orient). Waits until done.", input_schema(),
                 tool_auto_orient});
    t.push_back({"list_plates", "Plates with their objects and whether each is sliced.", input_schema(), tool_list_plates, true});
    t.push_back({"select_plate", "Make a plate (1-based) the current one.", input_schema({k_select_plate}),
                 tool_select_plate});
    t.push_back({"slice",
                 "Slice the current plate (or the given 1-based plate) and wait. Returns print time, filament use per slot, "
                 "filament changes and the warnings the QIDI Studio window shows for the plate, each with source, level "
                 "(note, warning, serious_warning, error), message and, where known, object. Sources: 'print' and 'object' "
                 "(slicing steps, for example floating regions, overhangs or empty layers), 'validate' (the settings check "
                 "before slicing), 'gcode' (G-code path conflicts and the G-code checks, for example bed temperature; 'key' "
                 "is the raw check name). Not covered: the 3D view's own checks, for example an object outside the plate or "
                 "filament and nozzle checks.",
                 input_schema({k_plate, k_slice_timeout}), tool_slice});
    t.push_back({"render_plate", "A picture of a plate in the 3D editor, to check placement and orientation.",
                 input_schema({k_plate, k_view, k_render_size}),
                 tool_render_plate, true});
    t.push_back({"export_gcode", "Save the sliced G-code of the current plate to a .gcode file on this computer (replaces the file).",
                 input_schema({k_gcode_path}), tool_export_gcode});
    t.push_back({"save_project", "Save the project as .3mf (to its own file, or to a path ending in .3mf).", input_schema({k_project_path}),
                 tool_save_project});
}

}}} // namespace Slic3r::GUI::Mcp
