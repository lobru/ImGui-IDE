//	gh_client.h - async GitHub REST transport for the wasm IDE shell
//	Part of ImGui-IDE (github.com/lobotomy-x/ImGuiColorTextEdit)
//
//	Copyright (c) 2024-2026 Johan A. Goossens, Logan Brunet. All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.
//
//	Thin emscripten_fetch wrapper. All callbacks fire on the main thread (no
//	pthreads), so app state can be touched directly from them. Parsing of the
//	responses lives in gh_parse.h (transport-free, natively tested).
//
//	GitHub's REST API sends CORS headers (Access-Control-Allow-Origin: *), so
//	this works from any origin - including a file:// page.

#ifndef GH_CLIENT_H
#define GH_CLIENT_H

#include <functional>
#include <string>
#include <vector>

#include <emscripten/fetch.h>

#include "gh_parse.h"

namespace ghc {

struct Config {
	std::string owner;
	std::string repo;
	std::string branch;
	std::string token; // fine-grained PAT; empty = unauthenticated (public, low rate limit)
};

//	status: HTTP status, or 0 when the request never reached the server
using Callback = std::function<void(int status, std::string body)>;

namespace detail {

struct Closure {
	Callback cb;
	std::string body;                 // PUT payload, kept alive for the fetch
	std::vector<std::string> headers; // alternating key/value, kept alive
};

inline void onDone(emscripten_fetch_t* fetch) {
	Closure* c = static_cast<Closure*>(fetch->userData);
	std::string data(fetch->data ? fetch->data : "", (size_t) fetch->numBytes);
	int status = (int) fetch->status;
	emscripten_fetch_close(fetch);

	if (c) {
		c->cb(status, std::move(data));
		delete c;
	}
}

inline void request(const char* method, const std::string& url, const Config& cfg,
                    const char* accept, std::string body, Callback cb) {
	Closure* c = new Closure{std::move(cb), std::move(body), {}};

	c->headers.push_back("Accept");
	c->headers.push_back(accept);
	c->headers.push_back("X-GitHub-Api-Version");
	c->headers.push_back("2022-11-28");

	if (!cfg.token.empty()) {
		c->headers.push_back("Authorization");
		c->headers.push_back("Bearer " + cfg.token);
	}

	if (!c->body.empty()) {
		c->headers.push_back("Content-Type");
		c->headers.push_back("application/json");
	}

	std::vector<const char*> hdrs;

	for (auto& h : c->headers) {
		hdrs.push_back(h.c_str());
	}

	hdrs.push_back(nullptr);

	emscripten_fetch_attr_t attr;
	emscripten_fetch_attr_init(&attr);
	std::snprintf(attr.requestMethod, sizeof(attr.requestMethod), "%s", method);
	attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
	attr.requestHeaders = hdrs.data();
	attr.userData = c;
	attr.onsuccess = onDone;
	attr.onerror = onDone; // non-2xx also lands here; status tells the story

	if (!c->body.empty()) {
		attr.requestData = c->body.data();
		attr.requestDataSize = c->body.size();
	}

	emscripten_fetch(&attr, url.c_str());
}

inline std::string apiBase(const Config& cfg) {
	return "https://api.github.com/repos/" + ghp::urlEncode(cfg.owner) + "/" +
		ghp::urlEncode(cfg.repo);
}

} // namespace detail

//
//	endpoints
//

//	GET /repos/{o}/{r}/branches (first 100)
inline void listBranches(const Config& cfg, Callback cb) {
	detail::request("GET", detail::apiBase(cfg) + "/branches?per_page=100", cfg,
		"application/vnd.github+json", "", std::move(cb));
}

//	GET /repos/{o}/{r}/git/trees/{branch}?recursive=1 - whole repo listing
inline void getTree(const Config& cfg, Callback cb) {
	detail::request("GET",
		detail::apiBase(cfg) + "/git/trees/" + ghp::urlEncode(cfg.branch) + "?recursive=1",
		cfg, "application/vnd.github+json", "", std::move(cb));
}

//	GET /repos/{o}/{r}/contents/{path} with the raw media type: the body IS
//	the file content (no base64 step, no 1MB contents-API JSON limit games)
inline void getFileRaw(const Config& cfg, const std::string& path, Callback cb) {
	// path segments need encoding; slashes stay
	std::string enc;

	for (char ch : path) {
		if (ch == '/') { enc += ch; } else { enc += ghp::urlEncode(std::string(1, ch)); }
	}

	detail::request("GET",
		detail::apiBase(cfg) + "/contents/" + enc + "?ref=" + ghp::urlEncode(cfg.branch),
		cfg, "application/vnd.github.raw+json", "", std::move(cb));
}

//	PUT /repos/{o}/{r}/contents/{path} - commit one file to the branch.
//	`sha` is the current blob sha (from the tree) for updates; empty creates.
inline void putFile(const Config& cfg, const std::string& path, const std::string& content,
                    const std::string& sha, const std::string& message, Callback cb) {
	nlohmann::json j;
	j["message"] = message.empty() ? ("Update " + path) : message;
	j["content"] = ghp::base64Encode(content);
	j["branch"] = cfg.branch;

	if (!sha.empty()) {
		j["sha"] = sha;
	}

	std::string enc;

	for (char ch : path) {
		if (ch == '/') { enc += ch; } else { enc += ghp::urlEncode(std::string(1, ch)); }
	}

	detail::request("PUT", detail::apiBase(cfg) + "/contents/" + enc, cfg,
		"application/vnd.github+json", j.dump(), std::move(cb));
}

//	GET /search/code - NOTE: indexes the DEFAULT branch only, requires auth
inline void searchCode(const Config& cfg, const std::string& query, Callback cb) {
	std::string q = ghp::urlEncode(query + " repo:" + cfg.owner + "/" + cfg.repo);
	detail::request("GET", "https://api.github.com/search/code?per_page=30&q=" + q, cfg,
		"application/vnd.github.text-match+json", "", std::move(cb));
}

} // namespace ghc

#endif // GH_CLIENT_H
