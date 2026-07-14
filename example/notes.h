//	Sticky notes — persistent, line-anchored annotations.
//
//	A note is a comment the user leaves on a line without touching the source.
//	It lives in a sidecar file (.imguiide/notes.json at the project root), so it
//	survives restarts and is shareable/committable alongside the code.
//
//	The whole problem with a line-anchored note is that lines MOVE. Storing a bare
//	line number rots the moment anyone inserts a line above it. So a note also
//	stores an `anchor`: the text of the line it was attached to. On load we
//	re-anchor — if the remembered line no longer holds that text, we search
//	outward for it and follow the line to wherever it went. A note whose line is
//	gone entirely becomes `orphaned` rather than silently pointing at a lie.
//
//	This header is UI-free and ImGui-free on purpose: the anchoring and the
//	serialization are exactly the parts worth testing headlessly.

#pragma once

#include <string>
#include <vector>

namespace notes {

struct Note {
	std::string file;      // path relative to the project root (posix separators)
	int         line = 0;  // 0-based line index (the CURRENT resolved line)
	std::string text;      // the note itself
	std::string anchor;    // text of the line when the note was written (trimmed)
	std::string commit;    // git HEAD (short) when the note was written; may be empty
	std::string author;    // git user.name when the note was written; may be empty
	long long   epoch = 0; // creation time (unix seconds)
	bool        orphaned = false; // its anchor line no longer exists in the file
	bool        resolved = false; // user ticked it off; kept for history
};

// Trim leading/trailing whitespace — anchors compare on the meaningful text, so
// a pure re-indent (clang-format, tab->spaces) doesn't orphan every note.
std::string trim(const std::string& s);

// Re-anchor one note against the file's current lines.
//
// 1. If `line` still holds the anchor text, it hasn't moved — keep it.
// 2. Otherwise search OUTWARD from `line` (nearest first, so a duplicate line
//    elsewhere in the file can't steal the note from the closest match) and move
//    the note to the first line whose trimmed text equals the anchor.
// 3. If no line matches, the note is orphaned: `line` is clamped into range and
//    `orphaned` is set, so the UI can show it as "the code this referred to is
//    gone" instead of pinning it to an unrelated line.
//
// An empty anchor (a note on a blank line) can only be resolved by line number.
void reanchor(Note& note, const std::vector<std::string>& lines);

// Re-anchor every note that belongs to `file` (others are left untouched).
void reanchorFile(std::vector<Note>& all, const std::string& file,
                  const std::vector<std::string>& lines);

// Shift notes in `file` after a pure insert/delete of whole lines, so a note
// keeps its position within the same session without a full re-anchor pass.
// `atLine` is where the change happened; `delta` is lines added (>0) or removed
// (<0). A note ON a deleted line is not moved (the next re-anchor orphans it).
void shiftLines(std::vector<Note>& all, const std::string& file, int atLine, int delta);

// Serialization. The sidecar is plain JSON so it diffs and merges like source.
std::string toJson(const std::vector<Note>& all);
std::vector<Note> fromJson(const std::string& json);

} // namespace notes
