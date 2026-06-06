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

add_library(treesitter STATIC
	${treesitter_core_SOURCE_DIR}/lib/src/lib.c
	${treesitter_cpp_SOURCE_DIR}/src/parser.c
	${treesitter_cpp_SOURCE_DIR}/src/scanner.c)

# api.h is public (our wrapper includes it); core/grammar internal headers private.
target_include_directories(treesitter PUBLIC
	${treesitter_core_SOURCE_DIR}/lib/include)
target_include_directories(treesitter PRIVATE
	${treesitter_core_SOURCE_DIR}/lib/src
	${treesitter_cpp_SOURCE_DIR}/src)

if(MSVC)
	target_compile_options(treesitter PRIVATE /W0 /wd4996 /wd4244 /wd4018)
else()
	target_compile_options(treesitter PRIVATE -w)
endif()

# Hand the runtime the path to the C++ tags query so we don't hand-maintain it.
target_compile_definitions(treesitter PUBLIC
	TS_CPP_TAGS_SCM="${treesitter_cpp_SOURCE_DIR}/queries/tags.scm")
