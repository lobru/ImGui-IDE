//	git_url — normalize a git remote URL to an https:// browse URL.
//
//	Header-only + dependency-free so both the editor and the headless selftest can
//	use it. The parsing (ssh/scp-style vs https, stripping a trailing .git) is the
//	fiddly part worth testing, so it lives away from the UI.

#pragma once

#include <string>

inline std::string gitRemoteToWebUrl(std::string url)
{
	if (url.empty())
		return {};

	// git@github.com:owner/repo(.git)  ->  https://github.com/owner/repo
	if (url.rfind("git@", 0) == 0)
	{
		size_t colon = url.find(':');
		if (colon != std::string::npos)
			url = "https://" + url.substr(4, colon - 4) + "/" + url.substr(colon + 1);
	}
	// ssh://git@github.com/owner/repo(.git)  ->  https://github.com/owner/repo
	else if (url.rfind("ssh://", 0) == 0)
	{
		url = url.substr(6);
		size_t at = url.find('@');
		size_t slash = url.find('/');
		if (at != std::string::npos && (slash == std::string::npos || at < slash))
			url = url.substr(at + 1);
		url = "https://" + url;
	}
	else if (url.rfind("http://", 0) == 0)
	{
		url = "https://" + url.substr(7);
	}

	if (url.size() > 4 && url.compare(url.size() - 4, 4, ".git") == 0)
		url.erase(url.size() - 4);

	// Only ever hand a genuine web URL to the OS opener.
	if (url.rfind("https://", 0) != 0)
		return {};

	return url;
}
