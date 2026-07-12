#
#  rename_aside.cmake — run in -P script mode before a link step (PRE_LINK).
#
#  Invoked as:  cmake -DFILE=<path-to-link-output> -P rename_aside.cmake
#
#  If the link output already exists and is locked by a running process (a live
#  ImGui-IDE-run.exe, or a loaded plugin DLL), the linker cannot overwrite it and
#  fails with LNK1168. Windows DOES, however, allow RENAMING a running image: the
#  process keeps executing from the renamed file, and the original name is freed
#  for the linker to write fresh. So we rename the existing output aside to a
#  unique <file>.old<N> and let the link proceed. Stale asides no longer locked by
#  any process are swept first. A no-op on the normal path (no existing file).
#

if(NOT DEFINED FILE OR NOT EXISTS "${FILE}")
	return()
endif()

# Reap asides from earlier builds whose process has since exited (locked ones just
# fail to remove and are left for a later sweep). Keeps the dir from growing.
file(GLOB _asides "${FILE}.old*")
foreach(_a IN LISTS _asides)
	file(REMOVE "${_a}")
endforeach()

# Try a plain delete first — if nothing has it open, that's the cleanest outcome
# and leaves no aside behind. file(REMOVE) is silent on failure (locked file).
file(REMOVE "${FILE}")
if(NOT EXISTS "${FILE}")
	return()
endif()

# Still present -> it's locked by a running process. Rename it aside to the first
# free .old<N> (a currently-running instance holds an earlier .old<N>, so its slot
# is skipped and the rename lands on an unused name).
set(_n 0)
while(EXISTS "${FILE}.old${_n}")
	math(EXPR _n "${_n} + 1")
endwhile()
# Plain RENAME (no RESULT keyword — that needs CMake 3.21; this project targets
# 3.17). The target name is guaranteed free and Windows permits renaming a running
# image, so this succeeds on the lock path we care about.
file(RENAME "${FILE}" "${FILE}.old${_n}")
