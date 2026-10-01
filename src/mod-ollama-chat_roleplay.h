#ifndef MOD_OLLAMA_CHAT_ROLEPLAY_H
#define MOD_OLLAMA_CHAT_ROLEPLAY_H

#include "ScriptMgr.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

class Player;

// Roleplay presence (see docs/roleplay.md): what makes a bot feel like a person living in the world rather than a chat source.
//
//  - a journal: the level-ups, zones, errands, deaths, great kills and finds of every bot, kept in a small ring and sent with the bot's
//    context, so its story follows what it really did (the mind service turns them into chapters and "lately" lines)
//  - companions: a bot in a party with a whitelisted player remarks on a moment of the road (a new zone, a great foe down, a fall)
//  - a greeting when a player a bot knows walks up to it, an emote back (and now and then a line) when a player bows, waves or thanks
//  - real characters of the world (an innkeeper, a guard, a trainer) answer a player who speaks to them in /say
//
// All of it needs the mind service in roleplay mode and does nothing otherwise.
namespace OllamaChat
{
    namespace Roleplay
    {
        // Any thread. Remember something a bot did ("arrived in Ashenvale"); the oldest of a bot's ten are dropped.
        void Note(uint64_t botGuid, const std::string& kind, const std::string& text);

        // [{"k": kind, "t": text, "ago": seconds}, ...], newest first, at most eight. [] when there is nothing.
        nlohmann::json RecentEvents(uint64_t botGuid);

        // World thread. The time of day and the holidays being kept, for the bot's brief ("time", "holidays").
        void AddWorldColour(nlohmann::json& brief, Player* bot);

        // World thread, every world update. Greetings, idle remarks of companions.
        void Tick(uint32_t diff);
    }
}

class RoleplayScript : public PlayerScript
{
public:
    RoleplayScript();
    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override;
    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 newArea) override;
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override;
    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override;
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override;
    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid lootGuid) override;
    void OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 emoteNum, ObjectGuid guid) override;
    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override;
};

#endif // MOD_OLLAMA_CHAT_ROLEPLAY_H
