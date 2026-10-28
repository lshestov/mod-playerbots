/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELFBOTQUESTCATALOG_H
#define PLAYERBOTS_SELFBOTQUESTCATALOG_H

#include <array>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "MoveSplineInitArgs.h"
#include "QuestDef.h"
#include "TravelMgr.h"

class Player;

struct SelfBotQuestSource
{
    int32 Entry = 0;
    GuidPosition Position;
    uint32 AreaId = 0;
    uint8 CreatureMinLevel = 0;
    uint8 CreatureMaxLevel = 0;
    uint32 CreatureRank = 0;
    uint32 AreaTriggerId = 0;
    uint32 LocationId = 0;
    uint32 PhaseMask = uint32(PHASEMASK_ANYWHERE);
    uint32 FactionTemplateId = 0;
    uint32 NpcFlags = 0;
    float WanderDistance = 0.0f;
    uint32 MovementType = 0;
    float DetectionRange = 0.0f;
    uint32 UnitFlags = 0;
    bool Civilian = false;
    bool EventBound = false;
};

using SelfBotQuestSources = std::vector<SelfBotQuestSource const*>;

struct SelfBotQuestCorridor
{
    SelfBotQuestSources Sources;
    bool Complete = false;
    size_t VisitedCells = 0;
    size_t Examined = 0;
};

enum class SelfBotQuestItemKind : uint8
{
    Progress,
    Weaken,
    Transform,
    Summon
};

enum class SelfBotQuestItemTargetMode : uint8
{
    Explicit,
    NearbyUnit,
    Self
};

enum class SelfBotQuestItemContinuation : uint8
{
    None,
    Kill,
    Loot
};

struct SelfBotQuestItemUse
{
    uint32 ItemId = 0;
    uint32 SpellId = 0;
    uint8 Objective = 0;
    bool ItemObjective = false;
    bool RequiresDead = false;
    SelfBotQuestItemKind Kind = SelfBotQuestItemKind::Progress;
    SelfBotQuestItemTargetMode TargetMode = SelfBotQuestItemTargetMode::Explicit;
    SelfBotQuestItemContinuation Continuation = SelfBotQuestItemContinuation::None;
    uint32 ResultEntry = 0;
    uint32 ResultAura = 0;
    bool ResultAuraRemoved = false;
    bool SummonerIsTarget = false;
    float MinHealthPct = 0.0f;
    float MaxHealthPct = 100.0f;
    bool MinHealthExclusive = false;
    bool MaxHealthExclusive = false;
    SelfBotQuestSources Sources;
};

struct SelfBotQuestDefinition
{
    uint32 Id = 0;
    std::string Title;
    int32 QuestLevel = 0;
    uint32 MinLevel = 0;
    uint32 SuggestedPlayers = 0;
    bool Seasonal = false;
    std::string ExclusionReason;
    // Pickup feasibility also checks held/granted items and safe sources on the player's current map.
    bool Supported = false;
    std::string UnsupportedReason;
    SelfBotQuestSources Givers;
    SelfBotQuestSources Takers;
    SelfBotQuestSources ExplorationSources;
    std::vector<SelfBotQuestItemUse> ItemUses;
    std::array<SelfBotQuestSources, QUEST_ITEM_OBJECTIVES_COUNT> ItemSources;
    std::array<SelfBotQuestSources, QUEST_OBJECTIVES_COUNT> ObjectiveSources;
};

struct SelfBotQuestRow
{
    SelfBotQuestDefinition const* Quest = nullptr;
    QuestStatus Status = QUEST_STATUS_NONE;
    bool Rewarded = false;
    bool CanTake = false;
    bool CanAdd = false;
    std::string EligibilityReason;
};

class SelfBotQuestCatalog
{
public:
    static SelfBotQuestCatalog& Instance();

    // Call once on the world thread after the object, quest, loot and DBC stores are loaded.
    void Initialize();
    bool IsInitialized() const { return _initialized; }
    std::vector<SelfBotQuestDefinition> const& Definitions() const { return _definitions; }
    std::set<uint32> const& Areas() const { return _areas; }
    std::vector<SelfBotQuestRow> Evaluate(Player* player, WorldPosition const& origin, float radius) const;
    SelfBotQuestRow EvaluateQuest(Player* player, uint32 questId) const;
    SelfBotQuestDefinition const* Find(uint32 questId) const;
    float MaxSearchRadius(WorldPosition const& origin) const;
    SelfBotQuestCorridor QueryCreatureCorridor(uint32 mapId, Movement::PointsArray const& points, float radius,
                                               size_t maxCells = 256, size_t maxSpawns = 2048) const;
    static uint64 Fingerprint(Player* player);
    static bool IsAreaInRegion(uint32 areaId);
    static bool IsMapSupported(uint32 mapId);
    static bool IsInRegion(WorldPosition const& position);

private:
    struct GiverReference
    {
        size_t DefinitionIndex;
        SelfBotQuestSource const* Source;
    };

    struct MapIndex
    {
        std::map<int32, std::map<int32, std::vector<GiverReference>>> Cells;
        bool HasSources = false;
        float MinX = 0.0f;
        float MaxX = 0.0f;
        float MinY = 0.0f;
        float MaxY = 0.0f;
        float MinZ = 0.0f;
        float MaxZ = 0.0f;
    };

    bool _initialized = false;
    std::set<uint32> _areas;
    std::set<uint32> _maps;
    std::vector<SelfBotQuestDefinition> _definitions;
    std::deque<SelfBotQuestSource> _sources;
    std::unordered_map<uint32, size_t> _definitionIndex;
    std::unordered_map<uint32, MapIndex> _mapIndex;
    std::unordered_map<uint32, std::map<std::pair<int32, int32>, SelfBotQuestSources>> _creatureCells;
};

#endif
