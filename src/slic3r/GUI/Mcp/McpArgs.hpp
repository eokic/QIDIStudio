// Tool arguments. Each argument kind makes its input_schema entry and reads its value from the call, so the AI client
// reads the same rules (type, range, size, default, required) that the tool applies. A wrong value throws a ToolError
// that names the argument and its form.
//
// No GUI types and no wx here: tests/mcp_args checks this module. Checks that need the project, the presets or the
// printer (the number of filaments or plates, object names, the Box count) stay in the tools.
#ifndef slic3r_GUI_McpArgs_hpp_
#define slic3r_GUI_McpArgs_hpp_

#include "McpServer.hpp"

#include <array>
#include <climits>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// What every argument has: its name, its JSON schema and whether the call must give it.
struct Arg
{
    std::string    name;
    nlohmann::json schema;
    bool           required = false;
};

// A tool's input schema: an object with these arguments.
nlohmann::json input_schema(std::initializer_list<std::reference_wrapper<const Arg>> args = {});

// A whole number from min to max. Absent or null: def (which may be outside the range, to mean "not given").
// With no def, the argument is required.
struct IntArg : Arg
{
    IntArg(std::string name, std::string description, int min, int max = INT_MAX, std::optional<int> def = std::nullopt);
    int read(const nlohmann::json &args) const;

    int                min, max;
    std::optional<int> def;
};

// A list of whole numbers from min to max. Absent or null: an empty list.
struct IntListArg : Arg
{
    IntListArg(std::string name, std::string description, int min, int max);
    std::vector<int> read(const nlohmann::json &args) const;

    int min, max;
};

// min_size to 3 numbers, or with number_ok also one number for all three. A missing third number is 0.
// Absent or null: not given.
struct VecArg : Arg
{
    VecArg(std::string name, std::string description, size_t min_size, bool number_ok = false);
    std::optional<std::array<double, 3>> read(const nlohmann::json &args) const;

    size_t min_size;
    bool   number_ok;
};

// One of the values. Absent or null: def; with no def, the argument is required.
struct EnumArg : Arg
{
    EnumArg(std::string name, std::string description, std::vector<std::string> values, std::optional<std::string> def = std::nullopt);
    std::string read(const nlohmann::json &args) const;

    std::vector<std::string>   values;
    std::optional<std::string> def;
};

// A string. Absent, null or empty: "", or refused when required. With a suffix, a given value must end in it
// (letter case does not matter), for example ".gcode".
struct StringArg : Arg
{
    StringArg(std::string name, std::string description, bool required = false, std::string suffix = {});
    std::string read(const nlohmann::json &args) const;

    std::string suffix;
};

// A list of strings. Absent or null: an empty list.
struct StringListArg : Arg
{
    StringListArg(std::string name, std::string description);
    std::vector<std::string> read(const nlohmann::json &args) const;
};

// true or false. read(): absent or null gives def, or is refused with no def. read_if_given(): absent or null gives
// nothing, for a default that depends on other arguments. required only marks the argument in the schema.
struct BoolArg : Arg
{
    BoolArg(std::string name, std::string description, std::optional<bool> def = std::nullopt, bool required = false);
    bool                read(const nlohmann::json &args) const;
    std::optional<bool> read_if_given(const nlohmann::json &args) const;

    std::optional<bool> def;
};

// For an argument that a tool reads itself (an object by index or name): an absent or null key gives def, a value
// that is not a whole number throws ToolError.
int arg_int(const nlohmann::json &args, const char *key, int def);

}}} // namespace Slic3r::GUI::Mcp

#endif
