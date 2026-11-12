#pragma once

#include <GameDefinitions/Base/Base.h>
#include <functional>
#include <deque>
#include <mutex>
#include <vector>

BEGIN_SE()

extern char const* BuildDate;

// Keeps the most recent console messages so that each Lua state can raise an event for them.
// Messages arrive from any thread; each reader keeps its own position (the index of the last
// message it saw) and never blocks a writer for longer than a copy.
class ConsoleMessageLog
{
public:
    struct Message
    {
        uint64_t Index;
        DebugMessageType Type;
        std::string Text;
    };

    static constexpr std::size_t MaxMessages = 1024;

    void Add(DebugMessageType type, char const* msg);
    // Index of the newest message; a reader that starts here sees only what comes later
    uint64_t LastIndex();
    // Appends the messages newer than `cursor` to `out` and advances `cursor`
    void Fetch(uint64_t& cursor, std::vector<Message>& out);

    // Set on a thread while it runs the Lua handlers of a console message, so that what those
    // handlers print is not handed back to them as new messages.
    static thread_local bool Dispatching;

private:
    std::mutex mutex_;
    std::deque<Message> messages_;
    uint64_t lastIndex_{ 0 };
};

extern ConsoleMessageLog gConsoleMessageLog;

class DebugConsole : public Console
{
public:
    void Create() override;
    void HandleCommand(std::string const& cmd);
    void Print(DebugMessageType type, char const* msg) override;

private:
    bool consoleRunning_{ false };
    bool serverContext_{ true };
    bool multiLineMode_{ false };
    std::thread* consoleThread_{ nullptr };
    std::string multiLineCommand_;
    uint32_t width_{ 0 };
    uint32_t height_{ 0 };
    uint32_t resized_{ false };

    void ConsoleThread();
    void InputLoop();
    void UpdateConsoleSize();
    void SubmitTaskAndWait(bool server, std::function<void()> fun);
    void PrintHelp();
    void ResetLua();
    void ResetLuaClient();
    void ResetLuaServer();
    void ExecLuaCommand(std::string const& cmd);
    void ClearFromReset();
};

END_SE()
