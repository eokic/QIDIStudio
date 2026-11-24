// The hand-off from a worker thread to the GUI thread. A tool call runs on a worker thread; each step that touches the
// GUI, the model or the presets goes to the GUI thread, and the worker waits for its result with a limit.
//
// A step is queued, then started or cancelled. When the limit ends, a step that is still queued never runs. A step
// that started runs to its end, and the call says that it can still change the project. A stop ends each wait.
//
// No wx here: the app posts the steps with CallAfter, and tests/mcp_handoff gives a fake queue and a fake clock.
#ifndef slic3r_GUI_McpHandoff_hpp_
#define slic3r_GUI_McpHandoff_hpp_

#include "McpServer.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

class UiHandoff
{
public:
    // Gives a step to the GUI thread, which runs it later.
    using Post = std::function<void(std::function<void()> step)>;

    // How the hand-off waits. The app uses the real time; a test gives a time that moves only when the test says.
    struct Clock
    {
        std::function<std::chrono::steady_clock::time_point()> now;
        // Waits up to the given time for a step's result. True when the result is there.
        std::function<bool(std::future<nlohmann::json> &, std::chrono::milliseconds)> wait;
        std::function<void(std::chrono::milliseconds)>                               sleep;

        static Clock real();
    };

    explicit UiHandoff(Post post, Clock clock = Clock::real());

    // Runs fn on the GUI thread, and gives its result or throws what it threw. Throws ToolError when the server
    // stops, or when fn has not ended after timeout_s.
    nlohmann::json run(std::function<nlohmann::json()> fn, int timeout_s);

    // Waits poll_ms, then asks done_on_ui on the GUI thread, until it gives true. Throws ToolError(still_busy) when it
    // still gives false after timeout_s.
    void wait_until(std::function<bool()> done_on_ui, int timeout_s, int poll_ms, const std::string &still_busy);

    // After stop(), each wait ends within 100 ms, and each later run() throws at once. start() undoes it.
    void stop() { m_stopping = true; }
    void start() { m_stopping = false; }
    bool stopping() const { return m_stopping; }

private:
    void throw_if_stopping() const;

    Post              m_post;
    Clock             m_clock;
    std::atomic<bool> m_stopping { false };
};

}}} // namespace Slic3r::GUI::Mcp

#endif
