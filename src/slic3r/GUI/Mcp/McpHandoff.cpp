#include "McpHandoff.hpp"

#include <algorithm>
#include <memory>
#include <thread>

namespace Slic3r { namespace GUI { namespace Mcp {

using json = nlohmann::json;

namespace {

// How often a waiting worker looks at its limit and at the stop.
constexpr int k_poll_ms = 100;

// The limit of each question that wait_until asks the GUI thread.
constexpr int k_ask_timeout_s = 15;

enum StepState : int { Queued, Started, Cancelled };

// What the worker and the GUI thread share. The GUI thread can get to a step after the worker stopped waiting for it.
struct Step
{
    std::function<json()> fn;
    std::promise<json>    result;
    std::atomic<int>      state { Queued };
};

} // namespace

UiHandoff::Clock UiHandoff::Clock::real()
{
    return {[] { return std::chrono::steady_clock::now(); },
            [](std::future<json> &f, std::chrono::milliseconds ms) { return f.wait_for(ms) == std::future_status::ready; },
            [](std::chrono::milliseconds ms) { std::this_thread::sleep_for(ms); }};
}

UiHandoff::UiHandoff(Post post, Clock clock) : m_post(std::move(post)), m_clock(std::move(clock)) {}

void UiHandoff::throw_if_stopping() const
{
    if (m_stopping) throw ToolError("The MCP server is stopping.");
}

json UiHandoff::run(std::function<json()> fn, int timeout_s)
{
    throw_if_stopping();
    auto step = std::make_shared<Step>();
    step->fn  = std::move(fn);
    std::future<json> result = step->result.get_future();
    m_post([step] {
        // The worker cancels a step that is still queued at its limit: one compare-and-swap decides, so the step
        // either starts here or never runs.
        int queued = Queued;
        if (!step->state.compare_exchange_strong(queued, Started)) return;
        try {
            step->result.set_value(step->fn());
        } catch (...) {
            step->result.set_exception(std::current_exception());
        }
    });
    const auto until = m_clock.now() + std::chrono::seconds(timeout_s);
    while (!m_clock.wait(result, std::chrono::milliseconds(k_poll_ms))) {
        if (!m_stopping && m_clock.now() < until) continue;
        int        queued    = Queued;
        const bool cancelled = step->state.compare_exchange_strong(queued, Cancelled);
        if (!cancelled && result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) break; // it just ended
        throw_if_stopping();
        if (cancelled)
            throw ToolError("QIDI Studio did not answer in time (a dialog may be open, or it is busy). Check the window and try again.");
        throw ToolError("QIDI Studio started this step, and it is still running after " + std::to_string(timeout_s) +
                        " s. It can still change the project: call get_status before you try again.");
    }
    return result.get(); // rethrows what fn threw
}

void UiHandoff::wait_until(std::function<bool()> done_on_ui, int timeout_s, int poll_ms, const std::string &still_busy)
{
    const auto until = m_clock.now() + std::chrono::seconds(timeout_s);
    for (;;) {
        // Wait first: a job that a step just started can still show as idle.
        for (int slept = 0; slept < poll_ms; slept += k_poll_ms) {
            throw_if_stopping();
            m_clock.sleep(std::chrono::milliseconds(std::min(k_poll_ms, poll_ms - slept)));
        }
        // done_on_ui is copied into the step: the step can outlive this call.
        if (run([done_on_ui] { return json(done_on_ui()); }, k_ask_timeout_s).get<bool>()) return;
        if (m_clock.now() >= until) throw ToolError(still_busy);
    }
}

}}} // namespace Slic3r::GUI::Mcp
