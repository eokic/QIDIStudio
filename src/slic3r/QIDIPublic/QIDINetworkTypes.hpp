// Public stand-in for QIDI's unpublished src/slic3r/QIDI/QIDINetworkTypes.hpp.
// Used when QDT_RELEASE_TO_PUBLIC=0. Only the types that code outside the QIDI cloud module uses.
#ifndef slic3r_QIDIPublic_QIDINetworkTypes_hpp_
#define slic3r_QIDIPublic_QIDINetworkTypes_hpp_

#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

// QIDI Box contents as read from a printer. Each vector has one entry per Box slot (Box 1 is 0..3,
// up to 4 Boxes), then one last entry for the external spool.
struct Box_info
{
    int                      box_count { 0 };
    int                      auto_reload_detect { 0 };
    std::vector<int>         slot_state;      // 1 = filament loaded
    std::vector<int>         slot_id;
    std::vector<int>         filament_vendor; // 1 = QIDI, 0 = generic
    std::vector<int>         filament_index;  // index in the printer's filament list
    std::vector<std::string> filament_id;
    std::vector<std::string> filament_colors;
    std::vector<std::string> filament_type;
};

}} // namespace Slic3r::GUI

#endif
