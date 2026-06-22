// In-app updater: query GitHub Releases for a newer build and download the
// installer asset. Windows-only (WinHTTP); the .cpp is a no-op stub elsewhere so
// the rest of the app stays platform-agnostic.
#pragma once
#include <string>

namespace updater
{
struct Release
{
    bool        ok = false;       // request succeeded AND a release/tag was found
    bool        notFound = false; // 404 — repo has no published releases (treat as up-to-date)
    std::string error;            // human-readable failure reason when !ok
    std::string tag;              // e.g. "1.0" or "v0.2.0"
    std::string htmlUrl;          // the release page (browser fallback)
    std::string assetUrl;         // best installer asset (browser_download_url), may be empty
    std::string assetName;        // that asset's filename
    std::string notes;            // release body / changelog
};

// Blocking HTTPS GET of <owner>/<repo>'s latest release. Safe on a worker thread.
Release fetchLatest(const std::string& owner, const std::string& repo);

// Blocking download of url -> destPath (follows redirects). True on success.
bool download(const std::string& url, const std::string& destPath);

// Full path of the currently-running executable.
std::string runningExePath();

// Apply an update in place — no installer. `assetPath` is the downloaded asset:
//   .exe  → it IS the new build; replaces `targetExe`.
//   .zip  → extracted; the first .exe inside replaces `targetExe`.
// A running .exe can't be overwritten, so the live one is renamed to
// "<targetExe>.old" and the new build is dropped into its place; the swap takes
// effect on the next launch. Returns true if staged; sets `err` on failure.
bool applyUpdate(const std::string& assetPath, const std::string& targetExe, std::string& err);

// Delete a leftover "<targetExe>.old" from a previous in-place update. Best-effort.
void cleanupStaleUpdate(const std::string& targetExe);

// Open a URL in the default browser.
void openUrl(const std::string& url);

// Tolerant dotted-numeric compare, ignores a leading 'v'. True if latest > current.
bool isNewer(const std::string& latest, const std::string& current);
} // namespace updater
