#include "CosmeticDirectComposite.h"

#include "CosmeticModelSwap.h"

#include "../../SchemaSystem.h"

#include "../../../shared/binutils.h"

#include <windows.h>

namespace Filmmaker {

namespace {

typedef void (__fastcall* DirectUpdateCompositeMaterial_t)(void* compositeOwner, bool force);
typedef void (__fastcall* DirectUpdateCompositeMaterialSet_t)(void* weapon, bool force);
typedef void (__fastcall* DirectUpdateSkin_t)(void* weapon, bool force);
typedef void (__fastcall* DirectSetAttributeValueByName_t)(void* itemView, const char* attrName, float value);

struct DirectCompositeFns {
	DirectUpdateCompositeMaterial_t updateCompositeMaterial = nullptr;
	DirectUpdateCompositeMaterialSet_t updateCompositeMaterialSet = nullptr;
	DirectUpdateSkin_t updateSkin = nullptr;
	DirectSetAttributeValueByName_t setAttributeValueByName = nullptr;
	bool resolved = false;
};

void* ResolveRelCall(size_t callAddr) {
	if (!callAddr)
		return nullptr;
	int32_t rel = *(int32_t*)(callAddr + 1);
	return (void*)(callAddr + 5 + rel);
}

size_t FindClientTextPattern(const char* pattern) {
	HMODULE client = GetModuleHandleA("client.dll");
	if (!client)
		return 0;
	Afx::BinUtils::ImageSectionsReader sections(client);
	if (sections.Eof())
		return 0;
	Afx::BinUtils::MemRange result = Afx::BinUtils::FindPatternString(sections.GetMemRange(), pattern);
	return result.IsEmpty() ? 0 : result.Start;
}

const DirectCompositeFns& ResolveDirectCompositeFns() {
	static bool s_attempted = false;
	static DirectCompositeFns s_fns = {};
	if (s_attempted)
		return s_fns;
	s_attempted = true;

	size_t updateCompositeCall = FindClientTextPattern("E8 ?? ?? ?? ?? 48 8D 8B ?? ?? ?? ?? 48 89 BC 24");
	size_t setAttrCall = FindClientTextPattern("E8 ?? ?? ?? ?? 66 41 0F 6E D4");
	size_t updateCompositeSet = FindClientTextPattern("40 55 53 41 57 48 8D AC 24 00 FE ?? ??");
	size_t updateSkin = FindClientTextPattern(
		"48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ?? ?? ?? ?? F6 C3 01 74 0A 33 D2 48 8B CF E8 ?? ?? ?? ?? 48 8D 8F 60 19 00 00");

	s_fns.updateCompositeMaterial = (DirectUpdateCompositeMaterial_t)ResolveRelCall(updateCompositeCall);
	s_fns.updateCompositeMaterialSet = (DirectUpdateCompositeMaterialSet_t)updateCompositeSet;
	s_fns.updateSkin = (DirectUpdateSkin_t)updateSkin;
	s_fns.setAttributeValueByName = (DirectSetAttributeValueByName_t)ResolveRelCall(setAttrCall);
	s_fns.resolved = s_fns.updateCompositeMaterial && s_fns.updateCompositeMaterialSet && s_fns.updateSkin;
	return s_fns;
}

} // namespace

bool DirectCompositeResolved() {
	return ResolveDirectCompositeFns().resolved;
}

bool FireNamedSkinAttributes(unsigned char* itemView, int paintKit, float wear, int seed, int statTrak) {
	const DirectCompositeFns& fns = ResolveDirectCompositeFns();
	if (!itemView || !fns.setAttributeValueByName)
		return false;
	__try {
		fns.setAttributeValueByName(itemView, "set item texture prefab", (float)paintKit);
		fns.setAttributeValueByName(itemView, "set item texture wear", wear);
		fns.setAttributeValueByName(itemView, "set item texture seed", (float)seed);
		if (statTrak >= 0)
			fns.setAttributeValueByName(itemView, "kill eater", (float)statTrak);
		return true;
	} __except (1) {
		return false;
	}
}

DirectCompositeResult FireDirectCompositeRefresh(
	unsigned char* weapon,
	unsigned char* itemView,
	ptrdiff_t offRestoreMaterial,
	ptrdiff_t compositeOwnerOffset,
	int32_t paintKit,
	float wear,
	int32_t seed) {
	DirectCompositeResult out = {};
	const DirectCompositeFns& fns = ResolveDirectCompositeFns();
	out.resolved = fns.resolved;
	if (!weapon || !itemView || !fns.resolved)
		return out;

	unsigned char* compositeOwner = weapon + compositeOwnerOffset;
	__try {
		if (offRestoreMaterial)
			*(bool*)(itemView + offRestoreMaterial) = true;
		if (fns.setAttributeValueByName) {
			fns.setAttributeValueByName(itemView, "set item texture prefab", (float)paintKit);
			fns.setAttributeValueByName(itemView, "set item texture wear", wear);
			fns.setAttributeValueByName(itemView, "set item texture seed", (float)seed);
		}
		fns.updateCompositeMaterial(compositeOwner, true);
		fns.updateCompositeMaterialSet(weapon, false);
		fns.updateSkin(weapon, true);
		PostDataUpdate(weapon);
		out.called = true;
	} __except (1) {
		out.faulted = true;
	}
	return out;
}

} // namespace Filmmaker
