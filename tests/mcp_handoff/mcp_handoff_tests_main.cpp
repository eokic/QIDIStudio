// GUI hand-off tests. The GUI thread is a fake queue that a test runs when it wants, and the time moves only when
// the hand-off waits, so a 180 s limit takes no real time.
#include <catch_main.hpp>

#include "slic3r/GUI/Mcp/McpHandoff.hpp"

#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Slic3r::GUI::Mcp;
using json = nlohmann::json;
using ms   = std::chrono::milliseconds;

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

// The GUI thread: a queue of steps.
struct FakeGui
{
    std::vector<std::function<void()>> queue;

    UiHandoff::Post post()
    {
        return [this](std::function<void()> step) { queue.push_back(std::move(step)); };
    }
    std::vector<std::function<void()>> take() { return std::exchange(queue, {}); }
    void run_queue()
    {
        for (auto &step : take()) step();
    }
};

// The time. It moves only when the hand-off waits or sleeps; a wait ends at once when the result is there.
// on_wait is what happens on the GUI thread while the worker waits.
struct FakeTime
{
    std::chrono::steady_clock::time_point t {};
    std::function<void()>                 on_wait;

    UiHandoff::Clock clock()
    {
        return {[this] { return t; },
                [this](std::future<json> &f, ms d) {
                    if (on_wait) on_wait();
                    if (f.wait_for(ms(0)) == std::future_status::ready) return true;
                    t += d;
                    return false;
                },
                [this](ms d) {
                    if (on_wait) on_wait();
                    t += d;
                }};
    }
    ms since(std::chrono::steady_clock::time_point t0) const { return std::chrono::duration_cast<ms>(t - t0); }
};

const std::string k_no_answer = "QIDI Studio did not answer in time (a dialog may be open, or it is busy). Check the window and try again.";
const std::string k_stopping  = "The MCP server is stopping.";

} // namespace

TEST_CASE("A step runs on the GUI thread, and its result or its error comes back", "[mcp_handoff]")
{
    FakeGui  gui;
    FakeTime time;
    time.on_wait = [&] { gui.run_queue(); };
    UiHandoff handoff(gui.post(), time.clock());

    CHECK(handoff.run([] { return json(3); }, 15) == 3);
    CHECK(error_of([&] { handoff.run([]() -> json { throw ToolError("no such plate"); }, 15); }) == "no such plate");
    CHECK(time.since({}) == ms(0));
}

TEST_CASE("A step that is still queued at its limit never runs", "[mcp_handoff]")
{
    FakeGui   gui; // a dialog is open: the GUI thread does not get to the queue
    FakeTime  time;
    UiHandoff handoff(gui.post(), time.clock());

    bool ran = false;
    CHECK(error_of([&] { handoff.run([&] { ran = true; return json(); }, 180); }) == k_no_answer);
    CHECK(time.since({}) == ms(180000));
    gui.run_queue(); // the dialog closes
    CHECK_FALSE(ran);
}

TEST_CASE("A step that started and passes its limit says that it can still change the project", "[mcp_handoff]")
{
    FakeGui            gui;
    FakeTime           time;
    std::promise<void> started, release;
    std::thread        gui_thread;
    // At the first wait, the GUI thread starts the step, and the step runs longer than its limit.
    time.on_wait = [&] {
        if (gui_thread.joinable()) return;
        gui_thread = std::thread([steps = gui.take()] { for (auto &step : steps) step(); });
        started.get_future().wait();
    };
    UiHandoff handoff(gui.post(), time.clock());

    std::shared_future<void> released = release.get_future().share();
    const std::string        e        = error_of([&] {
        handoff.run([&] {
            started.set_value();
            released.wait();
            return json(1);
        }, 180);
    });
    release.set_value();
    gui_thread.join();
    CHECK(e == "QIDI Studio started this step, and it is still running after 180 s. It can still change the project: call get_status before you try again.");
    CHECK(time.since({}) == ms(180000));
}

TEST_CASE("wait_until asks the GUI thread after each pause, until it is done or the limit ends", "[mcp_handoff]")
{
    FakeGui  gui;
    FakeTime time;
    time.on_wait = [&] { gui.run_queue(); };
    UiHandoff handoff(gui.post(), time.clock());

    int asked = 0;
    handoff.wait_until([&] { return ++asked == 4; }, 120, 300, "still busy after 120 s");
    CHECK(asked == 4);
    CHECK(time.since({}) == ms(1200));

    const auto t0 = time.t;
    CHECK(error_of([&] { handoff.wait_until([] { return false; }, 2, 500, "Slicing is still running after 2 s."); }) ==
          "Slicing is still running after 2 s.");
    CHECK(time.since(t0) == ms(2000));
}

TEST_CASE("A stop ends each wait within 100 ms, and start undoes it", "[mcp_handoff]")
{
    FakeGui   gui;
    FakeTime  time;
    UiHandoff handoff(gui.post(), time.clock());

    SECTION("A worker that waits for a queued step")
    {
        bool ran     = false;
        time.on_wait = [&] {
            if (time.t == std::chrono::steady_clock::time_point(ms(1000))) handoff.stop();
        };
        CHECK(error_of([&] { handoff.run([&] { ran = true; return json(); }, 180); }) == k_stopping);
        CHECK(time.since({}) == ms(1100)); // the stop came at 1000 ms
        gui.run_queue();
        CHECK_FALSE(ran);
    }
    SECTION("A worker that waits between two questions")
    {
        int asked    = 0;
        time.on_wait = [&] {
            gui.run_queue();
            if (time.t == std::chrono::steady_clock::time_point(ms(1200))) handoff.stop();
        };
        CHECK(error_of([&] { handoff.wait_until([&] { ++asked; return false; }, 900, 500, "busy"); }) == k_stopping);
        CHECK(asked == 2);
        CHECK(time.since({}) == ms(1300)); // the stop came at 1200 ms, in the third pause of 500 ms
    }
    SECTION("A step after the stop does not go to the GUI thread, and after start it does")
    {
        handoff.stop();
        CHECK(handoff.stopping());
        CHECK(error_of([&] { handoff.run([] { return json(); }, 15); }) == k_stopping);
        CHECK(gui.queue.empty());

        handoff.start();
        time.on_wait = [&] { gui.run_queue(); };
        CHECK(handoff.run([] { return json(5); }, 15) == 5);
    }
}
