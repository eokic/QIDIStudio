#include "QIDIDeviceApi.hpp"

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/Utils/LanPrinter.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r {

namespace {

LanPrinter lan_printer(const std::string &url, const std::string &api_key) { return LanPrinter(url, url, api_key, lan_http_transport()); }

bool get_body(const std::string &url, const std::string &path, std::string &body)
{
    body.clear();
    try {
        body = lan_printer(url, "").get(path);
        return true;
    } catch (const LanPrinterError &e) {
        BOOST_LOG_TRIVIAL(info) << "QIDIDeviceApi: GET " << path << " failed: " << e.detail;
        return false;
    }
}

wxString failure(const LanPrinterError &e)
{
    if (e.kind == LanPrinterError::Kind::BadReply)
        return _L("The printer sent an unexpected answer.");
    return _L("Failed to connect to the printer.") + " " + wxString::FromUTF8(e.detail);
}

} // namespace

bool QIDIDeviceApi::get_file_list(const std::string &url, std::string &body) { return get_body(url, "/api/qidiclient/files/list", body); }

bool QIDIDeviceApi::get_timelapse_directory(const std::string &url, std::string &body)
{
    return get_body(url, "/server/files/directory?root=timelapse&path=timelapse&extended=true", body);
}

bool QIDIDeviceApi::get_system_info(const std::string &url, std::string &body) { return get_body(url, "/machine/system_info", body); }

bool QIDIDeviceApi::get_client_info(const std::string &, std::string &body)
{
    body.clear();
    return false;
}

bool QIDIDeviceApi::get_box_state(wxString &msg, const std::string &url, const std::string &api_key)
{
    try {
        return lan_printer(url, api_key).box_count() > 0;
    } catch (const LanPrinterError &e) {
        msg = failure(e);
        return false;
    }
}

GUI::Box_info QIDIDeviceApi::get_box_info(wxString &msg, const std::string &url, const std::string &api_key)
{
    GUI::Box_info info;
    LanBox        box;
    try {
        box = lan_printer(url, api_key).box();
    } catch (const LanPrinterError &e) {
        msg = failure(e);
    }
    const size_t n = 16 + 1; // 4 Boxes with 4 slots, then the external spool
    info.slot_state.assign(n, 0);
    info.slot_id.assign(n, 0);
    info.filament_vendor.assign(n, 0);
    info.filament_index.assign(n, 0);
    info.filament_id.assign(n, "");
    info.filament_colors.assign(n, "");
    info.filament_type.assign(n, "");
    info.box_count          = box.box_count;
    info.auto_reload_detect = box.auto_reload_detect;
    for (size_t i = 0; i < n; ++i) {
        info.slot_id[i] = int(i);
        if (i >= box.slots.size())
            continue;
        const LanBoxSlot &s     = box.slots[i];
        info.slot_state[i]      = s.loaded ? 1 : 0;
        info.filament_vendor[i] = s.qidi ? 1 : 0;
        info.filament_index[i]  = s.filament_index;
        info.filament_colors[i] = s.colour;
        info.filament_type[i]   = s.type;
    }
    if (!box.filament_list_error.empty())
        msg = _L("Failed to read the printer's filament list.") + " " + wxString::FromUTF8(box.filament_list_error);
    return info;
}

void QIDIDeviceApi::get_color_filament_str(wxString &, GUI::Box_info &info, const std::string &, const std::string &)
{
    const size_t n = info.slot_state.size();
    info.filament_type.resize(n);
    for (size_t i = 0; i < n; ++i)
        if (info.filament_type[i].empty() && info.slot_state[i])
            info.filament_type[i] = "PLA";
}

} // namespace Slic3r
