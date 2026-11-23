// Public stand-in for QIDI's unpublished src/slic3r/QIDI/QIDIDeviceApi.
// Used when QDT_RELEASE_TO_PUBLIC=0. Talks to a LAN printer through LanPrinter (slic3r/Utils/LanPrinter.hpp).
//
// `url` is the printer address as QIDI Studio stores it ("192.168.1.20", "http://192.168.1.20:7125", ...).
#ifndef slic3r_QIDIPublic_QIDIDeviceApi_hpp_
#define slic3r_QIDIPublic_QIDIDeviceApi_hpp_

#include <string>

#include <wx/string.h>

#include "QIDINetworkTypes.hpp"

namespace Slic3r {

class QIDIDeviceApi
{
public:
    // Each puts the response body in `body` and returns true on HTTP 2xx.
    static bool get_file_list(const std::string &url, std::string &body);           // printer's print files
    static bool get_timelapse_directory(const std::string &url, std::string &body); // timelapse folder
    static bool get_system_info(const std::string &url, std::string &body);         // /machine/system_info
    // The firmware's agent version. QIDI's endpoint for it is not public, so this reports false and
    // QIDI Studio keeps its default device settings.
    static bool get_client_info(const std::string &url, std::string &body);

    // QIDI Box. `msg` gets a readable reason when a call fails.
    bool          get_box_state(wxString &msg, const std::string &url, const std::string &api_key);
    // The Box slots with their filament type and colour.
    GUI::Box_info get_box_info(wxString &msg, const std::string &url, const std::string &api_key);
    // Called after get_box_info, as QIDI's own version needs. Here it only gives a loaded slot whose type the
    // printer does not report the type "PLA", for the Box sync dialog. It sends no request.
    void get_color_filament_str(wxString &msg, GUI::Box_info &info, const std::string &url, const std::string &api_key);
};

} // namespace Slic3r

#endif
