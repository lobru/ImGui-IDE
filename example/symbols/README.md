# Symbol packs

Pregenerated `type → member` maps that give the editor Visual-Studio-style member
completion for whole frameworks the project index never parses — the C++ standard
library, the .NET BCL, Python/Lua stdlib, Unreal, and so on.

Every `.json` file in this folder (deployed next to the exe) **and** in
`%APPDATA%/ImGuiColorTextEdit/symbols/` is loaded once at startup and merged into
the completion table (`ts::registerTypeMembers`). Drop a file in, restart, done.

## Format

```json
{
  "name": "human label",
  "language": "C++",
  "types": {
    "TypeName": ["member1", "member2", "..."],
    "OtherType": ["..."]
  }
}
```

- **Keys** are the *simple* (unqualified) type name. The resolver reduces
  `std::filesystem::path p;` to `path`, `TArray<int> a;` to `TArray`, so key on
  `path` / `TArray`, not the qualified or templated spelling.
- **Members** are **names only** — completion inserts the name and you type the
  call. No signatures (yet).
- Merging is deduped and order-preserving, so a pack may overlap the built-in
  table or another pack without producing duplicate entries.
- A malformed file is skipped, not fatal.

## Generating more

These are hand-curated, but the format is deliberately trivial to emit from a
script or an LLM: point one at a framework's reference and ask for
`{type: [members]}`. Because packs are additive and load-time-merged, adding
coverage never risks a regression in the existing completion behavior.

Naming convention: `<framework>-<variant>.json` (e.g. `cpp-std.json`,
`dotnet-8.json`, `unreal-5.4.json`).
