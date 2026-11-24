// LanPrinter tests: the printer is a fake transport that records each request and answers with replies written in
// the shapes that the Q2's Moonraker and QIDI's firmware send. The replies are written by hand: the Q2 was off when
// these tests were made, so none of them is a recording.
#include <catch_main.hpp>

#include "slic3r/Utils/LanPrinter.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using json = nlohmann::json;

namespace {

const std::string k_host = "http://h:7125/";
const std::string k_base = "http://h:7125";

// New firmware: hex colours in save_variables, the type in multi_color_controller. One Box: slots 1 and 2 loaded,
// slot 2's type empty (the printer does not report it), slots 3 and 4 empty.
const std::string k_box_new = R"({"result": {"eventtime": 1.0, "status": {
  "save_variables": {"variables": {"box_count": 1, "auto_reload_detect": 0,
    "filament_slot0": 3, "vendor_slot0": 1, "color_slot0": ["#FF0000"],
    "filament_slot1": 9, "vendor_slot1": 0, "color_slot1": ["#00FF00"]}},
  "multi_color_controller": {"auto_reload_detect": 1, "box": {"box_count": 1,
    "box0_filament_info_0_filament_type": "PLA", "box0_filament_info_1_filament_type": ""}},
  "box_stepper slot0": {"runout_button": 0}, "box_stepper slot1": {"runout_button": 0},
  "box_stepper slot2": {"runout_button": 1}, "box_stepper slot3": {"runout_button": 1}}}})";

// Old firmware: the type and the colour are indexes into the printer's filament list.
const std::string k_box_old = R"({"result": {"eventtime": 1.0, "status": {
  "save_variables": {"variables": {"box_count": 1, "filament_slot0": 3, "color_slot0": 2, "vendor_slot0": 0}},
  "box_stepper slot0": {"runout_button": 0}}}})";

const std::string k_filament_list = "[colordict]\n1 = #FFFFFF\n2 = #000000\n\n[fila3]\nfilament = PETG\n";

const std::string k_status = R"({"result": {"eventtime": 1.0, "status": {
  "print_stats": {"state": "printing", "filename": "cube.gcode", "print_duration": 61.9, "message": "",
                  "info": {"current_layer": 3, "total_layer": 40}},
  "virtual_sdcard": {"progress": 0.256, "is_active": true},
  "extruder": {"temperature": 219.6, "target": 220.0},
  "heater_bed": {"temperature": 59.4, "target": 60.0}}}})";

// Records each request and answers with the first reply whose URL prefix matches, else 200 {"result": "ok"}.
struct FakePrinter
{
    std::vector<LanRequest>                       sent;
    std::vector<std::pair<std::string, LanReply>> replies;

    void answer(const std::string &path_prefix, LanReply reply) { replies.emplace_back(k_base + path_prefix, std::move(reply)); }

    LanPrinter printer(const std::string &api_key = "key")
    {
        return LanPrinter("Q2", k_host, api_key, [this](const LanRequest &req) {
            sent.push_back(req);
            for (const auto &[prefix, reply] : replies)
                if (req.url.rfind(prefix, 0) == 0)
                    return reply;
            return LanReply{200, R"({"result": "ok"})", ""};
        });
    }

    // Each POST as one line: the G-code script, or the URL.
    std::vector<std::string> posts() const
    {
        std::vector<std::string> out;
        for (const LanRequest &r : sent)
            if (r.method == "POST")
                out.push_back(r.url == k_base + "/printer/gcode/script" ? json::parse(r.json_body).at("script").get<std::string>() : r.url);
        return out;
    }
};

LanStartPrint two_filament_job()
{
    LanStartPrint job;
    job.file_path          = "/tmp/cube.gcode.3mf";
    job.file_name          = "cube.gcode.3mf";
    job.plate              = 2;
    job.used_filaments     = {1, 2};
    job.filament_types     = {"PLA", "PETG"};
    job.use_box            = true;
    job.box_slots          = {1, 2};
    job.box_printer        = true;
    job.supports_timelapse = true;
    job.supports_cooler    = true;
    return job;
}

} // namespace

TEST_CASE("LanPrinter reads the Box", "[lan_printer]")
{
    FakePrinter fake;

    SECTION("new firmware: hex colours and types from the printer")
    {
        fake.answer("/printer/objects/query?save_variables", {200, k_box_new, ""});
        fake.answer("/server/files/config/officiall_filas_list.cfg", {200, k_filament_list, ""});
        const LanBox box = fake.printer().box();
        CHECK(box.box_count == 1);
        CHECK(box.auto_reload_detect == 1);
        REQUIRE(box.slots.size() == 17);
        CHECK(box.slots[0].loaded);
        CHECK(box.slots[0].qidi);
        CHECK(box.slots[0].type == "PLA");
        CHECK(box.slots[0].colour == "#FF0000");
        CHECK(box.slots[1].loaded);
        CHECK(box.slots[1].type.empty()); // not reported, and index 9 is not in the filament list
        CHECK_FALSE(box.slots[2].loaded);
        CHECK(box.filament_list_error.empty());
        CHECK(fake.sent.size() == 2);
        CHECK(fake.sent[0].api_key == "key");
    }

    SECTION("old firmware: type and colour from the printer's filament list")
    {
        fake.answer("/printer/objects/query?save_variables", {200, k_box_old, ""});
        fake.answer("/server/files/config/officiall_filas_list.cfg", {200, k_filament_list, ""});
        const LanBox box = fake.printer().box();
        CHECK(box.slots[0].loaded);
        CHECK_FALSE(box.slots[0].qidi);
        CHECK(box.slots[0].type == "PETG");
        CHECK(box.slots[0].colour == "#000000");
    }

    SECTION("a filament list that cannot be read is reported, and the Box is still read")
    {
        fake.answer("/printer/objects/query?save_variables", {200, k_box_new, ""});
        fake.answer("/server/files/config/officiall_filas_list.cfg", {404, "", ""});
        const LanBox box = fake.printer().box();
        CHECK(box.filament_list_error == "HTTP 404");
        CHECK(box.slots[0].type == "PLA");
    }

    SECTION("box_count sends one query")
    {
        fake.answer("/printer/objects/query?save_variables", {200, k_box_new, ""});
        CHECK(fake.printer().box_count() == 1);
        CHECK(fake.sent.size() == 1);
    }
}

TEST_CASE("LanPrinter starts a print with the Print dialog's commands", "[lan_printer]")
{
    FakePrinter fake;
    fake.answer("/printer/objects/query?save_variables", {200, k_box_new, ""});
    fake.answer("/server/files/config/officiall_filas_list.cfg", {200, k_filament_list, ""});
    LanStartPrint job = two_filament_job();

    SECTION("the whole sequence, in order")
    {
        job.timelapse = true;
        job.cooler    = false;
        const std::vector<std::string> warnings = fake.printer().start_print(job);
        CHECK(fake.posts() == std::vector<std::string>{
                                  "SAVE_VARIABLE VARIABLE=enable_box VALUE=1",
                                  "SAVE_VARIABLE VARIABLE=value_t0 VALUE=\"'slot0'\"",
                                  "SAVE_VARIABLE VARIABLE=value_t1 VALUE=\"'slot1'\"",
                                  "G31",
                                  "http://h/machine/timelapse/settings?enabled=true",
                                  "SAVE_VARIABLE VARIABLE=enable_polar_cooler VALUE=0",
                                  k_base + "/server/files/upload",
                              });
        const LanRequest &upload = fake.sent.back();
        CHECK(upload.form == std::vector<std::pair<std::string, std::string>>{{"root", "gcodes"}, {"print", "true"}, {"plateindex", "2"}});
        CHECK(upload.file_path == "/tmp/cube.gcode.3mf");
        CHECK(upload.file_name == "cube.gcode.3mf");
        CHECK(upload.timeout_s == 600);
        // Filament 2 (PETG) is in slot 2, whose type the printer does not report.
        REQUIRE(warnings.size() == 1);
        CHECK(warnings[0].find("Box 1 slot 2: the printer does not report its filament type") == 0);
    }

    SECTION("a slot that holds another filament type gives a warning")
    {
        job.used_filaments = {1};
        job.filament_types = {"Generic PETG"};
        job.box_slots      = {1};
        CHECK(fake.printer().start_print(job) == std::vector<std::string>{"Filament 1 is PETG but Box 1 slot 1 holds PLA"});
    }

    SECTION("a slot type that is missing because the filament list could not be read gives the read error")
    {
        fake.replies[1].second = {404, "", ""}; // the filament list
        CHECK(fake.printer().start_print(job) ==
              std::vector<std::string>{"Box 1 slot 2: the printer's filament list could not be read (HTTP 404), so it is not checked "
                                       "against PETG (filament 2)"});
    }

    SECTION("a Box mapping that does not fit sends nothing")
    {
        job.box_slots = {1, 3}; // slot 3 is empty
        CHECK_THROWS_AS(fake.printer().start_print(job), std::invalid_argument);
        job.box_slots = {1, 5}; // one Box has 4 slots
        CHECK_THROWS_AS(fake.printer().start_print(job), std::invalid_argument);
        job.box_slots = {1}; // filament 2 has no slot
        CHECK_THROWS_AS(fake.printer().start_print(job), std::invalid_argument);
        CHECK(fake.posts().empty());
    }

    SECTION("timelapse and the cooler are not sent to a printer whose preset has none")
    {
        job.supports_timelapse = false;
        job.supports_cooler    = false;
        job.timelapse          = true;
        job.cooler             = true;
        std::vector<std::string> warnings = fake.printer().start_print(job);
        CHECK(std::count(warnings.begin(), warnings.end(), "This printer has no timelapse, so timelapse was not set") == 1);
        CHECK(std::count(warnings.begin(), warnings.end(), "This printer has no polar cooler, so the cooler was not set") == 1);
        for (const std::string &p : fake.posts()) {
            CHECK(p.find("timelapse") == std::string::npos);
            CHECK(p.find("polar_cooler") == std::string::npos);
        }

        fake.sent.clear();
        job.timelapse = false;
        job.cooler    = false;
        warnings      = fake.printer().start_print(job);
        CHECK(warnings.size() == 1); // only the slot type warning
        CHECK(fake.posts().size() == 5);
    }

    SECTION("without the Box, on a printer with no Box")
    {
        job.use_box     = false;
        job.box_printer = false;
        job.bed_leveling = false;
        CHECK(fake.printer().start_print(job).empty());
        CHECK(fake.posts() == std::vector<std::string>{"G32", k_base + "/server/files/upload"});
    }
}

TEST_CASE("LanPrinter tells the error kinds apart", "[lan_printer]")
{
    FakePrinter fake;

    SECTION("the printer's own error message is a refusal")
    {
        fake.answer("/printer/gcode/script", {400, R"({"error": {"code": 400, "message": "Unknown command: \"FOO\""}})", ""});
        try {
            fake.printer().gcode("FOO");
            FAIL("no error");
        } catch (const LanPrinterError &e) {
            CHECK(e.kind == LanPrinterError::Kind::Refused);
            CHECK(e.detail == "Unknown command: \"FOO\"");
            CHECK(std::string(e.what()) == "The printer 'Q2' refused 'FOO': Unknown command: \"FOO\"");
        }
    }

    SECTION("no answer")
    {
        fake.answer("/printer/objects/query", {0, "", "Couldn't connect to server"});
        try {
            fake.printer().status();
            FAIL("no error");
        } catch (const LanPrinterError &e) {
            CHECK(e.kind == LanPrinterError::Kind::NoAnswer);
            CHECK(e.http_status == 0);
            CHECK(std::string(e.what()) ==
                  "The printer 'Q2' did not answer '/printer/objects/query': Couldn't connect to server. Is it on and on the network?");
        }
    }

    SECTION("an HTTP error with no message")
    {
        fake.answer("/server/webcams/list", {503, "Service Unavailable", ""});
        try {
            fake.printer().webcams();
            FAIL("no error");
        } catch (const LanPrinterError &e) {
            CHECK(e.kind == LanPrinterError::Kind::NoAnswer);
            CHECK(e.http_status == 503);
        }
    }

    SECTION("an answer of another shape is a bad reply, not a bad argument")
    {
        fake.answer("/printer/objects/query", {200, R"({"result": {"status": {"print_stats": {"state": 7}}}})", ""});
        try {
            fake.printer().status();
            FAIL("no error");
        } catch (const LanPrinterError &e) {
            CHECK(e.kind == LanPrinterError::Kind::BadReply);
        }
        fake.answer("/server/webcams/list", {200, "not json", ""});
        CHECK_THROWS_AS(fake.printer().webcams(), LanPrinterError);
    }
}

TEST_CASE("LanPrinter reads the status, the console and the camera", "[lan_printer]")
{
    FakePrinter fake;

    SECTION("status")
    {
        fake.answer("/printer/objects/query", {200, k_status, ""});
        const LanPrintStatus s = fake.printer().status();
        CHECK(s.state == "printing");
        CHECK(s.file == "cube.gcode");
        CHECK(s.printing_s == 61);
        CHECK(s.layer == 3.0);
        CHECK(s.progress_pct == 26);
        REQUIRE(s.extruder);
        CHECK(s.extruder->temp == 220);
        CHECK(s.extruder->target == 220);
        REQUIRE(s.bed);
        CHECK(s.bed->temp == 59);
    }

    SECTION("console")
    {
        fake.answer("/server/gcode_store", {200, R"({"result": {"gcode_store": [
            {"message": "PAUSE", "time": 10.5, "type": "command"},
            {"message": "// Please load filament", "time": 11.0, "type": "response"}]}})", ""});
        const std::vector<LanConsoleLine> lines = fake.printer().console(2);
        REQUIRE(lines.size() == 2);
        CHECK(lines[1].type == "response");
        CHECK(lines[1].time == 11.0);
        CHECK(fake.sent[0].url == k_base + "/server/gcode_store?count=2");
    }

    SECTION("a camera picture: relative and localhost URLs go to the printer, with no API key")
    {
        fake.printer().snapshot("/webcam/?action=snapshot", 100);
        fake.printer().snapshot("http://127.0.0.1:8080/?action=snapshot", 100);
        fake.printer().snapshot("http://cam.lan/snap.jpg", 100);
        REQUIRE(fake.sent.size() == 3);
        CHECK(fake.sent[0].url == "http://h/webcam/?action=snapshot");
        CHECK(fake.sent[1].url == "http://h:8080/?action=snapshot");
        CHECK(fake.sent[2].url == "http://cam.lan/snap.jpg");
        CHECK(fake.sent[0].api_key.empty());
        CHECK(fake.sent[0].size_limit == 100);
    }
}

TEST_CASE("Filament types and slot labels", "[lan_printer]")
{
    CHECK(normalize_filament_type(" Generic PETG ") == "PETG");
    CHECK(normalize_filament_type("PLA Matte") == "PLA");
    CHECK(normalize_filament_type("Nylon") == "PA");
    CHECK(normalize_filament_type("").empty());
    CHECK(box_slot_label(0) == "Box 1 slot 1");
    CHECK(box_slot_label(6) == "Box 2 slot 3");
}
