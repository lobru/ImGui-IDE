// imgui_ide_uevr_bridge: a HEADLESS UEVR plugin that lets the standalone
// ImGui-IDE ("UEVR Live" panel) drive this running game's Lua state over a
// file inbox. No ImGui, no window — it only polls files on the engine tick and
// runs them through UEVR's Lua via exec_lua_chunk.
//
// Protocol (matches example/uevr_bridge.cpp in ImGui-IDE):
//   IDE writes  <ide_bridge>/cmd/<reqId>.txt : line1 = kind
//               (run|globals|modules|inspect), remaining lines = payload
//   we run it and write <ide_bridge>/out/<reqId>.txt : line1 = kind, rest = result
//   then delete the cmd file.
//
//   <ide_bridge> = %APPDATA%\UnrealVRMod\UEVR\ide_bridge
//
// Build: drop next to the other UEVR example plugins (see CMakeLists.txt) and
// build it there, or install via ImGui-IDE. It links ONLY the UEVR SDK headers
// — nothing from ImGui-IDE — so it is tiny and host-ABI-neutral.

#define _CRT_SECURE_NO_WARNINGS

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "uevr/Plugin.hpp"

using namespace uevr;
namespace fs = std::filesystem;

namespace {

constexpr const char* TAG = "[ide_bridge]";

fs::path g_cmd_dir;
fs::path g_out_dir;
int      g_frame = 0;

// Reused verbatim from the UEVR lua_script_editor_plugin — a safe, pcall-guarded
// dump of the user globals / loaded modules as plain text.
constexpr const char* DUMP_GLOBALS_LUA = R"LUA(
local _ok, _result = pcall(function()
local out = {}
local builtin = {
    _G=true, _VERSION=true, assert=true, collectgarbage=true, dofile=true,
    error=true, getmetatable=true, ipairs=true, load=true, loadfile=true,
    next=true, pairs=true, pcall=true, print=true, rawequal=true, rawget=true,
    rawlen=true, rawset=true, require=true, select=true, setmetatable=true,
    tonumber=true, tostring=true, type=true, unpack=true, xpcall=true,
    coroutine=true, debug=true, io=true, math=true, os=true, package=true,
    string=true, table=true, utf8=true, bit32=true,
}
local function fmt(v)
    local t = type(v)
    if t == 'string'  then
        local s = v:gsub('\n', '\\n'):gsub('\r', '\\r')
        if #s > 60 then s = s:sub(1,60)..'..' end
        return string.format('%q', s)
    end
    if t == 'number'  then return tostring(v) end
    if t == 'boolean' then return tostring(v) end
    if t == 'nil'     then return 'nil' end
    if t == 'function' then return 'function' end
    if t == 'table'   then
        local okc, n = pcall(function()
            local c=0; for _ in pairs(v) do c=c+1; if c>9999 then break end end; return c
        end)
        if okc then return string.format('table[#=%d]', n) end
        return 'table[?]'
    end
    return t
end
local keys = {}
for k in pairs(_G) do
    local ks = tostring(k)
    if not builtin[ks] then keys[#keys+1] = ks end
end
table.sort(keys)
for _, k in ipairs(keys) do
    local row_ok, row = pcall(function()
        local ok, v = pcall(function() return _G[k] end)
        if not ok then return string.format('%-32s %-12s %s', k, '?', '<unreadable>') end
        return string.format('%-32s %-12s %s', k, type(v), fmt(v))
    end)
    out[#out+1] = row_ok and row or string.format('%-32s %-12s %s', k, '?', '<error>')
end
return table.concat(out, '\n')
end)
if not _ok then return '!! globals dump error: '..tostring(_result) end
return _result
)LUA";

constexpr const char* DUMP_MODULES_LUA = R"LUA(
local _ok, _result = pcall(function()
local out = {}
local loaded = package and package.loaded or {}
local keys = {}
for k in pairs(loaded) do keys[#keys+1] = tostring(k) end
table.sort(keys)
for _, k in ipairs(keys) do
    local v = loaded[k]
    out[#out+1] = string.format('%-48s %s', k, type(v))
end
if package and package.path then
    out[#out+1] = ''
    out[#out+1] = '-- package.path:'
    for p in package.path:gmatch('[^;]+') do out[#out+1] = '  '..p end
end
return table.concat(out, '\n')
end)
if not _ok then return '!! modules dump error: '..tostring(_result) end
return _result
)LUA";

// quote arbitrary text as a Lua string literal (used to embed watch expressions as data
// rather than splicing them directly into chunk source)
std::string lua_quote(const std::string& text) {
    std::string result = "\"";
    for (auto c : text) {
        if (c == '\\') { result += "\\\\"; }
        else if (c == '"') { result += "\\\""; }
        else if (c == '\n') { result += "\\n"; }
        else { result += c; }
    }
    result += "\"";
    return result;
}

fs::path bridge_dir(const char* sub) {
    const char* appdata = std::getenv("APPDATA");
    fs::path base = appdata ? fs::path(appdata) : fs::path(".");
    return base / "UnrealVRMod" / "UEVR" / "ide_bridge" / sub;
}

// Run `chunk` in the host Lua state; returns the captured text (result or error).
std::string run_chunk(const std::string& chunk, const char* label) {
    auto fns = API::get()->param()->functions;
    if (fns == nullptr || fns->exec_lua_chunk == nullptr)
        return "!! exec_lua_chunk unavailable — rebuild UEVRBackend";
    static std::vector<char> out(64 * 1024);
    out[0] = '\0';
    fns->exec_lua_chunk(chunk.c_str(), label, out.data(), (unsigned int)out.size());
    return std::string(out.data());
}

void write_out(const std::string& reqId, const std::string& kind, const std::string& body) {
    std::error_code ec;
    fs::create_directories(g_out_dir, ec);
    std::ofstream f(g_out_dir / (reqId + ".txt"), std::ios::binary);
    if (f)
        f << kind << "\n" << body;
}

// Process one command file: parse kind + payload, run, write the result.
void handle_cmd(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    auto nl = content.find('\n');
    std::string kind = (nl == std::string::npos) ? content : content.substr(0, nl);
    std::string payload = (nl == std::string::npos) ? std::string() : content.substr(nl + 1);
    while (!kind.empty() && (kind.back() == '\r' || kind.back() == ' '))
        kind.pop_back();

    std::string reqId = p.stem().string();
    std::string result;

    if (kind == "globals") {
        result = run_chunk(DUMP_GLOBALS_LUA, "ide_bridge.globals");
    } else if (kind == "modules") {
        result = run_chunk(DUMP_MODULES_LUA, "ide_bridge.modules");
    } else if (kind == "inspect") {
        // Evaluate the expression and describe the value. A reflectable UObject/UStruct
        // (has :get_property_info(), see APIUE.lua) is dumped as tab-delimited
        // name/type/value rows so the IDE can render a real property table; a plain Lua
        // table falls back to the same 3-column shape over pairs(); anything else is a
        // single-line type/value description. Both row-producing branches emit the exact
        // same "name\ttype\tvalue" shape so the IDE-side table renderer doesn't need to
        // special-case which branch produced them.
        std::string chunk =
            "local ok, v = pcall(function() return " + payload + " end)\n"
            "if not ok then return '!! '..tostring(v) end\n"
            "local t = type(v)\n"
            "if t == 'userdata' and v.get_property_info then\n"
            "  local pok, props = pcall(function() return v:get_property_info() end)\n"
            "  if pok and props then\n"
            "    local out = {}\n"
            "    for _, p in ipairs(props) do\n"
            "      local vs = p.Value ~= nil and tostring(p.Value) or ''\n"
            "      out[#out+1] = tostring(p.name)..'\\t'..tostring(p.type)..'\\t'..vs\n"
            "    end\n"
            "    return table.concat(out, '\\n')\n"
            "  end\n"
            "end\n"
            "if t == 'table' then\n"
            "  local out = {}\n"
            "  local n = 0\n"
            "  for k, val in pairs(v) do n = n + 1; if n > 200 then break end\n"
            "    out[#out+1] = tostring(k)..'\\t'..type(val)..'\\t'..tostring(val) end\n"
            "  return table.concat(out, '\\n')\n"
            "end\n"
            "return '\\t'..t..'\\t'..tostring(v)\n";
        result = run_chunk(chunk, "ide_bridge.inspect");
        kind = "inspect";
    } else if (kind == "watch") {
        // Batched, stateless: the IDE resends the full expression list every poll (no
        // watch_add/remove registration -- the transport is already best-effort/stateless
        // everywhere else, and a handful of short expressions is cheap to replay in full).
        // Response is one "type\tpreview" line per expression, in the SAME ORDER sent --
        // order is the correlation key, no per-row expression echo needed.
        std::istringstream in(payload);
        std::string line;
        std::string lua_list = "{";
        while (std::getline(in, line)) {
            if (line.empty())
                continue;
            lua_list += lua_quote(line) + ",";
        }
        lua_list += "}";
        std::string chunk =
            "local exprs = " + lua_list + "\n"
            "local out = {}\n"
            "for _, src in ipairs(exprs) do\n"
            "  local lok, fn = pcall(load, 'return '..src)\n"
            "  local ok, v = false, nil\n"
            "  if lok and fn then ok, v = pcall(fn) end\n"
            "  if not ok then\n"
            "    out[#out+1] = 'error\\t'..tostring(v)\n"
            "  else\n"
            "    local s = tostring(v)\n"
            "    if #s > 200 then s = s:sub(1,200)..'..' end\n"
            "    out[#out+1] = type(v)..'\\t'..s\n"
            "  end\n"
            "end\n"
            "return table.concat(out, '\\n')\n";
        result = run_chunk(chunk, "ide_bridge.watch");
        kind = "watch";
    } else {
        // "run": execute the payload as a chunk, capturing errors.
        std::string chunk =
            "local ok, err = pcall(function()\n" + payload + "\nend)\n"
            "if not ok then return '!! '..tostring(err) end\n"
            "return '' \n";
        result = run_chunk(chunk, "ide_bridge.run");
        kind = "run";
    }

    write_out(reqId, kind, result);
    std::error_code ec;
    fs::remove(p, ec);
}

void poll_inbox() {
    std::error_code ec;
    if (!fs::is_directory(g_cmd_dir, ec))
        return;
    std::vector<fs::path> files;
    std::error_code iec;
    for (auto it = fs::directory_iterator(g_cmd_dir, fs::directory_options::skip_permission_denied, iec);
         !iec && it != fs::directory_iterator() && files.size() < 16; it.increment(iec)) {
        std::error_code fec;
        if (it->is_regular_file(fec) && !fec && it->path().extension() == ".txt")
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (auto& p : files)
        handle_cmd(p);
}

class BridgePlugin : public Plugin {
public:
    void on_initialize() override {
        g_cmd_dir = bridge_dir("cmd");
        g_out_dir = bridge_dir("out");
        std::error_code ec;
        fs::create_directories(g_cmd_dir, ec);
        fs::create_directories(g_out_dir, ec);
        API::get()->log_info("%s ready — inbox %s", TAG, g_cmd_dir.string().c_str());
    }

    void on_pre_engine_tick(API::UGameEngine*, float) override {
        // Poll a few times a second, not every frame (file I/O is cheap but
        // there's no reason to stat the folder at full framerate).
        if ((++g_frame % 12) != 0)
            return;
        poll_inbox();
    }
};

} // namespace

// Register the plugin instance with UEVR.
static BridgePlugin g_plugin;
