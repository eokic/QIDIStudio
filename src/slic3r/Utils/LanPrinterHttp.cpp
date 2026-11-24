// The production LanTransport: sends each LanPrinter request with curl (Http). Kept apart from LanPrinter.cpp so that
// the LanPrinter tests build without curl.
#include "LanPrinter.hpp"

#include "Http.hpp"

#include <boost/filesystem/path.hpp>

namespace Slic3r {

LanTransport lan_http_transport(std::function<bool()> stop)
{
    return [stop](const LanRequest &req) {
        LanReply r;
        if (stop && stop()) {
            r.error = "stopped";
            return r;
        }
        Http http = req.method == "POST" ? Http::post(req.url) : Http::get(req.url);
        if (!req.api_key.empty())
            http.header("X-Api-Key", req.api_key);
        if (!req.json_body.empty())
            http.header("Content-Type", "application/json").set_post_body(req.json_body);
        for (const auto &[name, value] : req.form)
            http.form_add(name, value);
        if (!req.file_path.empty())
            http.form_add_file("file", boost::filesystem::path(req.file_path), req.file_name);
        if (req.size_limit > 0)
            http.size_limit(req.size_limit);
        // A cancel from here calls neither on_complete nor on_error, so the error is set here.
        if (stop)
            http.on_progress([&r, stop](Http::Progress, bool &cancel) {
                if (stop()) {
                    cancel  = true;
                    r.error = "stopped";
                }
            });
        http.timeout_connect(5)
            .timeout_max(req.timeout_s)
            .on_complete([&r](std::string body, unsigned status) {
                r.body   = std::move(body);
                r.status = status;
            })
            // A curl error comes with status 0; an HTTP error status (400 and up) comes with no error text.
            .on_error([&r](std::string body, std::string error, unsigned status) {
                r.body   = std::move(body);
                r.status = status;
                r.error  = std::move(error);
            })
            .perform_sync();
        if (r.status == 0 && r.error.empty())
            r.error = "no answer"; // Http calls neither function for a 1xx or 3xx status
        return r;
    };
}

} // namespace Slic3r
