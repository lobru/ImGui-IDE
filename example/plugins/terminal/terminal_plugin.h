//
//  terminal_plugin.h — an integrated, project-local terminal (like VS Code's).
//
//  A dockable panel that runs a PERSISTENT shell (cmd.exe / $SHELL) rooted at the
//  project directory, with stdin/stdout/stderr piped so commands and their output
//  stream in-app. Output is read on a worker thread; the UI drains a mutex-guarded
//  buffer each frame. A real DLL plugin — the core links none of this.
//

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "plugin_api.h"

class TerminalPlugin : public EditorPlugin
{
public:
    TerminalPlugin() = default;
    ~TerminalPlugin() override;

    const char *id() const override { return "terminal"; }
    const char *displayName() const override { return "Integrated terminal"; }

    void onMenu(PluginHost &host, PluginMenu which) override;
    void contributePaletteCommands(PluginHost &host, const PluginDocInfo &doc,
                                   const std::function<void(const std::string &,
                                                            std::function<void()>)> &add) override
    {
        (void) host; (void) doc;
        add("Terminal: Toggle", [this] { visible = !visible; });
    }
    void contributeKeybinds(PluginHost &, std::vector<PluginKeybind> &out) override
    {
        // VS Code's terminal chord; rebindable in Settings > Keybinds.
        out.push_back({"terminal.toggle", "Toggle terminal", "Ctrl+`",
                       [this] { visible = !visible; }, {}});
    }
    void onFrame(PluginHost &host) override;

private:
    // ── One shell session ───────────────────────────────────────────────────
    // Held by shared_ptr so the reader thread (which captures it) can outlive a
    // restart/close: the thread keeps the pipe/handle alive until it drains.
    struct Session
    {
        std::mutex mutex;             // guards `output`
        std::string output;          // accumulated shell output (capped)
        std::atomic<bool> alive{false};
        std::atomic<bool> readerDone{false};
        std::string shellName;       // e.g. "cmd.exe"
        std::string cwd;

#ifdef _WIN32
        void *hProcess = nullptr;    // HANDLE
        void *hStdinWrite = nullptr; // HANDLE (parent -> child stdin)
        void *hStdoutRead = nullptr; // HANDLE (child stdout/stderr -> parent)
#else
        int pid = -1;
        int stdinWrite = -1;
        int stdoutRead = -1;
#endif
        // NO std::thread member: the reader is DETACHED and holds a shared_ptr to
        // this Session, so Session must be destroyable from the reader thread
        // itself (last ref). A joinable std::thread destroyed from within its own
        // thread calls std::terminate() — the crash this design avoids.
    };

    std::shared_ptr<Session> session;
    bool  visible = false;
    bool  autoScroll = true;
    char  inputBuf[4096] = {0};
    std::string startedForRoot;      // project root the current session was started in

    void startSession(PluginHost &host);
    void stopSession();
    void sendLine(const std::string &line);
    void sendSignalCtrlC();
    static void readerLoop(std::shared_ptr<Session> s); // drains child stdout → s->output
};

std::unique_ptr<EditorPlugin> createTerminalPlugin();
