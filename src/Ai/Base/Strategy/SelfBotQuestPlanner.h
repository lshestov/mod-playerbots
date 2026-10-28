/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELFBOTQUESTPLANNER_H
#define PLAYERBOTS_SELFBOTQUESTPLANNER_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace SelfBotQuestPlanning
{
constexpr unsigned MaxActiveQuests = 6;
constexpr unsigned MaxLevelAboveHero = 3;
constexpr unsigned MaxItemUseAttempts = 3;
constexpr float PickupRadius = 150.0f;
constexpr float WorkRadius = 100.0f;
constexpr float NearbyTurnInRadius = 35.0f;
constexpr unsigned MaxWorkSteps = 32;
constexpr unsigned MaxWorkAgeSeconds = 600;
constexpr float InitialOpportunityRadius = 600.0f;
constexpr float InitialSourceRadius = 1200.0f;
constexpr float MaxTravelDistance = 2400.0f;
constexpr float LowValueTravelDistance = 600.0f;
constexpr unsigned LowValueXpDivisor = 200;
constexpr float MaxXpBenefit = 100.0f;
constexpr float XpBenefitScale = 1000.0f;
constexpr float ActiveWorkBenefit = 10.0f;
constexpr float ProgressBenefitDivisor = 5.0f;
constexpr float FollowUpBenefit = 5.0f;
constexpr float ReadyRewardBenefit = 30.0f;
constexpr float BaseTravelCost = 300.0f;
constexpr float ClusterTravelDiscount = 100.0f;
constexpr float UsefulUtilityFraction = 2.0f / 3.0f;
constexpr float CurrentUtilityImprovement = 1.25f;
constexpr float RouteAggroBuffer = 5.0f;
constexpr float UnsafeRouteThreat = -1.0f;
constexpr float BaseRouteThreatCost = 80.0f;
constexpr float RouteThreatLevelCost = 40.0f;
constexpr std::size_t NoCandidate = std::numeric_limits<std::size_t>::max();

enum class Task
{
    TurnIn,
    Objective,
    Item,
    Accept,
    Explore,
    UseItem
};

enum class ItemUseStep
{
    Completed,
    Wait,
    Defer,
    PrepareKill,
    Apply,
    PrepareWeaken
};

struct ItemUseHealthRange
{
    float MinPercent = 0.0f;
    float MaxPercent = 100.0f;
    bool MinInclusive = true;
    bool MaxInclusive = true;
};

enum class ItemUseHealthStep
{
    Ready,
    Weaken,
    TooLow,
    Invalid
};

inline ItemUseHealthStep EvaluateItemUseHealth(ItemUseHealthRange const& range, float percent)
{
    if (!std::isfinite(percent) || percent < 0.0f || percent > 100.0f || !std::isfinite(range.MinPercent) ||
        !std::isfinite(range.MaxPercent) || range.MinPercent < 0.0f || range.MaxPercent > 100.0f ||
        range.MinPercent > range.MaxPercent ||
        (range.MinPercent == range.MaxPercent && (!range.MinInclusive || !range.MaxInclusive)))
        return ItemUseHealthStep::Invalid;
    if (percent < range.MinPercent || (percent == range.MinPercent && !range.MinInclusive))
        return ItemUseHealthStep::TooLow;
    if (percent > range.MaxPercent || (percent == range.MaxPercent && !range.MaxInclusive))
        return ItemUseHealthStep::Weaken;
    return ItemUseHealthStep::Ready;
}

struct ItemUseObservation
{
    bool Needed = true;
    // Native progress for this source, not a successful dispatch or the preparatory kill.
    bool Progressed = false;
    bool Supported = true;
    bool Pending = false;
    bool HasItem = false;
    bool TargetPresent = false;
    bool TargetInPhase = false;
    bool RequiresDead = false;
    bool TargetAlive = false;
    bool AttackAllowed = false;
    unsigned Attempts = 0;
    ItemUseHealthRange HealthRange;
    float TargetHealthPercent = 100.0f;
};

inline ItemUseStep NextItemUseStep(ItemUseObservation const& observation)
{
    if (!observation.Needed || observation.Progressed)
        return ItemUseStep::Completed;
    if (!observation.Supported)
        return ItemUseStep::Defer;
    if (observation.Pending)
        return ItemUseStep::Wait;
    if (observation.Attempts >= MaxItemUseAttempts || !observation.HasItem || !observation.TargetPresent ||
        !observation.TargetInPhase)
        return ItemUseStep::Defer;
    if (observation.RequiresDead && observation.TargetAlive)
        return observation.AttackAllowed ? ItemUseStep::PrepareKill : ItemUseStep::Defer;
    if (!observation.RequiresDead && !observation.TargetAlive)
        return ItemUseStep::Defer;
    if (!observation.RequiresDead)
    {
        ItemUseHealthStep const health =
            EvaluateItemUseHealth(observation.HealthRange, observation.TargetHealthPercent);
        if (health == ItemUseHealthStep::Weaken)
            return observation.AttackAllowed ? ItemUseStep::PrepareWeaken : ItemUseStep::Defer;
        if (health != ItemUseHealthStep::Ready)
            return ItemUseStep::Defer;
    }
    return ItemUseStep::Apply;
}

enum class ItemUseContinuation
{
    None,
    Kill,
    Loot
};

enum class ItemUseContinuationStep
{
    Completed,
    Wait,
    RetryUse,
    Defer,
    Kill,
    Loot
};

struct ItemUseContinuationObservation
{
    bool Needed = true;
    bool Progressed = false;
    bool Supported = true;
    bool Pending = false;
    bool MechanicObserved = false;
    ItemUseContinuation Kind = ItemUseContinuation::None;
    bool Attributed = false;
    bool TargetPresent = false;
    bool TargetInPhase = false;
    bool TargetAlive = false;
    bool AttackAllowed = false;
};

inline ItemUseContinuationStep NextItemUseContinuationStep(ItemUseContinuationObservation const& observation)
{
    if (!observation.Needed || observation.Progressed)
        return ItemUseContinuationStep::Completed;
    if (!observation.Supported)
        return ItemUseContinuationStep::Defer;
    if (observation.Pending)
        return ItemUseContinuationStep::Wait;
    if (!observation.MechanicObserved)
        return ItemUseContinuationStep::RetryUse;
    if (!observation.Attributed || !observation.TargetPresent || !observation.TargetInPhase)
        return ItemUseContinuationStep::Defer;
    if (observation.Kind == ItemUseContinuation::None)
        return ItemUseContinuationStep::Wait;
    if (observation.TargetAlive)
        return observation.AttackAllowed ? ItemUseContinuationStep::Kill : ItemUseContinuationStep::Defer;
    return observation.Kind == ItemUseContinuation::Loot ? ItemUseContinuationStep::Loot
                                                         : ItemUseContinuationStep::Wait;
}

struct Candidate
{
    std::uint32_t QuestId = 0;
    std::uint64_t SourceKey = 0;
    Task Kind = Task::Accept;
    std::uint8_t Objective = 0;
    int QuestLevel = 0;
    std::uint32_t NativeQuestXp = 0;
    unsigned ProgressPercent = 0;
    bool HasFollowUp = false;
    float Distance = 0.0f;
    // An unmeasured route is an optimistic shortlist estimate; the executor validates it before movement.
    float RouteDistance = -1.0f;
    // Lower bound from a prospective giver to the remaining objectives and taker, not a complete route proof.
    float ContinuationDistance = 0.0f;
    float RiskCost = 0.0f;
    float OpportunityDistance = 0.0f;
    float PickupDistance = std::numeric_limits<float>::max();
    float WorkDistance = std::numeric_limits<float>::max();
    bool Eligible = true;
    bool Safe = true;
    bool Deferred = false;
    bool PathAvailable = true;
    bool Blocked = false;
};

struct State
{
    unsigned HeroLevel = 1;
    std::uint32_t LevelXp = 0;
    unsigned ActiveCount = 0;
    std::uint32_t OrderedQuestId = 0;
    bool PickupActive = false;
    bool PickupAllowed = true;
    bool WorkActive = false;
    unsigned WorkSteps = 0;
    unsigned WorkAgeSeconds = 0;
    bool HasCurrent = false;
    bool CurrentUsable = false;
    Task CurrentTask = Task::Accept;
    int CurrentQuestLevel = 0;
    float CurrentUtility = 0.0f;
    float OpportunityRadius = std::numeric_limits<float>::max();
};

enum class Reason
{
    None,
    KeepCurrent,
    PickupHub,
    NearbyPickupHub,
    NearbyTurnIn,
    NearbyObjective,
    WorkBatchComplete,
    AcceptedTask,
    NoSafeTask,
    LowerLevelPickup
};

struct Decision
{
    std::size_t Index = NoCandidate;
    Reason Why = Reason::None;
    bool KeepCurrent = false;
    bool StartPickup = false;
    bool PickupAtSource = false;
    bool ClosePickup = false;
    bool StartWork = false;
    bool CloseWork = false;
};

inline bool QuestLevelAllowed(int questLevel, unsigned heroLevel)
{
    return questLevel <= 0 || static_cast<unsigned>(questLevel) <= heroLevel + MaxLevelAboveHero;
}

inline unsigned EffectiveQuestLevel(int questLevel, unsigned heroLevel)
{
    return questLevel > 0 ? static_cast<unsigned>(questLevel) : heroLevel;
}

inline bool IsWorkTask(Task task)
{
    return task == Task::Objective || task == Task::Item || task == Task::Explore || task == Task::UseItem;
}

inline bool RequirementFeasible(unsigned required, unsigned current, unsigned provided, bool safeSource)
{
    return current >= required || provided >= required - current || safeSource;
}

inline float NextOpportunityRadius(float current, float maximum)
{
    current = std::min(current, MaxTravelDistance);
    maximum = std::min(maximum, MaxTravelDistance);
    if (current >= maximum)
        return current;
    return current >= maximum / 2.0f ? maximum : current * 2.0f;
}

inline float FutureSourceRadius(float opportunityRadius)
{
    float const radius = std::min(opportunityRadius, MaxTravelDistance) * 2.0f;
    return std::min(MaxTravelDistance, std::max(InitialSourceRadius, radius));
}

inline bool WithinOpportunity(Candidate const& candidate, State const& state)
{
    return candidate.OpportunityDistance <= std::min(state.OpportunityRadius, MaxTravelDistance);
}

inline float TravelDistance(Candidate const& candidate)
{
    return candidate.RouteDistance < 0.0f ? candidate.Distance : candidate.RouteDistance;
}

inline float TravelCost(Candidate const& candidate)
{
    return TravelDistance(candidate) + (candidate.Kind == Task::Accept ? candidate.ContinuationDistance : 0.0f);
}

inline float TravelLimit(Candidate const& candidate, State const& state)
{
    if (candidate.Kind == Task::TurnIn || state.OrderedQuestId)
        return MaxTravelDistance;
    bool const lowValue =
        state.LevelXp == 0 || static_cast<std::uint64_t>(candidate.NativeQuestXp) * LowValueXpDivisor < state.LevelXp;
    return lowValue ? LowValueTravelDistance : MaxTravelDistance;
}

inline float CandidateUtility(Candidate const& candidate, State const& state)
{
    float const xpBenefit =
        state.LevelXp == 0 ? 0.0f : std::min(MaxXpBenefit, XpBenefitScale * candidate.NativeQuestXp / state.LevelXp);
    float const benefit = xpBenefit + (IsWorkTask(candidate.Kind) ? ActiveWorkBenefit : 0.0f) +
                          std::min(candidate.ProgressPercent, 100u) / ProgressBenefitDivisor +
                          (candidate.HasFollowUp ? FollowUpBenefit : 0.0f) +
                          (candidate.Kind == Task::TurnIn ? ReadyRewardBenefit : 0.0f);
    bool const clustered = candidate.PickupDistance <= PickupRadius || candidate.WorkDistance <= WorkRadius;
    float const cost = std::max(0.0f, TravelCost(candidate) - (clustered ? ClusterTravelDiscount : 0.0f));
    return benefit / (BaseTravelCost + cost + candidate.RiskCost);
}

inline float RouteThreatCost(unsigned heroLevel, unsigned creatureLevel, bool elite, bool hostile,
                             float distanceToRoute, float aggroRange)
{
    if (!hostile)
        return 0.0f;
    if (!std::isfinite(distanceToRoute) || !std::isfinite(aggroRange) || distanceToRoute < 0.0f || aggroRange < 0.0f)
        return UnsafeRouteThreat;
    if (distanceToRoute > aggroRange + RouteAggroBuffer)
        return 0.0f;
    if (elite || creatureLevel > heroLevel + MaxLevelAboveHero)
        return UnsafeRouteThreat;
    unsigned const levelDifference = creatureLevel > heroLevel ? creatureLevel - heroLevel : 0;
    return BaseRouteThreatCost + RouteThreatLevelCost * levelDifference;
}

inline bool AttackSourceAllowed(unsigned templateMaxLevel, unsigned actualLevel, bool templateElite, bool actualElite,
                                unsigned heroLevel)
{
    return !templateElite && !actualElite && templateMaxLevel <= heroLevel + MaxLevelAboveHero &&
           actualLevel <= heroLevel + MaxLevelAboveHero;
}

inline bool Usable(Candidate const& candidate, State const& state)
{
    float const cost = TravelCost(candidate);
    return (!state.OrderedQuestId || candidate.QuestId == state.OrderedQuestId) && candidate.Eligible &&
           candidate.Safe && !candidate.Blocked && !candidate.Deferred && candidate.PathAvailable &&
           std::isfinite(candidate.Distance) && candidate.Distance >= 0.0f &&
           (candidate.Kind != Task::Accept ||
            (std::isfinite(candidate.ContinuationDistance) && candidate.ContinuationDistance >= 0.0f)) &&
           std::isfinite(cost) && cost >= 0.0f && cost <= TravelLimit(candidate, state) &&
           std::isfinite(candidate.RiskCost) && candidate.RiskCost >= 0.0f &&
           (candidate.Kind == Task::TurnIn || QuestLevelAllowed(candidate.QuestLevel, state.HeroLevel)) &&
           (candidate.Kind != Task::Accept || state.ActiveCount < MaxActiveQuests);
}

inline bool StableBefore(Candidate const& left, Candidate const& right)
{
    if (left.QuestId != right.QuestId)
        return left.QuestId < right.QuestId;
    if (left.SourceKey != right.SourceKey)
        return left.SourceKey < right.SourceKey;
    if (left.Kind != right.Kind)
        return left.Kind < right.Kind;
    return left.Objective < right.Objective;
}

inline Decision Choose(std::vector<Candidate> const& candidates, State const& state)
{
    Decision result;
    if (state.OrderedQuestId)
    {
        if (state.HasCurrent && state.CurrentUsable)
        {
            result.KeepCurrent = true;
            result.Why = Reason::KeepCurrent;
            return result;
        }
        for (std::size_t index = 0; index < candidates.size(); ++index)
        {
            Candidate const& candidate = candidates[index];
            if (!Usable(candidate, state) || !WithinOpportunity(candidate, state))
                continue;
            if (result.Index == NoCandidate || candidate.Kind < candidates[result.Index].Kind ||
                (candidate.Kind == candidates[result.Index].Kind &&
                 (TravelCost(candidate) + candidate.RiskCost <
                      TravelCost(candidates[result.Index]) + candidates[result.Index].RiskCost ||
                  (TravelCost(candidate) + candidate.RiskCost ==
                       TravelCost(candidates[result.Index]) + candidates[result.Index].RiskCost &&
                   StableBefore(candidate, candidates[result.Index])))))
                result.Index = index;
        }
        result.Why = result.Index == NoCandidate ? Reason::NoSafeTask : Reason::AcceptedTask;
        return result;
    }
    float bestUtility = 0.0f;
    for (Candidate const& candidate : candidates)
        if ((IsWorkTask(candidate.Kind) || candidate.Kind == Task::Accept) && Usable(candidate, state) &&
            WithinOpportunity(candidate, state))
            bestUtility = std::max(bestUtility, CandidateUtility(candidate, state));
    auto useful = [&](Candidate const& candidate)
    {
        return candidate.Kind == Task::TurnIn ||
               CandidateUtility(candidate, state) >= bestUtility * UsefulUtilityFraction;
    };
    unsigned lowestWorkLevel = std::numeric_limits<unsigned>::max();
    bool betterLowerWork = false;
    for (Candidate const& candidate : candidates)
        if (IsWorkTask(candidate.Kind) && Usable(candidate, state) && WithinOpportunity(candidate, state) &&
            useful(candidate))
        {
            unsigned const level = EffectiveQuestLevel(candidate.QuestLevel, state.HeroLevel);
            if (level < lowestWorkLevel)
                lowestWorkLevel = level;
            if (level < EffectiveQuestLevel(state.CurrentQuestLevel, state.HeroLevel) &&
                CandidateUtility(candidate, state) >= state.CurrentUtility * CurrentUtilityImprovement)
                betterLowerWork = true;
        }
    if (state.HasCurrent && state.CurrentUsable && (!IsWorkTask(state.CurrentTask) || !betterLowerWork))
    {
        result.KeepCurrent = true;
        result.Why = Reason::KeepCurrent;
        return result;
    }

    auto best = [&](auto allowed, auto score, bool preferLowerLevel = false, bool keepPickupHub = false)
    {
        std::size_t selected = NoCandidate;
        float selectedScore = std::numeric_limits<float>::max();
        float selectedTravelCost = std::numeric_limits<float>::max();
        unsigned lowestPickupLevel = std::numeric_limits<unsigned>::max();
        if (preferLowerLevel)
            for (Candidate const& candidate : candidates)
                if (candidate.Kind == Task::Accept && Usable(candidate, state) && allowed(candidate) &&
                    (keepPickupHub || (WithinOpportunity(candidate, state) && useful(candidate))))
                {
                    unsigned const level = EffectiveQuestLevel(candidate.QuestLevel, state.HeroLevel);
                    if (level < lowestPickupLevel)
                        lowestPickupLevel = level;
                }
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            Candidate const& candidate = candidates[i];
            if (!Usable(candidate, state) || !allowed(candidate) ||
                (!keepPickupHub && (!WithinOpportunity(candidate, state) || !useful(candidate))))
                continue;
            unsigned const level = EffectiveQuestLevel(candidate.QuestLevel, state.HeroLevel);
            if ((IsWorkTask(candidate.Kind) && level > lowestWorkLevel) ||
                (preferLowerLevel && candidate.Kind == Task::Accept && level > lowestPickupLevel))
                continue;
            float const value = score(candidate);
            float const travelCost = TravelCost(candidate);
            if (selected == NoCandidate || value < selectedScore ||
                (value == selectedScore &&
                 (travelCost < selectedTravelCost ||
                  (travelCost == selectedTravelCost && StableBefore(candidate, candidates[selected])))))
            {
                selected = i;
                selectedScore = value;
                selectedTravelCost = travelCost;
            }
        }
        return selected;
    };
    auto distance = [](Candidate const& candidate) { return TravelCost(candidate); };
    auto taskScore = [&](Candidate const& candidate) { return -CandidateUtility(candidate, state); };
    auto pickupScore = [](Candidate const& candidate)
    {
        return TravelCost(candidate) -
               (candidate.Kind == Task::TurnIn && candidate.Distance <= NearbyTurnInRadius ? NearbyTurnInRadius : 0.0f);
    };

    if (state.PickupActive && state.ActiveCount < MaxActiveQuests)
    {
        result.Index = best(
            [](Candidate const& candidate)
            {
                return candidate.PickupDistance <= PickupRadius &&
                       (candidate.Kind == Task::Accept || candidate.Kind == Task::TurnIn);
            },
            pickupScore, true, true);
        if (result.Index != NoCandidate)
        {
            result.Why = Reason::PickupHub;
            return result;
        }
    }
    if (state.PickupActive)
    {
        result.ClosePickup = true;
    }

    result.Index =
        best([](Candidate const& candidate)
             { return candidate.Kind == Task::TurnIn && candidate.Distance <= NearbyTurnInRadius; }, distance);
    if (result.Index != NoCandidate)
    {
        result.Why = Reason::NearbyTurnIn;
        return result;
    }

    bool const workExpired =
        state.WorkActive && (state.WorkSteps >= MaxWorkSteps || state.WorkAgeSeconds >= MaxWorkAgeSeconds);
    if (workExpired)
    {
        result.CloseWork = true;
        result.Index = best([](Candidate const& candidate) { return candidate.Kind == Task::TurnIn; }, distance);
        if (result.Index != NoCandidate)
        {
            result.Why = Reason::WorkBatchComplete;
            return result;
        }
    }

    result.Index = best(
        [&](Candidate const& candidate)
        {
            return candidate.Kind == Task::Accept &&
                   EffectiveQuestLevel(candidate.QuestLevel, state.HeroLevel) < lowestWorkLevel;
        },
        distance, true);
    if (result.Index != NoCandidate)
    {
        result.StartPickup = true;
        result.PickupAtSource = true;
        result.CloseWork = state.WorkActive;
        result.Why = Reason::LowerLevelPickup;
        return result;
    }

    if (!state.PickupActive && state.PickupAllowed && (!state.WorkActive || workExpired))
    {
        result.Index = best(
            [&](Candidate const& candidate)
            {
                return candidate.Kind == Task::Accept && candidate.Distance <= PickupRadius &&
                       EffectiveQuestLevel(candidate.QuestLevel, state.HeroLevel) <= lowestWorkLevel;
            },
            distance, true);
        if (result.Index != NoCandidate)
        {
            result.StartPickup = true;
            result.CloseWork = state.WorkActive;
            result.Why = Reason::NearbyPickupHub;
            return result;
        }
    }

    if (state.WorkActive && !workExpired)
    {
        result.Index = best(
            [](Candidate const& candidate)
            {
                return candidate.Kind != Task::Accept &&
                       (candidate.WorkDistance <= WorkRadius ||
                        (candidate.Kind == Task::TurnIn && candidate.Distance <= NearbyTurnInRadius));
            },
            taskScore);
        if (result.Index != NoCandidate)
        {
            result.Why = candidates[result.Index].Kind == Task::TurnIn ? Reason::NearbyTurnIn : Reason::NearbyObjective;
            return result;
        }
        result.CloseWork = true;
    }

    result.Index = best([](Candidate const& candidate) { return candidate.Kind != Task::Accept; }, taskScore);
    if (result.Index != NoCandidate)
    {
        bool const objective = IsWorkTask(candidates[result.Index].Kind);
        result.StartWork = objective;
        result.Why = Reason::AcceptedTask;
        return result;
    }

    result.Why = Reason::NoSafeTask;
    return result;
}

inline char const* ReasonName(Reason reason)
{
    switch (reason)
    {
        case Reason::KeepCurrent:
            return "current source remains useful";
        case Reason::PickupHub:
            return "collecting within fixed pickup hub";
        case Reason::NearbyPickupHub:
            return "starting nearby pickup hub";
        case Reason::NearbyTurnIn:
            return "nearby turn-in within work cluster";
        case Reason::NearbyObjective:
            return "useful lower-level objective within fixed work cluster";
        case Reason::WorkBatchComplete:
            return "bounded work batch ended; ready reward visit";
        case Reason::AcceptedTask:
            return "useful lower-level work or earned reward";
        case Reason::NoSafeTask:
            return "no safe executable task";
        case Reason::LowerLevelPickup:
            return "useful lower-level available quest before new work";
        default:
            return "none";
    }
}
}  // namespace SelfBotQuestPlanning

#endif
