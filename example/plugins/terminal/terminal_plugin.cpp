//
//  terminal_plugin.cpp — see terminal_plugin.h.
//

#include "terminal_plugin.h"

#include <algorithm>
#include <cstring>

#include <imgui.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace
{
// Cap the retained output so a chatty build doesn't grow the buffer without bound.
constexpr size_t kMaxOutput = 2 * 1024 * 1024;

void appendCapped(std::string &buf, const char *data, size_t n)
{
    buf.append(data, n);
    if (buf.size() > kMaxOutput)
        buf.erase(0, buf.size() - kMaxOutput);
}
} // namespace

TerminalPlugin::~TerminalPlugin()
{
    stopSession();
}

void TerminalPlugin::readerLoop(std::shared_ptr<Session> s)
{
    char buf[4096];
    for (;;)
    {
#ifdef _WIN32
        DWORD read = 0;
        BOOL ok = ReadFile(s->hStdoutRead, buf, sizeof(buf), &read, nullptr);
        if (!ok || read == 0)
            break;
#else
        ssize_t read = ::read(s->stdoutRead, buf, sizeof(buf));
        if (read <= 0)
            break;
#endif
        std::lock_guard<std::mutex> lk(s->mutex);
        appendCapped(s->output, buf, (size_t)read);
    }
    s->readerDone.store(true);
    s->alive.store(false);
}

void TerminalPlugin::startSession(PluginHost &host)
{
    stopSession();

    auto s = std::make_shared<Session>();
    std::filesystem::path root = host.hostProjectRoot();
    s->cwd = root.empty() ? std::filesystem::current_path().string() : root.string();

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRead = nullptr, outWrite = nullptr, inRead = nullptr, inWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0) || !CreatePipe(&inRead, &inWrite, &sa, 0))
    {
        host.hostError("Terminal: could not create pipes");
        return;
    }
    // The parent's ends must NOT be inherited by the child.
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    si.hStdInput = inRead;

    PROCESS_INFORMATION pi = {};
    // /Q echo off, /K stay open for interactive input. A fresh cmd per session.
    char cmdline[] = "cmd.exe /Q /K";
    BOOL ok = CreateProcessA(nullptr, cmdline, nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, s->cwd.c_str(), &si, &pi);

    // The child owns its ends now; the parent closes its copies of them.
    CloseHandle(outWrite);
    CloseHandle(inRead);

    if (!ok)
    {
        CloseHandle(outRead);
        CloseHandle(inWrite);
        host.hostError("Terminal: could not start cmd.exe");
        return;
    }

    CloseHandle(pi.hThread);
    s->hProcess = pi.hProcess;
    s->hStdinWrite = inWrite;
    s->hStdoutRead = outRead;
    s->shellName = "cmd.exe";
#else
    int outPipe[2], inPipe[2];
    if (pipe(outPipe) != 0 || pipe(inPipe) != 0)
    {
        host.hostError("Terminal: could not create pipes");
        return;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, outPipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, inPipe[1]);
    posix_spawn_file_actions_addclose(&fa, outPipe[0]);

    const char *shell = getenv("SHELL");
    if (!shell || !*shell)
        shell = "/bin/sh";
    // -i interactive so the prompt/job control behaves; cwd via chdir in the child
    // is not available to posix_spawn, so wrap in a cd.
    std::string cmd = "cd '" + s->cwd + "' 2>/dev/null; exec \"" + shell + "\" -i";
    char *argv[] = {(char *)shell, (char *)"-c", (char *)cmd.c_str(), nullptr};

    pid_t pid = -1;
    int rc = posix_spawn(&pid, shell, &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(inPipe[0]);
    close(outPipe[1]);

    if (rc != 0)
    {
        close(inPipe[1]);
        close(outPipe[0]);
        host.hostError("Terminal: could not start the shell");
        return;
    }

    s->pid = pid;
    s->stdinWrite = inPipe[1];
    s->stdoutRead = outPipe[0];
    s->shellName = shell;
#endif

    s->alive.store(true);
    s->reader = std::thread(&TerminalPlugin::readerLoop, s);
    session = s;
    startedForRoot = s->cwd;
}

void TerminalPlugin::stopSession()
{
    if (!session)
        return;

    auto s = session;
    session.reset();

#ifdef _WIN32
    if (s->hProcess)
    {
        TerminateProcess((HANDLE)s->hProcess, 0);
        // closing the read end unblocks the reader thread's ReadFile
    }
    if (s->hStdinWrite)
        CloseHandle((HANDLE)s->hStdinWrite);
    if (s->hStdoutRead)
        CloseHandle((HANDLE)s->hStdoutRead);
    if (s->reader.joinable())
        s->reader.join();
    if (s->hProcess)
        CloseHandle((HANDLE)s->hProcess);
    s->hProcess = s->hStdinWrite = s->hStdoutRead = nullptr;
#else
    if (s->pid > 0)
        kill(s->pid, SIGKILL);
    if (s->stdinWrite >= 0)
        close(s->stdinWrite);
    if (s->stdoutRead >= 0)
        close(s->stdoutRead);
    if (s->reader.joinable())
        s->reader.join();
    if (s->pid > 0)
        waitpid(s->pid, nullptr, 0);
    s->pid = -1;
    s->stdinWrite = s->stdoutRead = -1;
#endif
    s->alive.store(false);
}

void TerminalPlugin::sendLine(const std::string &line)
{
    if (!session || !session->alive.load())
        return;

    std::string data = line;
    data += "\n"; // cmd.exe accepts \n on a pipe

#ifdef _WIN32
    DWORD written = 0;
    WriteFile((HANDLE)session->hStdinWrite, data.data(), (DWORD)data.size(), &written, nullptr);
    FlushFileBuffers((HANDLE)session->hStdinWrite);
#else
    ssize_t n = ::write(session->stdinWrite, data.data(), data.size());
    (void)n;
#endif

    // Echo the command locally so the transcript reads like a real terminal
    // (cmd.exe with /Q doesn't echo piped input).
    std::lock_guard<std::mutex> lk(session->mutex);
    session->output += line;
    session->output += "\n";
}

void TerminalPlugin::sendSignalCtrlC()
{
    if (!session || !session->alive.load())
        return;
#ifdef _WIN32
    // A Ctrl-C over a pipe: cmd.exe treats a lone 0x03 as an interrupt for the
    // running child on most builds; also send an empty line to break input.
    const char etx = 0x03;
    DWORD written = 0;
    WriteFile((HANDLE)session->hStdinWrite, &etx, 1, &written, nullptr);
#else
    if (session->pid > 0)
        kill(session->pid, SIGINT);
#endif
}

void TerminalPlugin::onMenu(PluginHost &, PluginMenu which)
{
    if (which == PluginMenu::Tools)
        ImGui::MenuItem("Terminal", nullptr, &visible);
}

void TerminalPlugin::onFrame(PluginHost &host)
{
    if (!visible)
        return;

    ImGui::SetNextWindowSize(ImVec2(720, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Terminal", &visible))
    {
        ImGui::End();
        return;
    }

    // Restart the shell if the project root changed under us (or it died).
    std::filesystem::path root = host.hostProjectRoot();
    std::string wantCwd = root.empty() ? std::filesystem::current_path().string() : root.string();
    const bool running = session && session->alive.load();

    // ── toolbar ──
    if (!running)
    {
        if (ImGui::Button(session ? "Restart" : "Start"))
            startSession(host);
    }
    else
    {
        if (ImGui::Button("Restart"))
            startSession(host);
        ImGui::SameLine();
        if (ImGui::Button("Stop"))
            stopSession();
        ImGui::SameLine();
        if (ImGui::Button("Ctrl-C"))
            sendSignalCtrlC();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear") && session)
    {
        std::lock_guard<std::mutex> lk(session->mutex);
        session->output.clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll);
    ImGui::SameLine();
    // Re-evaluate — the Stop button above just reset `session`, so the pre-toolbar
    // `running` is stale THIS frame and dereferencing through it crashed (null
    // shellName read the frame Stop was clicked).
    const bool runningNow = session && session->alive.load();
    ImGui::TextDisabled("%s  \xc2\xb7  %s", runningNow ? session->shellName.c_str() : "(stopped)",
                        wantCwd.c_str());

    if (runningNow && startedForRoot != wantCwd)
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("cd to project"))
            startSession(host);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The project changed since this shell started \xe2\x80\x94 restart it in %s",
                              wantCwd.c_str());
    }

    ImGui::Separator();

    // ── output ──
    const float inputH = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##termOut", ImVec2(0, -inputH), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    host.hostMiddleMousePanScroll(120); // drag-scroll, honoring invert-pan
    {
        std::string snapshot;
        if (session)
        {
            std::lock_guard<std::mutex> lk(session->mutex);
            snapshot = session->output;
        }
        else
        {
            snapshot = "Integrated terminal. Press Start to launch a shell in the project directory.\n";
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));
        ImGui::TextUnformatted(snapshot.c_str(), snapshot.c_str() + snapshot.size());
        ImGui::PopStyleVar();

        if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    // ── input ──
    ImGui::SetNextItemWidth(-1.0f);
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
    bool submitted = ImGui::InputTextWithHint("##termIn",
                                              running ? "type a command, Enter to run" : "start the shell first",
                                              inputBuf, sizeof(inputBuf), flags);
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere(-1);

    if (submitted)
    {
        if (!running)
            startSession(host);
        sendLine(inputBuf);
        inputBuf[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1); // keep focus for the next command
    }

    ImGui::End();
}

std::unique_ptr<EditorPlugin> createTerminalPlugin()
{
    return std::make_unique<TerminalPlugin>();
}
