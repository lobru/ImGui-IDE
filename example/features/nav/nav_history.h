//	Navigation history — PURE (no SDL/ImGui). A back/forward stack of editor
//	locations, like a browser's. Go-to-definition (and other jumps) record the
//	origin; Back returns there, Forward re-applies. Selftest-covered.

#pragma once

#include <string>
#include <vector>

struct NavLocation {
	std::string file;
	int         line = 0;
	int         column = 0;
	bool valid() const { return !file.empty(); }
	// Two locations are "the same spot" if same file + line (column ignored, so a
	// jump that lands on the same line at a different column isn't double-recorded).
	bool sameSpot(const NavLocation& o) const { return file == o.file && line == o.line; }
};

class NavHistory {
public:
	// Record a jump: `from` (the location we're leaving) goes on the back stack and
	// the forward stack is cleared (a new jump branches history). Re-recording the
	// current back top is a no-op (still clears forward).
	void record(const NavLocation& from) {
		if (!from.valid())
			return;
		if (!mBack.empty() && mBack.back().sameSpot(from)) { mForward.clear(); return; }
		mBack.push_back(from);
		mForward.clear();
	}

	bool canBack() const { return !mBack.empty(); }
	bool canForward() const { return !mForward.empty(); }

	// Go back: push `current` onto the forward stack, pop the back stack into `out`.
	bool back(const NavLocation& current, NavLocation& out) {
		if (mBack.empty())
			return false;
		if (current.valid())
			mForward.push_back(current);
		out = mBack.back();
		mBack.pop_back();
		return true;
	}

	// Go forward: push `current` onto the back stack, pop the forward stack into `out`.
	bool forward(const NavLocation& current, NavLocation& out) {
		if (mForward.empty())
			return false;
		if (current.valid())
			mBack.push_back(current);
		out = mForward.back();
		mForward.pop_back();
		return true;
	}

	void clear() { mBack.clear(); mForward.clear(); }
	std::size_t backDepth() const { return mBack.size(); }
	std::size_t forwardDepth() const { return mForward.size(); }

private:
	std::vector<NavLocation> mBack, mForward;
};
