#pragma once

#include "FollowCamera.h"
#include "../../ClientEntitySystem.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace Filmmaker {

// Entity/naming/attachment-resolution helpers shared by the target providers below and by
// FollowCamera's candidate discovery + state-JSON building (FollowTargetDiscovery.cpp,
// FollowCameraStateJson.cpp). Moved out of FollowCamera.cpp's anonymous namespace -- same
// implementations, now with external linkage so other Follow*.cpp files can call them.

CEntityInstance* EntityAt(int index);
std::string Lower(std::string value);
std::string EntityClass(CEntityInstance* entity);
bool IsFiniteOrigin(const FollowVec3& p);
bool ReadOrigin(CEntityInstance* entity, FollowVec3& out);
bool IsGrenadeClass(const std::string& className);
bool IsBombClass(const std::string& className);
bool IsWeaponClass(const std::string& className);
bool IsDroppedWeapon(CEntityInstance* entity, const std::string& className);
bool IsWeaponOrBomb(const std::string& className);
std::string GrenadeTypeName(const std::string& className);
std::string PrettyWeaponName(const std::string& className);
std::string EntityFriendlyName(CEntityInstance* entity);
uint64_t EntityHandleValue(CEntityInstance* entity);
FollowAttachPoint MakeAttachPoint(const char* id, const char* label, const char* kind,
	bool valid, int index, const char* source);
void AddAttachmentIfValid(std::vector<FollowAttachPoint>& out, CEntityInstance* entity,
	const char* id, const char* label, const char* kind);
std::vector<FollowAttachPoint> AvailableAttachPoints(CEntityInstance* entity, FollowTargetType type, bool weapon, bool bomb);
std::vector<std::string> AttachPointIds(const std::vector<FollowAttachPoint>& points);

struct AttachTransformResult {
	bool valid = false;
	bool oriented = false;
	int entityIndex = -1;
	uint64_t entityHandle = 0;
	std::string entityType;
	std::string className;
	std::string modelName;
	std::string attachId;
	std::string attachKind;
	int attachIndex = -1;
	std::string source;
	FollowVec3 pos;
	FollowAngles ang;
};

bool ResolvePlayerVirtualPoint(CEntityInstance* pawn, const std::string& id, AttachTransformResult& out);
AttachTransformResult ResolveAttachTransform(CEntityInstance* entity, int entityIndex,
	FollowTargetType type, const std::string& requestedAttachId);

std::string JsonEscape(const std::string& value);
void AppendAttachPointJson(std::ostringstream& o, const FollowAttachPoint& point);
const char* TeamName(int team);

class PlayerTargetProvider final : public IFollowTargetProvider {
public:
	explicit PlayerTargetProvider(int index) : m_index(index) {}
	bool IsValid() override {
		CEntityInstance* pawn = EntityAt(m_index);
		return pawn && pawn->IsPlayerPawn() && pawn->GetHealth() > 0;
	}
	FollowVec3 GetWorldPosition() override {
		FollowVec3 out;
		CEntityInstance* pawn = EntityAt(m_index);
		if (!pawn) return out;
		float eye[3] = {};
		pawn->GetRenderEyeOrigin(eye);
		out = FollowVec3{ eye[0], eye[1], eye[2] };
		return out;
	}
	bool GetWorldTransform(FollowVec3& pos, FollowAngles& ang) override {
		CEntityInstance* pawn = EntityAt(m_index);
		if (!pawn) { pos = {}; return false; }
		float eye[3] = {};
		pawn->GetRenderEyeOrigin(eye);
		pos = FollowVec3{ eye[0], eye[1], eye[2] };
		float a[3] = {};
		pawn->GetRenderEyeAngles(a);
		ang = FollowAngles{ a[0], a[1], a[2] }; // pitch, yaw, roll
		return true;
	}
	std::string GetDisplayName() override {
		CEntityInstance* pawn = EntityAt(m_index);
		if (!pawn) return "Missing player";
		auto controllerHandle = pawn->GetPlayerControllerHandle();
		CEntityInstance* controller = controllerHandle.IsValid() ? EntityAt(controllerHandle.GetEntryIndex()) : nullptr;
		const char* name = controller ? controller->GetPlayerName() : nullptr;
		return name && *name ? name : "Player";
	}
	FollowTargetStatus GetStatus() override {
		CEntityInstance* pawn = EntityAt(m_index);
		if (!pawn || !pawn->IsPlayerPawn()) return FollowTargetStatus::Missing;
		return pawn->GetHealth() > 0 ? FollowTargetStatus::Alive : FollowTargetStatus::Dead;
	}
	int EntityIndex() const override { return m_index; }
private:
	int m_index;
};

class EntityTargetProvider : public IFollowTargetProvider {
public:
	EntityTargetProvider(int index, FollowTargetType type) : m_index(index), m_type(type) {}
	bool IsValid() override {
		CEntityInstance* entity = EntityAt(m_index);
		if (!entity) return false;
		const std::string cls = EntityClass(entity);
		switch (m_type) {
		case FollowTargetType::Grenade: return IsGrenadeClass(cls);
		case FollowTargetType::Weapon: return IsWeaponOrBomb(cls);
		default: return true;
		}
	}
	FollowVec3 GetWorldPosition() override {
		FollowVec3 out;
		ReadOrigin(EntityAt(m_index), out);
		return out;
	}
	std::string GetDisplayName() override {
		const std::string cls = EntityClass(EntityAt(m_index));
		if (cls.empty()) return "Entity";
		if (m_type == FollowTargetType::Grenade) return GrenadeTypeName(cls);
		return PrettyWeaponName(cls);
	}
	FollowTargetStatus GetStatus() override {
		return IsValid() ? FollowTargetStatus::Active : FollowTargetStatus::Missing;
	}
	int EntityIndex() const override { return m_index; }
protected:
	int m_index;
	FollowTargetType m_type;
};

class AttachmentTargetProvider final : public EntityTargetProvider {
public:
	AttachmentTargetProvider(int index, FollowTargetType type, const std::string& attachment)
		: EntityTargetProvider(index, type), m_attachment(attachment) {}
	// Attach rides whatever entity is selected (a player pawn OR a weapon), so validity
	// is simply "the entity still exists" -- NOT a weapon-class check (that broke Player
	// Bone attach) and NOT a strict attachment lookup (world weapon models often lack
	// muzzle/shell points, which made Live Preview bail with "target disappeared"). The
	// exact point is resolved in GetWorldTransform, with an origin fallback.
	//
	// Player targets are the one exception: on death a CS2 pawn's render-eye/origin collapse
	// to (0,0,0) (the class effectively becomes an inert observer proxy), so "still exists"
	// is not enough -- without a health check every virtual attach point (chest/head/eyes/...)
	// silently resolved to world origin, i.e. camera "on the ground" instead of on the player.
	// Requiring health here routes dead targets through RunFrame's EXISTING death handling
	// (autoDisableOnDeath / holdLastKnownPosition), the same as LockOn+Player already gets via
	// PlayerTargetProvider::IsValid() below -- Attach was just never wired to it.
	bool IsValid() override {
		CEntityInstance* entity = EntityAt(m_index);
		if (!entity) return false;
		if (m_type == FollowTargetType::Player)
			return entity->IsPlayerPawn() && entity->GetHealth() > 0;
		return true;
	}
	FollowTargetStatus GetStatus() override {
		if (m_type == FollowTargetType::Player) {
			CEntityInstance* entity = EntityAt(m_index);
			if (!entity || !entity->IsPlayerPawn()) return FollowTargetStatus::Missing;
			return entity->GetHealth() > 0 ? FollowTargetStatus::Active : FollowTargetStatus::Dead;
		}
		return EntityTargetProvider::GetStatus();
	}
	FollowVec3 GetWorldPosition() override {
		FollowVec3 pos; FollowAngles ang;
		GetWorldTransform(pos, ang);
		return pos;
	}
	bool GetWorldTransform(FollowVec3& pos, FollowAngles& ang) override {
		AttachTransformResult resolved = ResolveAttachTransform(EntityAt(m_index), m_index, m_type, m_attachment);
		pos = resolved.pos;
		ang = resolved.ang;
		return resolved.valid && resolved.oriented;
	}
	// Just the entity name (player or weapon); the attach point is shown separately in the
	// "Attach pt" row, so appending "/ head" here only made the Target label noisy.
	std::string GetDisplayName() override {
		return EntityFriendlyName(EntityAt(m_index));
	}
private:
	std::string m_attachment;
};

// Follows a player's CURRENTLY HELD weapon: re-resolves the active weapon each frame
// so the camera tracks through weapon switches. With an attachment name it reads that
// point (muzzle/etc.) on the live weapon; otherwise the weapon's origin.
class HeldWeaponProvider final : public IFollowTargetProvider {
public:
	HeldWeaponProvider(int playerIndex, std::string attachment)
		: m_playerIndex(playerIndex), m_attachment(std::move(attachment)) {}
	int ActiveWeaponIndex() const {
		CEntityInstance* pawn = EntityAt(m_playerIndex);
		if (!pawn || !pawn->IsPlayerPawn()) return -1;
		const auto weapon = pawn->GetActiveWeaponHandle();
		return weapon.IsValid() ? weapon.GetEntryIndex() : -1;
	}
	bool IsValid() override {
		const int wi = ActiveWeaponIndex();
		if (wi < 0) return false;
		CEntityInstance* weapon = EntityAt(wi);
		if (!weapon) return false;
		// Attachment is optional: GetWorldTransform rides the weapon origin when the named
		// point (muzzle/etc.) isn't on the model, so don't invalidate on a missing point.
		return true;
	}
	FollowVec3 GetWorldPosition() override {
		FollowVec3 pos; FollowAngles ang;
		GetWorldTransform(pos, ang);
		return pos;
	}
	bool GetWorldTransform(FollowVec3& pos, FollowAngles& ang) override {
		pos = {};
		const int wi = ActiveWeaponIndex();
		AttachTransformResult resolved = ResolveAttachTransform(EntityAt(wi), wi, FollowTargetType::Weapon, m_attachment);
		pos = resolved.pos;
		ang = resolved.ang;
		return resolved.valid && resolved.oriented;
	}
	std::string GetDisplayName() override {
		CEntityInstance* weapon = EntityAt(ActiveWeaponIndex());
		const std::string cls = EntityClass(weapon);
		std::string base = cls.empty() ? "Held weapon" : PrettyWeaponName(cls);
		return m_attachment.empty() ? base : (base + " / " + m_attachment);
	}
	FollowTargetStatus GetStatus() override {
		CEntityInstance* pawn = EntityAt(m_playerIndex);
		if (!pawn || !pawn->IsPlayerPawn()) return FollowTargetStatus::Missing;
		if (pawn->GetHealth() <= 0) return FollowTargetStatus::Dead;
		return IsValid() ? FollowTargetStatus::Active : FollowTargetStatus::Missing;
	}
	int EntityIndex() const override { return ActiveWeaponIndex(); }
private:
	int m_playerIndex;
	std::string m_attachment;
};


} // namespace Filmmaker
