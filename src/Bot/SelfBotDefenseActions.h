/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELFBOTDEFENSEACTIONS_H
#define PLAYERBOTS_SELFBOTDEFENSEACTIONS_H

#include "Common.h"

class Action;
class ActionExecutionListener;
class PlayerbotAI;
class Unit;

ActionExecutionListener* CreateSelfBotDefenseListener(PlayerbotAI* botAI);
bool IsSelfBotDefenseAction(PlayerbotAI* botAI, Action* action);
bool IsSelfBotDefenseSpell(PlayerbotAI* botAI, uint32 spellId, Unit const* target);

#endif
