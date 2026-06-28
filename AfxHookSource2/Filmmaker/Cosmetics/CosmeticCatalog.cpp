#include "CosmeticCatalog.h"

// Allocation-free classifier mirroring SlotForWeaponDef in Panorama/CameraEditorHud.cpp. The
// definition-index tables below MUST stay in sync with that switch -- they describe the same
// fixed CS2 econ item defs, just consumed by the native apply loop instead of the editor UI.

namespace Filmmaker {

namespace CosmeticCatalog {

namespace {

// Glove definitions live in a contiguous range (5027..5040 covers all current gloves; 5028 =
// default T hands, 5029 = default CT hands). A range check is forward-compat with new glove defs
// Valve adds inside the same block.
constexpr int kGloveDefMin = 5027;
constexpr int kGloveDefMax = 5040;

bool InGloveRange(int defIndex) {
	return defIndex >= kGloveDefMin && defIndex <= kGloveDefMax;
}

} // namespace

CosmeticSlot SlotForDefIndex(int defIndex) {
	if (InGloveRange(defIndex))
		return CosmeticSlot::Gloves;
	if (IsKnifeDef(defIndex))
		return CosmeticSlot::Knife;

	switch (defIndex) {
	case 1: case 2: case 3: case 4: case 30: case 32: case 36: case 61: case 63: case 64:
		return CosmeticSlot::Secondary;
	case 7: case 8: case 9: case 10: case 11: case 13: case 14: case 16: case 17: case 19:
	case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 33: case 34: case 35:
	case 38: case 39: case 40: case 60:
		return CosmeticSlot::Primary;
	default:
		return CosmeticSlot::None;
	}
}

bool IsKnifeDef(int defIndex) {
	switch (defIndex) {
	case 42: case 59: case 500: case 503: case 505: case 506: case 507: case 508: case 509:
	case 512: case 514: case 515: case 516: case 517: case 518: case 519: case 520: case 521:
	case 522: case 523: case 525: case 526:
		return true;
	default:
		return false;
	}
}

bool IsGloveDef(int defIndex) {
	return InGloveRange(defIndex);
}

bool IsDefaultKnifeDef(int defIndex) {
	return defIndex == 42 || defIndex == 59;
}

} // namespace CosmeticCatalog

} // namespace Filmmaker
