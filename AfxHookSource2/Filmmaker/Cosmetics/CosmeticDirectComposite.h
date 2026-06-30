#pragma once

#include <cstddef>
#include <cstdint>

namespace Filmmaker {

struct DirectCompositeResult {
	bool resolved = false;
	bool called = false;
	bool faulted = false;
};

// Lazily resolves Andromeda-style direct composite refresh functions from client.dll.
bool DirectCompositeResolved();

// Engine named-setter fallback for items with no networked paint attribute (viewmodel entities).
bool FireNamedSkinAttributes(unsigned char* itemView, int paintKit, float wear, int seed, int statTrak);

// Direct composite refresh on a weapon entity + its embedded C_EconItemView.
DirectCompositeResult FireDirectCompositeRefresh(
	unsigned char* weapon,
	unsigned char* itemView,
	ptrdiff_t offRestoreMaterial,
	ptrdiff_t compositeOwnerOffset,
	int32_t paintKit,
	float wear,
	int32_t seed);

} // namespace Filmmaker
