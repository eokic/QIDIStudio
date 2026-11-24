#include <catch_main.hpp>

#include "slic3r/Utils/Http.hpp"

TEST_CASE("Check SSL certificates paths", "[Http][NotWorking]") {
    
    Slic3r::Http g = Slic3r::Http::get("https://github.com/");
    
    unsigned status = 0;
    g.on_error([&status](std::string, std::string, unsigned http_status) {
        status = http_status;
    });
    
    g.on_complete([&status](std::string /* body */, unsigned http_status){
        status = http_status;
    });
    
    g.perform_sync();
    
    REQUIRE(status == 200);
}

TEST_CASE("Http digest authentication", "[Http][NotWorking]") {
    Slic3r::Http g = Slic3r::Http::get("https://jigsaw.w3.org/HTTP/Digest/");

    g.auth_digest("guest", "guest");

    unsigned status = 0;
    g.on_error([&status](std::string, std::string, unsigned http_status) {
        status = http_status;
    });

    g.on_complete([&status](std::string /* body */, unsigned http_status){
        status = http_status;
    });

    g.perform_sync();

    REQUIRE(status == 200);
}

TEST_CASE("Http basic authentication", "[Http][NotWorking]") {
    Slic3r::Http g = Slic3r::Http::get("https://jigsaw.w3.org/HTTP/Basic/");

    g.auth_basic("guest", "guest");

    unsigned status = 0;
    g.on_error([&status](std::string, std::string, unsigned http_status) {
        status = http_status;
    });

    g.on_complete([&status](std::string /* body */, unsigned http_status){
        status = http_status;
    });

    g.perform_sync();

    REQUIRE(status == 200);
}


#ifndef _WIN32
#include "slic3r/Utils/LanPrinter.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>

namespace {

// A printer that takes the connection and never answers: a socket that listens and never accepts. The kernel
// completes the connection, so curl sends its request and waits for the reply.
struct SilentPrinter
{
    int fd   = -1;
    int port = 0;

    SilentPrinter()
    {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a);
        ::listen(fd, 4);
        socklen_t n = sizeof a;
        ::getsockname(fd, reinterpret_cast<sockaddr *>(&a), &n);
        port = ntohs(a.sin_port);
    }
    ~SilentPrinter() { ::close(fd); }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/printer/info"; }
};

double seconds_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

// Before the stop check, a request waited for its whole limit (8.01 s with timeout_s 8): a stop of the MCP server
// then froze the window for up to 600 s, because the GUI thread waits for the tool calls.
TEST_CASE("A stop check ends a LAN request that the printer does not answer", "[LanHttp]")
{
    SilentPrinter      printer;
    Slic3r::LanRequest req;
    req.url       = printer.url();
    req.timeout_s = 30;

    const auto                 t0   = std::chrono::steady_clock::now();
    const Slic3r::LanTransport send = Slic3r::lan_http_transport([t0] { return seconds_since(t0) > 0.3; });
    const Slic3r::LanReply     r    = send(req);
    // curl asks about once a second while no data comes; the rest is room for a busy machine.
    CHECK(seconds_since(t0) < 5);
    CHECK(r.status == 0);
    CHECK(r.error == "stopped");

    // A request after the stop does not go out.
    const auto             t1    = std::chrono::steady_clock::now();
    const Slic3r::LanReply later = send(req);
    CHECK(seconds_since(t1) < 0.1);
    CHECK(later.error == "stopped");
}

TEST_CASE("With no stop check, a LAN request waits for its limit", "[LanHttp]")
{
    SilentPrinter      printer;
    Slic3r::LanRequest req;
    req.url       = printer.url();
    req.timeout_s = 2;

    const auto             t0 = std::chrono::steady_clock::now();
    const Slic3r::LanReply r  = Slic3r::lan_http_transport()(req);
    CHECK(seconds_since(t0) > 1.9);
    CHECK(r.status == 0);
    CHECK(r.error.find("Timeout") != std::string::npos);
}
#endif
