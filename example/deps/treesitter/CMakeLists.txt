#	Tree-sitter runtime + grammars, built as an isolated static lib.
#	The core (lib.c) and generated grammar parsers are NOT warning-clean, so we
#	compile them with warnings off — the project's /W4 /WX must not apply here.

include(FetchContent)

# Pin every tag: grammars break ABI across versions.
FetchContent_Populate(treesitter_core QUIET
	GIT_REPOSITORY "https://github.com/tree-sitter/tree-sitter"
	GIT_TAG "v0.24.4")

FetchContent_Populate(treesitter_cpp QUIET
	GIT_REPOSITORY "https://github.com/tree-sitter/tree-sitter-cpp"
	GIT_TAG "v0.23.4")

FetchContent_Populate(treesitter_csharp QUIET
	GIT_REPOSITORY "https://github.com/tree-sitter/tree-sitter-c-sharp"
	GIT_TAG "v0.23.1")

FetchContent_Populate(treesitter_python QUIET
	GIT_REPOSITORY "https://github.com/tree-sitter/tree-sitter-python"
	GIT_TAG "v0.23.6")

FetchContent_Populate(treesitter_javascript QUIET
	GIT_REPOSITORY "https://github.com/tree-sitter/tree-sitter-javascript"
	GIT_TAG "v0.23.1")

# Pin v0.3.0: it's the last Lua tag at grammar ABI 14 (v0.4.0 is ABI 15, which
# the pinned core v0.24.4 — max ABI 14 — refuses to load, yielding an empty tree).
FetchContent_Populate(treesitter_lua QUIET
	GIT_REPOSITORY "https://github.com/tree-sitter-grammars/tree-sitter-lua"
	GIT_TAG "v0.3.0")

FetchContent_Populate(treesitter_go QUIET
	GIT_REPOSITORY "https://github.com/tree-sitter/tree-sitter-go"
	GIT_TAG "v0.23.4")

FetchContent_Populate(treesitter_rust QUIET
	GIT_REPOSITORY "https://github.com/tree-sitter/tree-sitter-rust"
	GIT_TAG "v0.23.2")

# Grammar sources: parser.c always, scanner.c only when the grammar has one.
file(GLOB TS_CPP_SRC    "${treesitter_cpp_SOURCE_DIR}/src/*.c")
file(GLOB TS_CSHARP_SRC "${treesitter_csharp_SOURCE_DIR}/src/*.c")
file(GLOB TS_PYTHON_SRC "${treesitter_python_SOURCE_DIR}/src/*.c")
file(GLOB TS_JS_SRC     "${treesitter_javascript_SOURCE_DIR}/src/*.c")
file(GLOB TS_LUA_SRC    "${treesitter_lua_SOURCE_DIR}/src/*.c")
file(GLOB TS_GO_SRC     "${treesitter_go_SOURCE_DIR}/src/*.c")
file(GLOB TS_RUST_SRC   "${treesitter_rust_SOURCE_DIR}/src/*.c")

add_library(treesitter STATIC
	${treesitter_core_SOURCE_DIR}/lib/src/lib.c
	${TS_CPP_SRC}
	${TS_CSHARP_SRC}
	${TS_PYTHON_SRC}
	${TS_JS_SRC}
	${TS_LUA_SRC}
	${TS_GO_SRC}
	${TS_RUST_SRC})

# api.h is public (our wrapper includes it); core/grammar internal headers private.
# Each grammar bundles its own src/tree_sitter/parser.h, so its src/ must be on
# the include path while ITS parser.c compiles — but they'd collide if listed
# globally. Set the include per-file instead.
target_include_directories(treesitter PUBLIC
	${treesitter_core_SOURCE_DIR}/lib/include)
target_include_directories(treesitter PRIVATE
	${treesitter_core_SOURCE_DIR}/lib/src)
set_source_files_properties(${TS_CPP_SRC}    PROPERTIES INCLUDE_DIRECTORIES "${treesitter_cpp_SOURCE_DIR}/src")
set_source_files_properties(${TS_CSHARP_SRC} PROPERTIES INCLUDE_DIRECTORIES "${treesitter_csharp_SOURCE_DIR}/src")
set_source_files_properties(${TS_PYTHON_SRC} PROPERTIES INCLUDE_DIRECTORIES "${treesitter_python_SOURCE_DIR}/src")
set_source_files_properties(${TS_JS_SRC}     PROPERTIES INCLUDE_DIRECTORIES "${treesitter_javascript_SOURCE_DIR}/src")
set_source_files_properties(${TS_LUA_SRC}    PROPERTIES INCLUDE_DIRECTORIES "${treesitter_lua_SOURCE_DIR}/src")
set_source_files_properties(${TS_GO_SRC}     PROPERTIES INCLUDE_DIRECTORIES "${treesitter_go_SOURCE_DIR}/src")
set_source_files_properties(${TS_RUST_SRC}   PROPERTIES INCLUDE_DIRECTORIES "${treesitter_rust_SOURCE_DIR}/src")

if(MSVC)
	target_compile_options(treesitter PRIVATE /W0 /wd4996 /wd4244 /wd4018)
else()
	target_compile_options(treesitter PRIVATE -w)
endif()

# Hand the runtime the paths to each grammar's tags query (don't hand-maintain).
target_compile_definitions(treesitter PUBLIC
	TS_CPP_TAGS_SCM="${treesitter_cpp_SOURCE_DIR}/queries/tags.scm"
	TS_CSHARP_TAGS_SCM="${treesitter_csharp_SOURCE_DIR}/queries/tags.scm"
	TS_PYTHON_TAGS_SCM="${treesitter_python_SOURCE_DIR}/queries/tags.scm"
	TS_JS_TAGS_SCM="${treesitter_javascript_SOURCE_DIR}/queries/tags.scm"
	TS_LUA_TAGS_SCM="${treesitter_lua_SOURCE_DIR}/queries/tags.scm"
	TS_GO_TAGS_SCM="${treesitter_go_SOURCE_DIR}/queries/tags.scm"
	TS_RUST_TAGS_SCM="${treesitter_rust_SOURCE_DIR}/queries/tags.scm")
