#include "CosmeticProfile.h"

#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../../hlaeFolder.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>

namespace Filmmaker {

bool CosmeticProfile::Empty() const {
	return !primary.set && !secondary.set && !knife.set && !gloves.set && !agent.set;
}

namespace {

// Slot names as written in / read from the JSON document.
constexpr const char* kSlotNames[] = { "primary", "secondary", "knife", "gloves" };

CosmeticItem* SlotByName(CosmeticProfile& p, const char* name) {
	if (std::strcmp(name, "primary") == 0) return &p.primary;
	if (std::strcmp(name, "secondary") == 0) return &p.secondary;
	if (std::strcmp(name, "knife") == 0) return &p.knife;
	if (std::strcmp(name, "gloves") == 0) return &p.gloves;
	return nullptr;
}

const CosmeticItem* SlotByName(const CosmeticProfile& p, const char* name) {
	if (std::strcmp(name, "primary") == 0) return &p.primary;
	if (std::strcmp(name, "secondary") == 0) return &p.secondary;
	if (std::strcmp(name, "knife") == 0) return &p.knife;
	if (std::strcmp(name, "gloves") == 0) return &p.gloves;
	return nullptr;
}

void WriteItem(JsonBuilder& b, const char* slotName, const CosmeticItem& item) {
	if (!item.set)
		return;
	b.Key(slotName);
	b.BeginObject();
	b.IntField("defIndex", item.defIndex);
	b.IntField("paintKit", item.paintKit);
	b.DoubleField("wear", item.wear);
	b.IntField("seed", item.seed);
	b.IntField("statTrak", item.statTrak);
	b.EndObject();
}

// Object member lookup helper that tolerates a missing/non-object parent (Find() returning
// nullptr, or the node not being an Object) by returning nullptr instead of crashing.
const JsonValue* FindIn(const JsonValue* obj, const char* key) {
	if (!obj || obj->type != JsonValue::Type::Object)
		return nullptr;
	return obj->Find(key);
}

void ReadItem(const JsonValue* slotObj, CosmeticItem& item) {
	if (!slotObj || slotObj->type != JsonValue::Type::Object)
		return;
	item.set = true;
	const JsonValue* v;
	v = FindIn(slotObj, "defIndex");  item.defIndex = v ? v->AsInt(0) : 0;
	v = FindIn(slotObj, "paintKit");  item.paintKit = v ? v->AsInt(0) : 0;
	v = FindIn(slotObj, "wear");      item.wear = v ? (float)v->AsNumber(0.0) : 0.0f;
	v = FindIn(slotObj, "seed");      item.seed = v ? v->AsInt(0) : 0;
	v = FindIn(slotObj, "statTrak");  item.statTrak = v ? v->AsInt(-1) : -1;
}

} // namespace

CosmeticProfile& CosmeticProfileStore::GetOrCreate(uint64_t steamId) {
	return m_profiles[steamId];
}

CosmeticProfile* CosmeticProfileStore::Find(uint64_t steamId) {
	auto it = m_profiles.find(steamId);
	return it != m_profiles.end() ? &it->second : nullptr;
}

const CosmeticProfile* CosmeticProfileStore::Find(uint64_t steamId) const {
	auto it = m_profiles.find(steamId);
	return it != m_profiles.end() ? &it->second : nullptr;
}

void CosmeticProfileStore::ClearPlayer(uint64_t steamId) {
	m_profiles.erase(steamId);
}

void CosmeticProfileStore::ClearAll() {
	m_profiles.clear();
}

std::wstring CosmeticProfileStore::FilePath() const {
	std::wstring path = GetHlaeRoamingAppDataFolderW();
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
		path += L'\\';
	path += L"cosmetic_profiles.json";
	return path;
}

bool CosmeticProfileStore::Save() const {
	JsonBuilder b;
	b.BeginObject();
	b.BoolField("enabled", m_enabled);
	b.Key("players");
	b.BeginObject();
	for (const auto& [steamId, profile] : m_profiles) {
		if (profile.Empty())
			continue;
		b.Key(std::to_string(steamId).c_str());
		b.BeginObject();
		if (!profile.name.empty())
			b.StringField("name", profile.name);
		WriteItem(b, "primary", profile.primary);
		WriteItem(b, "secondary", profile.secondary);
		WriteItem(b, "knife", profile.knife);
		WriteItem(b, "gloves", profile.gloves);
		if (profile.agent.set) {
			b.Key("agent");
			b.BeginObject();
			b.IntField("defIndex", profile.agent.defIndex);
			b.StringField("model", profile.agent.model);
			b.EndObject();
		}
		b.EndObject();
	}
	b.EndObject(); // players
	b.EndObject(); // root

	std::ofstream f(std::filesystem::path(FilePath()), std::ios::binary | std::ios::trunc);
	if (!f.is_open())
		return false;
	f << b.Str();
	return true;
}

bool CosmeticProfileStore::Load() {
	ClearAll();
	m_enabled = false;

	std::ifstream f(std::filesystem::path(FilePath()), std::ios::binary);
	if (!f.is_open())
		return false; // No file yet is normal -- not an error.

	std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

	JsonValue root;
	if (!JsonParse(text, root) || root.type != JsonValue::Type::Object)
		return false;

	if (const JsonValue* enabled = root.Find("enabled"))
		m_enabled = enabled->AsBool(false);

	const JsonValue* players = root.Find("players");
	if (players && players->type == JsonValue::Type::Object) {
		for (const auto& [key, value] : players->obj) {
			uint64_t steamId = std::strtoull(key.c_str(), nullptr, 10);
			if (steamId == 0)
				continue;
			if (value.type != JsonValue::Type::Object)
				continue;

			CosmeticProfile& p = m_profiles[steamId];
			if (const JsonValue* name = FindIn(&value, "name"))
				p.name = name->AsString("");

			for (const char* slotName : kSlotNames) {
				CosmeticItem* item = SlotByName(p, slotName);
				if (!item)
					continue;
				ReadItem(FindIn(&value, slotName), *item);
			}

			if (const JsonValue* agentObj = FindIn(&value, "agent")) {
				if (agentObj->type == JsonValue::Type::Object) {
					p.agent.set = true;
					if (const JsonValue* defIndex = FindIn(agentObj, "defIndex"))
						p.agent.defIndex = defIndex->AsInt(0);
					if (const JsonValue* model = FindIn(agentObj, "model"))
						p.agent.model = model->AsString("");
				}
			}
		}
	}

	return true;
}

} // namespace Filmmaker
