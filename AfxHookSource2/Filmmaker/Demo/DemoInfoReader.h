#pragma once

// Reads the per-player scoreboard from a <demo>.dem.info match sidecar
// (CDataGCCStrike15_v2_MatchInfo). Validated against real CS2 match files:
//   MatchInfo.roundstatsall = field 5 (repeated; the LAST entry holds final totals)
//   roundstats.reservation  = field 2  -> account_ids = field 1 (repeated)
//   roundstats.kills        = field 5  (repeated, parallel to account_ids)
//   roundstats.assists      = field 6
//   roundstats.deaths       = field 7
//   roundstats.score        = field 8
//   roundstats.team_scores  = field 12 (two values)

#include "DemoEntry.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Filmmaker {

struct DemoInfoResult {
	bool ok = false;
	int teamScore0 = 0;
	int teamScore1 = 0;
	int64_t matchTime = 0; // CDataGCCStrike15_v2_MatchInfo.matchtime (field 2), unix seconds; 0 = unknown
	std::vector<ScoreboardPlayer> players;
};

// demoPath is the .dem file; the sidecar is looked up as demoPath + ".info".
DemoInfoResult ReadDemoInfo(const std::wstring& demoPath);

} // namespace Filmmaker
