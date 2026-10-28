/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELFBOTACTIONQUEUE_H
#define PLAYERBOTS_SELFBOTACTIONQUEUE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

enum class SelfBotActionKind
{
    Quest
};

enum class SelfBotQueueStatus
{
    Draft,
    Running,
    Paused,
    Blocked,
    Completed
};

enum class SelfBotQueueCommand
{
    AddQuest,
    Remove,
    Move,
    Clear,
    Start,
    Pause,
    Auto
};

struct SelfBotQueuedAction
{
    std::uint32_t Id = 0;
    SelfBotActionKind Kind = SelfBotActionKind::Quest;
    std::uint32_t QuestId = 0;
    std::string Title;
};

// Owned by the character's manager, so AI resets and Stop/Start preserve the route.
class SelfBotActionQueue
{
public:
    static constexpr std::size_t MaxActions = 64;

    std::uint32_t Revision() const { return _revision; }
    bool Selected() const { return _selected; }
    SelfBotQueueStatus Status() const { return _status; }
    std::string const& Reason() const { return _reason; }
    std::size_t Cursor() const { return _cursor; }
    std::vector<SelfBotQueuedAction> const& Actions() const { return _actions; }
    bool Running() const
    {
        return _selected && (_status == SelfBotQueueStatus::Running || _status == SelfBotQueueStatus::Blocked);
    }
    SelfBotQueuedAction const* Current() const { return _cursor < _actions.size() ? &_actions[_cursor] : nullptr; }
    bool Editable() const { return !Running(); }

    bool Append(SelfBotActionKind kind, std::uint32_t questId) { return Insert(kind, questId); }

    bool Insert(SelfBotActionKind kind, std::uint32_t questId, std::uint32_t beforeActionId = 0, std::string title = {})
    {
        if (!Editable() || kind != SelfBotActionKind::Quest || !questId || _actions.size() >= MaxActions ||
            _nextId == std::numeric_limits<std::uint32_t>::max() ||
            std::any_of(_actions.begin(), _actions.end(),
                        [questId](auto const& action) { return action.QuestId == questId; }))
            return false;
        auto before = _actions.end();
        if (beforeActionId)
        {
            before = std::find_if(_actions.begin(), _actions.end(),
                                  [beforeActionId](auto const& action) { return action.Id == beforeActionId; });
            if (before == _actions.end() || static_cast<std::size_t>(before - _actions.begin()) < _cursor)
                return false;
        }
        _actions.insert(before, {++_nextId, kind, questId, std::move(title)});
        _status = _selected ? SelfBotQueueStatus::Paused : SelfBotQueueStatus::Draft;
        _reason.clear();
        Changed();
        return true;
    }

    bool Move(std::uint32_t id, std::uint32_t beforeActionId)
    {
        if (!Editable())
            return false;
        auto const source =
            std::find_if(_actions.begin(), _actions.end(), [id](auto const& action) { return action.Id == id; });
        if (source == _actions.end() || static_cast<std::size_t>(source - _actions.begin()) < _cursor)
            return false;
        auto before = _actions.end();
        if (beforeActionId)
        {
            before = std::find_if(_actions.begin(), _actions.end(),
                                  [beforeActionId](auto const& action) { return action.Id == beforeActionId; });
            if (before == _actions.end() || static_cast<std::size_t>(before - _actions.begin()) < _cursor)
                return false;
        }
        if (source == before || source + 1 == before)
            return true;
        if (source < before)
            std::rotate(source, source + 1, before);
        else
            std::rotate(before, source, source + 1);
        Changed();
        return true;
    }

    bool Remove(std::uint32_t id)
    {
        if (!Editable())
            return false;
        auto const found =
            std::find_if(_actions.begin(), _actions.end(), [id](auto const& action) { return action.Id == id; });
        if (found == _actions.end())
            return false;
        if (static_cast<std::size_t>(found - _actions.begin()) < _cursor)
            --_cursor;
        _actions.erase(found);
        _status = _selected ? SelfBotQueueStatus::Paused : SelfBotQueueStatus::Draft;
        _reason.clear();
        Changed();
        return true;
    }

    bool Clear()
    {
        if (!Editable())
            return false;
        _actions.clear();
        _cursor = 0;
        _status = _selected ? SelfBotQueueStatus::Paused : SelfBotQueueStatus::Draft;
        _reason.clear();
        Changed();
        return true;
    }

    bool Start()
    {
        if (!Current())
            return false;
        _selected = true;
        _status = SelfBotQueueStatus::Running;
        _reason.clear();
        Changed();
        return true;
    }

    void Pause()
    {
        if (!_selected || _status == SelfBotQueueStatus::Completed || _status == SelfBotQueueStatus::Paused)
            return;
        _status = SelfBotQueueStatus::Paused;
        _reason.clear();
        Changed();
    }

    void UseAutomatic()
    {
        if (!_selected)
            return;
        Pause();
        _selected = false;
        Changed();
    }

    bool Complete(std::uint32_t id)
    {
        if (!Running() || !Current() || Current()->Id != id)
            return false;
        ++_cursor;
        _status = Current() ? SelfBotQueueStatus::Running : SelfBotQueueStatus::Completed;
        _reason.clear();
        Changed();
        return true;
    }

    void ReportBlocked(std::string reason)
    {
        if (!Running() || (_status == SelfBotQueueStatus::Blocked && _reason == reason))
            return;
        _status = SelfBotQueueStatus::Blocked;
        _reason = std::move(reason);
        Changed();
    }

    void ReportRunning()
    {
        if (!Running() || _status == SelfBotQueueStatus::Running)
            return;
        _status = SelfBotQueueStatus::Running;
        _reason.clear();
        Changed();
    }

private:
    void Changed()
    {
        if (!++_revision)
            ++_revision;
    }

    std::vector<SelfBotQueuedAction> _actions;
    std::uint32_t _revision = 1;
    std::uint32_t _nextId = 0;
    std::size_t _cursor = 0;
    bool _selected = false;
    SelfBotQueueStatus _status = SelfBotQueueStatus::Draft;
    std::string _reason;
};

#endif
