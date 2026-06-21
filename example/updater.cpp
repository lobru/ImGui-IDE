#include "updater.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <nlohmann/json.hpp>

namespace
{
std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// GET `url`, filling `status` (HTTP code) and either `body` or, if `sink` is set,
// streaming the bytes to that file. Returns false only on transport failure.
bool httpGet(const std::wstring& url, int& status, std::string& body, std::string& err,
             std::ofstream* sink = nullptr)
{
    status = 0;
    body.clear();

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[4096] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4095;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
    {
        err = "malformed URL";
        return false;
    }

    HINTERNET hSession = WinHttpOpen(L"ImGui-IDE-Updater/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
    {
        err = "WinHttpOpen failed";
        return false;
    }
    WinHttpSetTimeouts(hSession, 8000, 8000, 15000, 30000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        err = "connect failed";
        return false;
    }

    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        err = "open request failed";
        return false;
    }

    const wchar_t* headers = L"User-Agent: ImGui-IDE-Updater\r\n"
                             L"Accept: application/vnd.github+json\r\n";
    BOOL ok = WinHttpSendRequest(hReq, headers, (DWORD) -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok)
        ok = WinHttpReceiveResponse(hReq, nullptr);

    if (ok)
    {
        DWORD sc = 0, scl = sizeof(sc);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &sc, &scl, WINHTTP_NO_HEADER_INDEX);
        status = (int) sc;

        for (;;)
        {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0)
                break;
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(hReq, buf.data(), avail, &read) || read == 0)
                break;
            if (sink)
                sink->write(buf.data(), (std::streamsize) read);
            else
                body.append(buf.data(), read);
        }
    }
    else
    {
        err = "request failed";
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok != FALSE;
}
} // namespace

namespace updater
{
Release fetchLatest(const std::string& owner, const std::string& repo)
{
    Release r;
    std::string url = "https://api.github.com/repos/" + owner + "/" + repo + "/releases/latest";
    int status = 0;
    std::string body, err;
    if (!httpGet(widen(url), status, body, err))
    {
        r.error = err.empty() ? "network error" : err;
        return r;
    }
    if (status == 404)
    {
        r.notFound = true;
        r.ok = true; // reachable, just nothing published yet
        return r;
    }
    if (status != 200)
    {
        r.error = "HTTP " + std::to_string(status);
        return r;
    }
    try
    {
        auto j = nlohmann::json::parse(body);
        r.tag = j.value("tag_name", std::string());
        r.htmlUrl = j.value("html_url", std::string());
        r.notes = j.value("body", std::string());
        if (j.contains("assets") && j["assets"].is_array())
        {
            std::string firstUrl, firstName;
            for (auto& a : j["assets"])
            {
                std::string name = a.value("name", std::string());
                std::string durl = a.value("browser_download_url", std::string());
                if (durl.empty())
                    continue;
                if (firstUrl.empty())
                {
                    firstUrl = durl;
                    firstName = name;
                }
                std::string low = name;
                std::transform(low.begin(), low.end(), low.begin(),
                               [](unsigned char c) { return (char) std::tolower(c); });
                if (low.size() >= 4 && (low.compare(low.size() - 4, 4, ".exe") == 0 ||
                                        low.compare(low.size() - 4, 4, ".msi") == 0))
                {
                    r.assetUrl = durl;
                    r.assetName = name;
                    break;
                }
            }
            if (r.assetUrl.empty())
            {
                r.assetUrl = firstUrl;
                r.assetName = firstName;
            }
        }
        r.ok = !r.tag.empty();
        if (!r.ok)
            r.error = "release JSON missing tag_name";
    }
    catch (const std::exception& e)
    {
        r.error = std::string("parse error: ") + e.what();
    }
    return r;
}

bool download(const std::string& url, const std::string& destPath)
{
    std::ofstream out(destPath, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    int status = 0;
    std::string body, err;
    bool ok = httpGet(widen(url), status, body, err, &out);
    out.close();
    return ok && status == 200;
}

bool runInstaller(const std::string& path)
{
    HINSTANCE h = ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR) h > 32;
}

void openUrl(const std::string& url)
{
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
} // namespace updater

#else // !_WIN32 — non-Windows builds get harmless stubs.
namespace updater
{
Release fetchLatest(const std::string&, const std::string&)
{
    Release r;
    r.error = "updater only implemented on Windows";
    return r;
}
bool download(const std::string&, const std::string&) { return false; }
bool runInstaller(const std::string&) { return false; }
void openUrl(const std::string&) {}
} // namespace updater
#endif

namespace updater
{
bool isNewer(const std::string& latest, const std::string& current)
{
    auto strip = [](const std::string& s) {
        size_t i = 0;
        while (i < s.size() && (s[i] == 'v' || s[i] == 'V'))
            i++;
        return s.substr(i);
    };
    auto parse = [](const std::string& s) {
        std::vector<long> p;
        long n = 0;
        bool any = false;
        for (char c : s)
        {
            if (c >= '0' && c <= '9')
            {
                n = n * 10 + (c - '0');
                any = true;
            }
            else if (c == '.')
            {
                p.push_back(any ? n : 0);
                n = 0;
                any = false;
            }
            else
                break;
        }
        p.push_back(any ? n : 0);
        return p;
    };
    std::vector<long> a = parse(strip(latest)), b = parse(strip(current));
    size_t n = std::max(a.size(), b.size());
    a.resize(n, 0);
    b.resize(n, 0);
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] > b[i];
    return false; // equal → not newer
}
} // namespace updater
