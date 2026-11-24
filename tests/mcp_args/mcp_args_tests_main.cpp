// MCP argument tests: each kind makes the schema entry that the AI client reads, and reads the value with the same
// rules. The arguments here are the ones the tools use, with their real ranges.
#include <catch_main.hpp>

#include "slic3r/GUI/Mcp/McpArgs.hpp"

#include <string>

using namespace Slic3r::GUI::Mcp;
using json = nlohmann::json;

namespace {

// The ToolError text of fn, or "" when it does not throw.
template<class Fn> std::string error_of(Fn fn)
{
    try {
        fn();
    } catch (const ToolError &e) {
        return e.what();
    }
    return "";
}

} // namespace

TEST_CASE("The schema comes from the argument kinds", "[mcp_args]")
{
    const IntArg    plate{"plate", "Plate, 1-based; not given or 0: the current plate", 0, INT_MAX, 0};
    const IntArg    slot{"slot", "Box slot", 1, 16};
    const VecArg    position{"position", "[x,y] in mm", 2};
    const VecArg    scale{"scale", "A factor, or [x,y,z]", 3, true};
    const StringArg path{"path", "ending in .gcode", true, ".gcode"};
    const json      s = input_schema({plate, slot, position, scale, path});

    CHECK(s["type"] == "object");
    CHECK(s["required"] == json::array({"slot", "path"}));
    CHECK(s["properties"]["plate"] == json{{"type", "integer"}, {"minimum", 0}, {"default", 0},
                                           {"description", "Plate, 1-based; not given or 0: the current plate"}});
    CHECK(s["properties"]["slot"]["maximum"] == 16);
    CHECK(s["properties"]["position"]["minItems"] == 2);
    CHECK(s["properties"]["position"]["maxItems"] == 3);
    CHECK(s["properties"]["scale"]["anyOf"][0] == json{{"type", "number"}});
    CHECK(input_schema() == json{{"type", "object"}, {"properties", json::object()}});
}

TEST_CASE("Whole numbers", "[mcp_args]")
{
    const IntArg slot{"slot", "Box slot", 1, 16};
    const IntArg timeout{"timeout_s", "Seconds", 10, 3600, 900};
    const IntArg preset_filament{"filament", "Filament slot", 1, INT_MAX, 0}; // 0: not given

    CHECK(slot.read(json{{"slot", 3}}) == 3);
    CHECK(slot.read(json{{"slot", 3.0}}) == 3);
    CHECK(timeout.read(json::object()) == 900);
    CHECK(timeout.read(json{{"timeout_s", nullptr}}) == 900);
    CHECK(preset_filament.read(json::object()) == 0);

    CHECK(error_of([&] { slot.read(json::object()); }) == "'slot' is required: Box slot");
    CHECK(error_of([&] { slot.read(json{{"slot", 17}}); }) == "'slot' must be a whole number from 1 to 16");
    CHECK(error_of([&] { slot.read(json{{"slot", 1.5}}); }) == "'slot' must be a whole number from 1 to 16");
    CHECK(error_of([&] { slot.read(json{{"slot", "3"}}); }) == "'slot' must be a whole number from 1 to 16");
    // Out of range is refused, not changed to the nearest limit.
    CHECK(error_of([&] { timeout.read(json{{"timeout_s", 99999}}); }) == "'timeout_s' must be a whole number from 10 to 3600");
    CHECK(error_of([&] { preset_filament.read(json{{"filament", -2}}); }) == "'filament' must be a whole number of 1 or more");
}

TEST_CASE("Lists of whole numbers", "[mcp_args]")
{
    const IntListArg box_slots{"box_slots", "Box slots; 0: not used", 0, 16};

    CHECK(box_slots.read(json::object()).empty());
    CHECK(box_slots.read(json{{"box_slots", {3, 0, 1}}}) == std::vector<int>{3, 0, 1});
    const std::string form = "'box_slots' must be a list of whole numbers from 0 to 16";
    CHECK(error_of([&] { box_slots.read(json{{"box_slots", 3}}); }) == form);
    CHECK(error_of([&] { box_slots.read(json{{"box_slots", {1, 17}}}); }) == form);
    CHECK(error_of([&] { box_slots.read(json{{"box_slots", {{"a", 3}}}}); }) == form);
}

TEST_CASE("Vectors", "[mcp_args]")
{
    const VecArg position{"position", "[x,y]", 2};
    const VecArg rotate{"rotate", "[x,y,z]", 3};
    const VecArg scale{"scale", "A factor, or [x,y,z]", 3, true};

    CHECK_FALSE(position.read(json::object()));
    CHECK(*position.read(json{{"position", {100, 120}}}) == std::array<double, 3>{100, 120, 0});
    CHECK(*position.read(json{{"position", {100, 120, 5}}}) == std::array<double, 3>{100, 120, 5});
    CHECK(*scale.read(json{{"scale", 2}}) == std::array<double, 3>{2, 2, 2});

    CHECK(error_of([&] { position.read(json{{"position", {100}}}); }) == "'position' must be 2 or 3 numbers [x,y] or [x,y,z]");
    CHECK(error_of([&] { rotate.read(json{{"rotate", {0, 90}}}); }) == "'rotate' must be 3 numbers [x,y,z]");
    // Before: a non-number item gave "bad argument: [json.exception.type_error.302] type must be number, but is string".
    CHECK(error_of([&] { rotate.read(json{{"rotate", {0, 0, "90"}}}); }) == "'rotate' must be 3 numbers [x,y,z]");
    CHECK(error_of([&] { rotate.read(json{{"rotate", 90}}); }) == "'rotate' must be 3 numbers [x,y,z]");
    CHECK(error_of([&] { scale.read(json{{"scale", "2"}}); }) == "'scale' must be a number or 3 numbers [x,y,z]");
}

TEST_CASE("Strings, choices and true or false", "[mcp_args]")
{
    const EnumArg       view{"view", "", {"iso", "top"}, "iso"};
    const EnumArg       action{"action", "", {"load", "unload"}};
    const StringArg     gcode{"path", "ending in .gcode", true, ".gcode"};
    const StringArg     project{"path", "ending in .3mf", false, ".3mf"};
    const StringListArg keys{"keys", "Setting keys"};
    const BoolArg       leveling{"bed_leveling", "", true};
    const BoolArg       timelapse{"timelapse", ""};
    const BoolArg       confirm{"confirm", "true only after the user agreed", std::nullopt, true};

    CHECK(view.read(json::object()) == "iso");
    CHECK(error_of([&] { view.read(json{{"view", "side"}}); }) == "'view' must be one of iso, top");
    CHECK(error_of([&] { action.read(json::object()) ; }) == "'action' is required");

    CHECK(gcode.read(json{{"path", "/tmp/a.GCODE"}}) == "/tmp/a.GCODE");
    CHECK(error_of([&] { gcode.read(json{{"path", "/tmp/project.3mf"}}); }) == "'path' must end in .gcode");
    CHECK(error_of([&] { gcode.read(json{{"path", ""}}); }) == "'path' is required: ending in .gcode");
    CHECK(project.read(json::object()).empty());
    CHECK(error_of([&] { project.read(json{{"path", 3}}); }) == "'path' must be a string");

    CHECK(keys.read(json{{"keys", {"layer_height"}}}) == std::vector<std::string>{"layer_height"});
    CHECK(error_of([&] { keys.read(json{{"keys", "layer_height"}}); }) == "'keys' must be a list of strings");
    CHECK(error_of([&] { keys.read(json{{"keys", {1}}}); }) == "'keys' must be a list of strings");

    CHECK(leveling.read(json::object()));
    CHECK_FALSE(timelapse.read_if_given(json{{"timelapse", nullptr}}));
    CHECK(timelapse.read_if_given(json{{"timelapse", false}}) == false);
    CHECK(error_of([&] { timelapse.read_if_given(json{{"timelapse", "true"}}); }) == "'timelapse' must be true or false");
    CHECK(input_schema({confirm})["required"] == json::array({"confirm"}));
    CHECK_FALSE(confirm.read_if_given(json::object()));
}

TEST_CASE("Arguments that a tool reads itself", "[mcp_args]")
{
    CHECK(arg_int(json::object(), "object", -1) == -1);
    CHECK(arg_int(json{{"object", 2}}, "object", -1) == 2);
    CHECK(error_of([] { arg_int(json{{"object", true}}, "object", -1); }) == "'object' must be a whole number");
}
