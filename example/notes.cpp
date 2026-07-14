#include "notes.h"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

namespace notes {

std::string trim(const std::string& s)
{
	size_t b = 0, e = s.size();

	while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
		b++;
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
		e--;

	return s.substr(b, e - b);
}

void reanchor(Note& note, const std::vector<std::string>& lines)
{
	const int count = static_cast<int>(lines.size());

	if (count == 0)
	{
		note.line = 0;
		note.orphaned = true;
		return;
	}

	const std::string anchor = trim(note.anchor);

	// A note with no anchor text (blank line) can only be held by line number.
	if (anchor.empty())
	{
		note.orphaned = note.line >= count;
		note.line = std::clamp(note.line, 0, count - 1);
		return;
	}

	// still where we left it?
	if (note.line >= 0 && note.line < count && trim(lines[note.line]) == anchor)
	{
		note.orphaned = false;
		return;
	}

	// Search outward from the remembered line, nearest first, so an identical
	// line elsewhere in the file can't steal the note from the closest match.
	const int start = std::clamp(note.line, 0, count - 1);

	for (int radius = 1; radius < count; radius++)
	{
		const int up = start - radius;
		const int down = start + radius;

		if (up < 0 && down >= count)
			break;

		if (up >= 0 && trim(lines[up]) == anchor)
		{
			note.line = up;
			note.orphaned = false;
			return;
		}

		if (down < count && trim(lines[down]) == anchor)
		{
			note.line = down;
			note.orphaned = false;
			return;
		}
	}

	// the line this note was written against is gone
	note.line = std::clamp(note.line, 0, count - 1);
	note.orphaned = true;
}

void reanchorFile(std::vector<Note>& all, const std::string& file,
                  const std::vector<std::string>& lines)
{
	for (auto& note : all)
	{
		if (note.file == file)
			reanchor(note, lines);
	}
}

void shiftLines(std::vector<Note>& all, const std::string& file, int atLine, int delta)
{
	if (delta == 0)
		return;

	for (auto& note : all)
	{
		if (note.file != file || note.line < atLine)
			continue;

		// a note sitting ON a removed line stays put; the next re-anchor decides
		// whether its text moved or genuinely disappeared
		if (delta < 0 && note.line < atLine - delta)
			continue;

		note.line += delta;

		if (note.line < 0)
			note.line = 0;
	}
}

std::string toJson(const std::vector<Note>& all)
{
	nlohmann::json j = nlohmann::json::array();

	for (const auto& note : all)
	{
		j.push_back({
			{"file", note.file},
			{"line", note.line},
			{"text", note.text},
			{"anchor", note.anchor},
			{"commit", note.commit},
			{"author", note.author},
			{"epoch", note.epoch},
			{"resolved", note.resolved},
		});
	}

	return j.dump(2);
}

std::vector<Note> fromJson(const std::string& json)
{
	std::vector<Note> out;

	// A hand-edited or half-written sidecar must not take the editor down with
	// it — a bad file means "no notes", not a crash.
	nlohmann::json j = nlohmann::json::parse(json, nullptr, false);

	if (j.is_discarded() || !j.is_array())
		return out;

	for (const auto& e : j)
	{
		if (!e.is_object())
			continue;

		Note note;
		note.file = e.value("file", std::string());
		note.line = e.value("line", 0);
		note.text = e.value("text", std::string());
		note.anchor = e.value("anchor", std::string());
		note.commit = e.value("commit", std::string());
		note.author = e.value("author", std::string());
		note.epoch = e.value("epoch", 0LL);
		note.resolved = e.value("resolved", false);

		if (note.file.empty() || note.text.empty())
			continue;

		if (note.line < 0)
			note.line = 0;

		out.push_back(std::move(note));
	}

	return out;
}

} // namespace notes
