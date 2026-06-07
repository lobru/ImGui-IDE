#	nlohmann/json — single-header JSON library, used by the LSP client.
#	Header-only: fetch the source and expose its single_include via an INTERFACE
#	target (no build, matching the project's FetchContent_Populate convention).

include(FetchContent)

FetchContent_Populate(nlohmann_json QUIET
	GIT_REPOSITORY "https://github.com/nlohmann/json"
	GIT_TAG "v3.11.3")

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${nlohmann_json_SOURCE_DIR}/single_include")
