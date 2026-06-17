#pragma once

// Runs the FilmmakerDemoInfo .NET helper (demofile-net) on a .dem to obtain a
// scoreboard WITH player names + map/duration, which the .dem.info matchmaking
// sidecar cannot provide (it has account ids only).
//
// The result is cached next to the demo as "<demo>.fmjson"; a cache newer than
// the demo is reused, so a folder only pays the parse cost once. All of this runs
// on the library's background scan thread, so blocking on the child process is OK.

#include "DemoEntry.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Filmmaker {

struct DemoHelperResult {
	bool ok = false;            // helper produced a usable parse
	int schemaVersion = 0;      // "v" field; mismatch -> stale cache, regenerate
	bool namesOnly = false;     // parse stopped early (names only; no duration/scores)
	std::string map;            // map name from the demo (may be empty)
	int durationSeconds = 0;    // demo length in seconds (0 = unknown)
	bool hasScoreboard = false; // at least one player row present
	int teamScore0 = 0;         // CT
	int teamScore1 = 0;         // T
	std::vector<ScoreboardPlayer> players;
};

// Parses demoPath via the helper (using/refreshing the "<demo>.fmjson" cache).
// Returns ok=false if the helper is missing or the demo could not be parsed.
//
// When wantedAccountIds is non-empty the caller already has the full scoreboard
// (e.g. from the .dem.info matchmaking sidecar) and only needs player NAMES: the
// helper runs in "names-fast" mode and cancels the parse as soon as every wanted
// account id has a name, which is dramatically faster than a full decode.
DemoHelperResult ReadDemoInfoViaHelper(const std::wstring& demoPath,
	const std::vector<uint32_t>& wantedAccountIds = {});

} // namespace Filmmaker
