//	LSP integration gate — spawns a REAL clangd and asserts an end-to-end
//	completion + go-to-definition round trip. Headless (no GUI). Exit codes:
//	  0  = all checks passed
//	  1  = a check failed
//	  77 = clangd not found (ctest treats this as SKIP, keeping CI green)

#define _CRT_SECURE_NO_WARNINGS   // std::getenv (MSVC C4996 vs /WX)

#include "lsp_client.h"
#include "lsp_protocol.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

static int gFailures = 0, gChecks = 0;
#define CHECK(cond, msg) do { ++gChecks; if (!(cond)) { ++gFailures; \
	std::fprintf(stderr, "FAIL: %s\n", msg); } else { std::fprintf(stderr, "ok: %s\n", msg); } } while (0)

static std::string findClangd(int argc, char** argv)
{
	if (argc > 1 && std::filesystem::exists(argv[1])) return argv[1];
	if (const char* env = std::getenv("CLANGD_PATH"); env && std::filesystem::exists(env)) return env;
	const char* candidates[] = {
		"C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin/clangd.exe",
		"C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/bin/clangd.exe",
		"C:/Program Files/LLVM/bin/clangd.exe",
		"clangd",   // PATH (SDL_CreateProcess resolves via PATH)
	};
	for (const char* c : candidates)
		if (std::string(c) == "clangd" || std::filesystem::exists(c)) return c;
	return {};
}

// Poll the client until `pred()` is true or the deadline passes. Drains poll()
// into `sink`. Returns true if pred became true.
template <class Pred>
static bool pump(lsp::LspClient& c, std::vector<lsp::LspResult>& sink, int timeoutMs, Pred pred)
{
	int waited = 0;
	while (waited < timeoutMs)
	{
		for (auto& r : c.poll()) sink.push_back(std::move(r));
		if (pred()) return true;
		SDL_Delay(20);
		waited += 20;
	}
	for (auto& r : c.poll()) sink.push_back(std::move(r));
	return pred();
}

int main(int argc, char** argv)
{
	SDL_Init(0);   // base init (no subsystems) for the process API

	std::string clangd = findClangd(argc, argv);
	if (clangd.empty())
	{
		std::fprintf(stderr, "SKIP: clangd not found\n");
		SDL_Quit();
		return 77;
	}
	std::fprintf(stderr, "using clangd: %s\n", clangd.c_str());

	std::filesystem::path dir = std::filesystem::current_path();
	std::string rootUri = lsp::pathToUri(dir.string());
	std::string fileUri = lsp::pathToUri((dir / "lsptest_snippet.cpp").string());

	// Self-contained TU (no includes) so clangd resolves members without a
	// compile_commands.json. A non-ASCII comment exercises the byte-offset path.
	std::string code =
		"// \xC3\xA9\xC3\xA9 accented comment\n"   // 0
		"struct Foo { int alpha; int beta; void greet(); };\n"  // 1
		"int main() {\n"                            // 2
		"    Foo f;\n"                              // 3
		"    f.\n"                                  // 4  completion after 'f.' at char 6
		"    return 0;\n"                           // 5
		"}\n";                                      // 6

	lsp::LspClient client;
	if (!client.start(clangd, rootUri))
	{
		std::fprintf(stderr, "SKIP: clangd failed to spawn\n");
		SDL_Quit();
		return 77;
	}

	std::vector<lsp::LspResult> sink;
	bool ready = pump(client, sink, 15000, [&] { return client.ready(); });
	CHECK(ready, "clangd initialize handshake completed");
	if (!ready) { client.stop(); SDL_Quit(); return gFailures ? 1 : 0; }

	CHECK(client.offsetEncoding() == "utf-8", "clangd negotiated utf-8 offset encoding");

	client.didOpen(fileUri, "cpp", code);

	// Completion after "    f." (line 4, char 6). Retry: clangd returns an empty
	// list until its preamble/AST is built.
	bool gotMember = false;
	int waited = 0;
	while (waited < 15000 && !gotMember)
	{
		int id = client.requestCompletion(fileUri, 4, 6);
		(void) id;
		std::vector<lsp::LspResult> got;
		pump(client, got, 1500, [&] {
			for (auto& r : got) if (r.kind == lsp::ResultKind::Completion && !r.completionItems.empty()) return true;
			return false;
		});
		for (auto& r : got)
			if (r.kind == lsp::ResultKind::Completion)
				for (auto& it : r.completionItems)
					if (it.label == "alpha" || it.insertText == "alpha" ||
						it.label == "beta"  || it.insertText == "beta")
						gotMember = true;
		waited += 1500;
	}
	CHECK(gotMember, "completion after 'f.' lists struct members (alpha/beta)");

	// Go-to-definition on 'Foo' use at line 3 ("    Foo f;"), char 4 = 'F'.
	bool gotDef = false;
	int dwaited = 0;
	while (dwaited < 10000 && !gotDef)
	{
		client.requestDefinition(fileUri, 3, 4);
		std::vector<lsp::LspResult> got;
		pump(client, got, 1500, [&] {
			for (auto& r : got) if (r.kind == lsp::ResultKind::Definition && !r.locations.empty()) return true;
			return false;
		});
		for (auto& r : got)
			if (r.kind == lsp::ResultKind::Definition)
				for (auto& loc : r.locations)
					if (loc.line == 1)   // struct Foo is on line 1
						gotDef = true;
		dwaited += 1500;
	}
	CHECK(gotDef, "go-to-definition of Foo resolves to its declaration (line 1)");

	client.stop();
	SDL_Quit();

	if (gFailures == 0) { std::printf("lsptest: all %d checks passed\n", gChecks); return 0; }
	std::fprintf(stderr, "lsptest: %d/%d checks FAILED\n", gFailures, gChecks);
	return 1;
}
