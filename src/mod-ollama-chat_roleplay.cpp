#include "mod-ollama-chat_roleplay.h"

#include "mod-ollama-chat-utilities.h"
#include "mod-ollama-chat_ambient.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_gateway.h"
#include "mod-ollama-chat_promotion.h"
#include "mod-ollama-chat_tools.h"
#include "mod-ollama-chat_worldtask.h"

#include "CellImpl.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameEventMgr.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Random.h"
#include "World.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <ctime>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace OllamaChat
{
    namespace Roleplay
    {
        void NoteHuman(Player* human, const char* kind, const std::string& text);   // below: news about a whitelisted player

        namespace
        {
            struct Event
            {
                time_t      at = 0;
                std::string kind;
                std::string text;
            };

            std::mutex s_mutex;
            std::unordered_map<uint64_t, std::deque<Event>> s_events;               // bot guid -> what it did lately
            std::unordered_map<uint64_t, std::pair<std::string, time_t>> s_lastZone; // bot guid -> the zone last noted, and when
            std::unordered_map<uint64_t, time_t> s_botRemarkAt;                      // bot guid -> when it last remarked to its companions
            std::unordered_map<uint64_t, time_t> s_groupRemarkAt;                    // group -> when a companion last remarked
            std::unordered_map<uint64_t, time_t> s_groupIdleAt;                      // group -> when an idle remark was last considered
            std::unordered_map<uint64_t, time_t> s_greetedAt;                        // bot ^ operator -> when it last greeted them
            std::unordered_map<uint64_t, time_t> s_operatorGreetAt;                  // operator guid -> when any bot last greeted them
            std::unordered_map<uint64_t, time_t> s_emoteAt;                          // bot guid -> when it last answered an emote
            std::unordered_map<uint64_t, time_t> s_npcAt;                            // creature guid -> when it last answered
            uint32_t s_accumMs = 0;

            uint32_t Number(const char* key, uint32_t fallback) { return sConfigMgr->GetOption<uint32_t>(key, fallback, false); }

            bool IsBot(Player* player)
            {
                PlayerbotAI* ai = player ? PlayerbotsMgr::instance().GetPlayerbotAI(player) : nullptr;
                return ai && ai->IsBotAI();
            }

            // Everything here is for roleplay mode, with the gateway on.
            bool Active()
            {
                return g_GatewayEnable && Ambient::RoleplayActive() && sConfigMgr->GetOption<bool>("OllamaChat.Ambient.Enable", true);
            }

            std::string ToLower(std::string text)
            {
                std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return text;
            }

            // Whole-word search in a lowercased line.
            bool HasWord(const std::string& lowerText, const std::string& word)
            {
                if (word.size() < 3)
                    return false;
                for (size_t pos = lowerText.find(word); pos != std::string::npos; pos = lowerText.find(word, pos + 1))
                {
                    const bool startOk = pos == 0 || !std::isalnum(static_cast<unsigned char>(lowerText[pos - 1]));
                    const size_t end = pos + word.size();
                    const bool endOk = end >= lowerText.size() || !std::isalnum(static_cast<unsigned char>(lowerText[end]));
                    if (startOk && endOk)
                        return true;
                }
                return false;
            }

            std::string ZoneName(uint32 zoneId)
            {
                if (auto const* zone = sAreaTableStore.LookupEntry(zoneId))
                    return zone->area_name[LocaleConstant::LOCALE_enUS];
                return {};
            }

            // The brief a request carries: what the mind service needs to make the bot a character and to know where it is.
            void FillBrief(nlohmann::json& request, Player* bot)
            {
                nlohmann::json brief = DescribeBotBrief(bot);
                brief.erase("name");
                brief.erase("health_pct");
                for (auto it = brief.begin(); it != brief.end(); ++it)
                    request[it.key()] = it.value();
            }

            std::string KeyOf(uint64_t a, uint64_t b) { return std::to_string(a) + ":" + std::to_string(b); }
            uint64_t PairKey(uint64_t a, uint64_t b) { return a * 1000003ull ^ b; }

            // The whitelisted player in this bot's party, if any.
            Player* HumanCompanion(Player* bot)
            {
                Group* group = bot->GetGroup();
                if (!group)
                    return nullptr;
                for (auto const& slot : group->GetMemberSlots())
                {
                    Player* member = ObjectAccessor::FindPlayer(slot.guid);
                    if (member && member->IsInWorld() && !IsBot(member) && OllamaChat::Promotion::IsOperator(member))
                        return member;
                }
                return nullptr;
            }

            uint32_t TypingMs(size_t chars)
            {
                const uint64_t want = std::min<uint64_t>(Number("OllamaChat.Roleplay.TypingMaxMs", 3000),
                                                        Number("OllamaChat.Roleplay.TypingBaseMs", 300) + chars * Number("OllamaChat.Roleplay.TypingMsPerChar", 22));
                return static_cast<uint32_t>(want);
            }

            // ---- companions ----------------------------------------------------------------------------------------

            // A bot that travels with a whitelisted player says something about a moment of the road, in its own voice. A worker thread asks the
            // mind service (a model writes it: the moment is specific); the line goes to the party a moment later.
            void Companion(Player* bot, const char* event, const std::string& detail, uint32_t chancePercent, uint32_t delayMs)
            {
                if (!Active() || !Number("OllamaChat.Roleplay.CompanionRemarks", 1) || !bot || !bot->IsInWorld() || !IsBot(bot))
                    return;
                Player* human = HumanCompanion(bot);
                if (!human)
                    return;
                const uint64_t botGuid = bot->GetGUID().GetRawValue();
                const uint64_t groupKey = bot->GetGroup()->GetGUID().GetRawValue();
                const time_t now = time(nullptr);
                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    auto group = s_groupRemarkAt.find(groupKey);
                    if (group != s_groupRemarkAt.end() && now - group->second < static_cast<time_t>(Number("OllamaChat.Roleplay.CompanionGapSec", 75)))
                        return;
                    auto own = s_botRemarkAt.find(botGuid);
                    if (own != s_botRemarkAt.end() && now - own->second < static_cast<time_t>(Number("OllamaChat.Roleplay.CompanionBotGapSec", 150)))
                        return;
                    if (urand(0, 99) >= chancePercent)
                        return;
                    s_groupRemarkAt[groupKey] = now;
                    s_botRemarkAt[botGuid] = now;
                }
                if (IsGatewayBot(botGuid) || !TryAcquireGatewaySlot())
                    return;
                nlohmann::json request = {{"mode", "companion"}, {"event", event}, {"detail", detail}, {"bot_guid", botGuid}, {"bot_name", bot->GetName()}};
                FillBrief(request, bot);
                nlohmann::json companions = nlohmann::json::array();
                for (auto const& slot : bot->GetGroup()->GetMemberSlots())
                    if (slot.guid != bot->GetGUID())
                        companions.push_back(OllamaChat::Promotion::ShortName(slot.name));
                request["companions"] = companions;
                std::thread([botGuid, request, delayMs]() {
                    struct SlotGuard { ~SlotGuard() { ReleaseGatewaySlot(); } } slot;
                    try
                    {
                        if (delayMs)
                            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                        const nlohmann::json answer = Ambient::MindPost("/ambient", request, Number("OllamaChat.Ambient.TimeoutSeconds", 15));
                        const std::string text = answer.is_object() ? answer.value("text", std::string{}) : std::string{};
                        if (text.empty() || text.size() > 220)
                            return;
                        std::this_thread::sleep_for(std::chrono::milliseconds(TypingMs(text.size())));
                        OllamaChat::WorldTask::Run([botGuid, text]() -> nlohmann::json {
                            Player* speaker = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
                            PlayerbotAI* ai = speaker && speaker->IsInWorld() && speaker->IsAlive() ? PlayerbotsMgr::instance().GetPlayerbotAI(speaker) : nullptr;
                            if (ai && speaker->GetGroup())
                                ai->SayToParty(text);
                            return nlohmann::json{};
                        }, 3000);
                    }
                    catch (const std::exception& error)
                    {
                        LOG_ERROR("server.loading", "[Ollama Chat Roleplay] companion remark failed: {}", error.what());
                    }
                }).detach();
            }

            // ---- the journal ---------------------------------------------------------------------------------------

            // Remember it for a bot, and let a companion remark on it when it is the kind of moment that deserves a word.
            void Observe(Player* bot, const char* kind, const std::string& text, const char* companionEvent, const std::string& detail,
                         uint32_t chancePercent, uint32_t delayMs)
            {
                if (bot && !IsBot(bot))
                {
                    NoteHuman(bot, kind, text);     // not a journal: news, which the bots may pass on
                    return;
                }
                if (!bot)
                    return;
                Note(bot->GetGUID().GetRawValue(), kind, text);
                if (companionEvent)
                    Companion(bot, companionEvent, detail, chancePercent, delayMs);
            }

            // ---- an answer to an emote -----------------------------------------------------------------------------

            struct EmoteAnswer
            {
                const char* token;   // what the mind service calls it
                uint32      back;    // the emote the bot makes in return
            };

            bool AnswerFor(uint32 textEmote, EmoteAnswer& out)
            {
                switch (textEmote)
                {
                    case TEXT_EMOTE_BOW:     out = {"bow", EMOTE_ONESHOT_BOW}; return true;
                    case TEXT_EMOTE_SALUTE:  out = {"salute", EMOTE_ONESHOT_SALUTE}; return true;
                    case TEXT_EMOTE_KNEEL:   out = {"kneel", EMOTE_ONESHOT_BOW}; return true;
                    case TEXT_EMOTE_WAVE:    out = {"wave", EMOTE_ONESHOT_WAVE}; return true;
                    case TEXT_EMOTE_HELLO:   out = {"hello", EMOTE_ONESHOT_WAVE}; return true;
                    case TEXT_EMOTE_GREET:   out = {"greet", EMOTE_ONESHOT_BOW}; return true;
                    case TEXT_EMOTE_BYE:     out = {"bye", EMOTE_ONESHOT_WAVE}; return true;
                    case TEXT_EMOTE_THANK:   out = {"thank", EMOTE_ONESHOT_BOW}; return true;
                    case TEXT_EMOTE_LAUGH:   out = {"laugh", EMOTE_ONESHOT_LAUGH}; return true;
                    case TEXT_EMOTE_CHUCKLE: out = {"chuckle", EMOTE_ONESHOT_LAUGH}; return true;
                    case TEXT_EMOTE_CHEER:   out = {"cheer", EMOTE_ONESHOT_CHEER}; return true;
                    case TEXT_EMOTE_CRY:     out = {"cry", EMOTE_ONESHOT_CRY}; return true;
                    case TEXT_EMOTE_RUDE:    out = {"rude", EMOTE_ONESHOT_RUDE}; return true;
                    case TEXT_EMOTE_SPIT:    out = {"spit", EMOTE_ONESHOT_RUDE}; return true;
                    case TEXT_EMOTE_FLIRT:   out = {"flirt", EMOTE_ONESHOT_LAUGH}; return true;
                    case TEXT_EMOTE_POINT:   out = {"point", EMOTE_ONESHOT_POINT}; return true;
                    case TEXT_EMOTE_BEG:     out = {"beg", EMOTE_ONESHOT_BEG}; return true;
                    default: return false;
                }
            }

            // The bot nearest to `player` that could answer an emote aimed at nobody in particular.
            Player* NearestBot(Player* player, float range)
            {
                Player* best = nullptr;
                float bestDistance = range;
                for (auto const& entry : ObjectAccessor::GetPlayers())
                {
                    Player* bot = entry.second;
                    if (!bot || bot == player || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->GetMapId() != player->GetMapId() || !IsBot(bot))
                        continue;
                    const float distance = bot->GetDistance(player);
                    if (distance < bestDistance && !IsGatewayBot(bot->GetGUID().GetRawValue()))
                    {
                        best = bot;
                        bestDistance = distance;
                    }
                }
                return best;
            }

            // ---- real characters of the world ----------------------------------------------------------------------

            bool WorldFolk(Creature* creature)
            {
                return creature->IsGuard() || creature->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_FLIGHTMASTER
                                                                   | UNIT_NPC_FLAG_BANKER | UNIT_NPC_FLAG_AUCTIONEER | UNIT_NPC_FLAG_STABLEMASTER | UNIT_NPC_FLAG_QUESTGIVER);
            }

            // Is the line spoken to this person? Aimed at them (selected), or naming them: a word of their name or their title, or their role.
            bool Addressed(Creature* creature, Player* player, const std::string& lowerMsg)
            {
                if (player->GetSelectedUnit() == creature)
                    return true;
                std::string names = ToLower(creature->GetName());
                if (CreatureTemplate const* tpl = creature->GetCreatureTemplate())
                    names += " " + ToLower(tpl->SubName);
                size_t start = 0;
                while (start < names.size())
                {
                    size_t end = names.find_first_of(" ,-", start);
                    if (end == std::string::npos)
                        end = names.size();
                    if (HasWord(lowerMsg, names.substr(start, end - start)))
                        return true;
                    start = end + 1;
                }
                static const char* const kRoles[] = {"innkeeper", "guard", "captain", "trainer", "vendor", "merchant", "banker", "stablemaster", "auctioneer", "flightmaster"};
                for (const char* role : kRoles)
                    if (WorldFolk(creature) && HasWord(lowerMsg, role) && names.find(role) != std::string::npos)
                        return true;
                return false;
            }

            void NpcReply(Player* player, const std::string& message)
            {
                if (!Active() || !Number("OllamaChat.Roleplay.NpcReplies", 1) || !player || !player->IsInWorld() || IsBot(player) || !OllamaChat::Promotion::IsOperator(player))
                    return;
                const float range = static_cast<float>(Number("OllamaChat.Roleplay.NpcRangeYards", 10));
                std::list<Creature*> creatures;
                Acore::AllWorldObjectsInRange check(player, range);
                Acore::CreatureListSearcher<Acore::AllWorldObjectsInRange> searcher(player, creatures, check);
                Cell::VisitObjects(player, searcher, range);
                const std::string lower = ToLower(message);
                const time_t now = time(nullptr);
                Creature* chosen = nullptr;
                for (Creature* creature : creatures)
                {
                    if (!creature || !creature->IsAlive() || creature->IsPet() || creature->IsTotem() || creature->IsSummon() || !WorldFolk(creature))
                        continue;
                    if (creature->IsHostileTo(player) && !creature->IsGuard())
                        continue;
                    if (!Addressed(creature, player, lower))
                        continue;
                    if (!chosen || (player->GetSelectedUnit() == creature))
                        chosen = creature;
                }
                if (!chosen)
                    return;
                const uint64_t creatureGuid = chosen->GetGUID().GetRawValue();
                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    auto last = s_npcAt.find(creatureGuid);
                    if (last != s_npcAt.end() && now - last->second < static_cast<time_t>(Number("OllamaChat.Roleplay.NpcGapSec", 5)))
                        return;
                    s_npcAt[creatureGuid] = now;
                    if (s_npcAt.size() > 400)
                        for (auto it = s_npcAt.begin(); it != s_npcAt.end();)
                            it = (now - it->second > 600) ? s_npcAt.erase(it) : std::next(it);
                }
                if (!TryAcquireGatewaySlot())
                    return;
                const std::string attitude = chosen->IsFriendlyTo(player) ? "friendly" : chosen->IsHostileTo(player) ? "hostile" : "neutral";
                nlohmann::json request = {{"mode", "npc"}, {"npc_name", chosen->GetName()},
                                          {"npc_title", chosen->GetCreatureTemplate() ? chosen->GetCreatureTemplate()->SubName : std::string{}},
                                          {"zone", ZoneName(player->GetZoneId())}, {"area", ZoneName(player->GetAreaId())}, {"attitude", attitude},
                                          {"player_name", player->GetName()}, {"player_guid", player->GetGUID().GetRawValue()},
                                          {"player_faction", player->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde"}, {"message", SanitizeUTF8(message)}};
                const uint64_t playerGuid = player->GetGUID().GetRawValue();
                std::thread([creatureGuid, playerGuid, request]() {
                    struct SlotGuard { ~SlotGuard() { ReleaseGatewaySlot(); } } slot;
                    try
                    {
                        const auto asked = std::chrono::steady_clock::now();
                        const nlohmann::json answer = Ambient::MindPost("/ambient", request, Number("OllamaChat.Ambient.TimeoutSeconds", 15));
                        const std::string text = answer.is_object() ? answer.value("text", std::string{}) : std::string{};
                        if (text.empty() || text.size() > 250)
                            return;
                        const uint32_t spent = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - asked).count());
                        const uint32_t wait = TypingMs(text.size());
                        if (wait > spent)
                            std::this_thread::sleep_for(std::chrono::milliseconds(wait - spent));
                        OllamaChat::WorldTask::Run([creatureGuid, playerGuid, text]() -> nlohmann::json {
                            Player* listener = ObjectAccessor::FindPlayer(ObjectGuid(playerGuid));
                            Creature* speaker = listener && listener->IsInWorld() ? listener->GetMap()->GetCreature(ObjectGuid(creatureGuid)) : nullptr;
                            if (speaker && speaker->IsAlive())
                            {
                                speaker->SetFacingToObject(listener);
                                speaker->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
                                speaker->Say(text, LANG_UNIVERSAL);
                            }
                            return nlohmann::json{};
                        }, 3000);
                    }
                    catch (const std::exception& error)
                    {
                        LOG_ERROR("server.loading", "[Ollama Chat Roleplay] npc reply failed: {}", error.what());
                    }
                }).detach();
            }

            // ---- the world around a bot ----------------------------------------------------------------------------

            // ---- proximity -----------------------------------------------------------------------------------------

            // A bot that knows a whitelisted player says hello when they walk up to it. At most one greeting at a time per player, and a bot
            // greets the same player no more than once in ProximityGreetMinutes. The mind service decides who is a stranger (it says nothing).
            void Proximity(Player* operatorPlayer, time_t now)
            {
                if (!Number("OllamaChat.Roleplay.ProximityGreet", 1) || operatorPlayer->IsInCombat() || !operatorPlayer->IsAlive() || operatorPlayer->IsInFlight())
                    return;
                const uint64_t operatorGuid = operatorPlayer->GetGUID().GetRawValue();
                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    auto last = s_operatorGreetAt.find(operatorGuid);
                    if (last != s_operatorGreetAt.end() && now - last->second < static_cast<time_t>(Number("OllamaChat.Roleplay.ProximityGapSec", 25)))
                        return;
                }
                const float range = static_cast<float>(Number("OllamaChat.Roleplay.ProximityGreetYards", 9));
                const time_t again = static_cast<time_t>(Number("OllamaChat.Roleplay.ProximityGreetMinutes", 20)) * 60;
                Player* chosen = nullptr;
                float chosenDistance = range;
                for (auto const& entry : ObjectAccessor::GetPlayers())
                {
                    Player* bot = entry.second;
                    if (!bot || bot == operatorPlayer || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->GetMapId() != operatorPlayer->GetMapId())
                        continue;
                    const float distance = bot->GetDistance(operatorPlayer);
                    if (distance >= chosenDistance || !IsBot(bot) || !Ambient::SameSideCanTalk(bot, operatorPlayer))
                        continue;
                    const uint64_t botGuid = bot->GetGUID().GetRawValue();
                    if (IsGatewayBot(botGuid))
                        continue;
                    if (Group* group = bot->GetGroup())
                        if (group->IsMember(operatorPlayer->GetGUID()))
                            continue;   // a companion speaks through the party
                    {
                        std::lock_guard<std::mutex> lock(s_mutex);
                        auto seen = s_greetedAt.find(PairKey(botGuid, operatorGuid));
                        if (seen != s_greetedAt.end() && now - seen->second < again)
                            continue;
                    }
                    chosen = bot;
                    chosenDistance = distance;
                }
                if (!chosen || !TryAcquireGatewaySlot())
                    return;
                const uint64_t botGuid = chosen->GetGUID().GetRawValue();
                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    s_greetedAt[PairKey(botGuid, operatorGuid)] = now;
                    s_operatorGreetAt[operatorGuid] = now;
                    if (s_greetedAt.size() > 4000)
                        for (auto it = s_greetedAt.begin(); it != s_greetedAt.end();)
                            it = (now - it->second > again) ? s_greetedAt.erase(it) : std::next(it);
                }
                nlohmann::json request = {{"mode", "welcome"}, {"proximity", true}, {"bot_guid", botGuid}, {"bot_name", chosen->GetName()},
                                          {"player_guid", operatorGuid}, {"player_name", operatorPlayer->GetName()}, {"channel", "say"}};
                FillBrief(request, chosen);
                std::thread([botGuid, request]() {
                    struct SlotGuard { ~SlotGuard() { ReleaseGatewaySlot(); } } slot;
                    try
                    {
                        const nlohmann::json answer = Ambient::MindPost("/ambient", request, Number("OllamaChat.Ambient.TimeoutSeconds", 15));
                        const std::string text = answer.is_object() ? answer.value("text", std::string{}) : std::string{};
                        if (text.empty() || text.size() > 220)
                            return;
                        std::this_thread::sleep_for(std::chrono::milliseconds(TypingMs(text.size())));
                        OllamaChat::WorldTask::Run([botGuid, text]() -> nlohmann::json {
                            return nlohmann::json{{"ok", Ambient::SayInChannel(botGuid, "say", text)}};
                        }, 3000);
                    }
                    catch (const std::exception& error)
                    {
                        LOG_ERROR("server.loading", "[Ollama Chat Roleplay] greeting failed: {}", error.what());
                    }
                }).detach();
            }

            // A party with a whitelisted player in it, on a quiet road: now and then one of the bots remarks on the way.
            void CompanionIdle(Player* operatorPlayer, time_t now)
            {
                const uint32_t minutes = Number("OllamaChat.Roleplay.CompanionIdleMinutes", 6);
                Group* group = operatorPlayer->GetGroup();
                if (!minutes || !group || operatorPlayer->IsInCombat() || !operatorPlayer->IsAlive() || operatorPlayer->IsInFlight())
                    return;
                const uint64_t key = group->GetGUID().GetRawValue();
                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    auto last = s_groupIdleAt.find(key);
                    if (last == s_groupIdleAt.end())
                    {
                        s_groupIdleAt[key] = now;   // the first thought comes a while after the party forms
                        return;
                    }
                    if (now - last->second < static_cast<time_t>(minutes) * 60)
                        return;
                    s_groupIdleAt[key] = now;
                }
                std::vector<Player*> bots;
                for (auto const& slot : group->GetMemberSlots())
                {
                    Player* member = ObjectAccessor::FindPlayer(slot.guid);
                    if (member && member != operatorPlayer && member->IsInWorld() && member->IsAlive() && !member->IsInCombat() && IsBot(member))
                        bots.push_back(member);
                }
                if (bots.empty())
                    return;
                Companion(bots[urand(0, static_cast<uint32>(bots.size()) - 1)], "idle", "", 100, 0);
            }
        }   // namespace

        void Note(uint64_t botGuid, const std::string& kind, const std::string& text)
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto& ring = s_events[botGuid];
            ring.push_back({time(nullptr), kind, text});
            while (ring.size() > 10)
                ring.pop_front();
            if (s_events.size() > 6000)
                s_events.clear();   // a realm this busy has nobody to tell it to: start over rather than grow
        }

        // What happened to the whitelisted players lately, newest last. A bot hears of it second hand (Rumours), after a delay the mind service decides.
        struct HumanEvent
        {
            time_t      at;
            uint64_t    guid;
            std::string who;
            std::string kind;
            std::string text;
            uint32      zoneId;
            uint32      guildId;
        };
        std::deque<HumanEvent> s_humanEvents;       // guarded by s_mutex
        constexpr time_t kRumourLifeSec = 6 * 3600;

        void NoteHuman(Player* human, const char* kind, const std::string& text)
        {
            // Only a death or a level is news, and only about a whitelisted player.
            if (!human || (std::string(kind) != "death" && std::string(kind) != "levelup") || !OllamaChat::Promotion::IsOperator(human))
                return;
            const time_t now = time(nullptr);
            std::lock_guard<std::mutex> lock(s_mutex);
            s_humanEvents.push_back({now, human->GetGUID().GetRawValue(), human->GetName(), kind, text, human->GetZoneId(), human->GetGuildId()});
            while (!s_humanEvents.empty() && (s_humanEvents.size() > 20 || now - s_humanEvents.front().at > kRumourLifeSec))
                s_humanEvents.pop_front();
        }

        nlohmann::json Rumours(Player* bot)
        {
            nlohmann::json out = nlohmann::json::array();
            if (!bot)
                return out;
            const time_t now = time(nullptr);
            std::lock_guard<std::mutex> lock(s_mutex);
            for (auto event = s_humanEvents.rbegin(); event != s_humanEvents.rend() && out.size() < 3; ++event)
            {
                if (event->guid == bot->GetGUID().GetRawValue() || now - event->at > kRumourLifeSec)
                    continue;
                const int near = event->guildId && event->guildId == bot->GetGuildId() ? 2 : event->zoneId == bot->GetZoneId() ? 1 : 0;
                out.push_back({{"who", event->who}, {"k", event->kind}, {"t", event->text}, {"zone", ZoneName(event->zoneId)},
                               {"ago", static_cast<int64_t>(now - event->at)}, {"near", near}, {"id", static_cast<int64_t>(event->at)}});
            }
            return out;
        }

        nlohmann::json RecentEvents(uint64_t botGuid)
        {
            nlohmann::json out = nlohmann::json::array();
            const time_t now = time(nullptr);
            std::lock_guard<std::mutex> lock(s_mutex);
            auto it = s_events.find(botGuid);
            if (it == s_events.end())
                return out;
            for (auto event = it->second.rbegin(); event != it->second.rend() && out.size() < 8; ++event)
                out.push_back({{"k", event->kind}, {"t", event->text}, {"ago", static_cast<int64_t>(now - event->at)}});
            return out;
        }

        void AddWorldColour(nlohmann::json& brief, Player* bot)
        {
            if (!bot || !bot->IsInWorld())
                return;
            const time_t now = time(nullptr);
            tm local{};
            localtime_r(&now, &local);
            const int hour = local.tm_hour;
            brief["time"] = hour >= 5 && hour < 7 ? "dawn" : hour >= 7 && hour < 11 ? "morning" : hour >= 11 && hour < 14 ? "midday"
                          : hour >= 14 && hour < 18 ? "afternoon" : hour >= 18 && hour < 20 ? "dusk" : "night";
            nlohmann::json holidays = nlohmann::json::array();
            auto const& all = sGameEventMgr->GetEventMap();
            for (uint16 id : sGameEventMgr->GetActiveEventList())
                if (id < all.size() && all[id].HolidayId != HOLIDAY_NONE && !all[id].Description.empty() && holidays.size() < 3)
                    holidays.push_back(all[id].Description);
            if (!holidays.empty())
                brief["holidays"] = holidays;
        }

        void Tick(uint32_t diff)
        {
            s_accumMs += diff;
            if (s_accumMs < 3000)
                return;
            s_accumMs = 0;
            if (!Active())
                return;
            const time_t now = time(nullptr);
            for (auto const& entry : ObjectAccessor::GetPlayers())
            {
                Player* player = entry.second;
                if (!player || !player->IsInWorld() || IsBot(player) || !OllamaChat::Promotion::IsOperator(player))
                    continue;
                Proximity(player, now);
                CompanionIdle(player, now);
            }
        }
    }
}

RoleplayScript::RoleplayScript() : PlayerScript("RoleplayScript") {}

void RoleplayScript::OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/)
{
    // No number in the line: the people of the world do not count their years of adventuring.
    OllamaChat::Roleplay::Observe(player, "levelup", "came into new strength after a hard stretch of road", "levelup", "", 100, 1500);
}

void RoleplayScript::OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/)
{
    using namespace OllamaChat::Roleplay;
    if (!player || !IsBot(player))
        return;
    const std::string zone = ZoneName(newZone);
    if (zone.empty())
        return;
    const time_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto& last = s_lastZone[player->GetGUID().GetRawValue()];
        if (last.first == zone && now - last.second < 1800)
            return;   // back and forth over a border is not a journey
        last = {zone, now};
    }
    Observe(player, "zone", "arrived in " + zone, "zone", zone, 40, 2500);
}

void RoleplayScript::OnPlayerCompleteQuest(Player* player, Quest const* quest)
{
    if (!quest)
        return;
    OllamaChat::Roleplay::Observe(player, "quest", "finished an errand: " + quest->GetTitle(), "quest", quest->GetTitle(), 50, 2500);
}

void RoleplayScript::OnPlayerKilledByCreature(Creature* killer, Player* killed)
{
    OllamaChat::Roleplay::Observe(killed, "death", std::string("was struck down by ") + (killer ? killer->GetName() : "something unseen") + " and had to be raised", "death", "", 60, 25000);
}

void RoleplayScript::OnPlayerCreatureKill(Player* killer, Creature* killed)
{
    using namespace OllamaChat::Roleplay;
    if (!killer || !killed || !IsBot(killer))
        return;
    const CreatureTemplate* tpl = killed->GetCreatureTemplate();
    if (killed->IsDungeonBoss())
        Observe(killer, "boss", "helped bring down " + killed->GetName() + ", a great foe", "boss", killed->GetName(), 100, 2500);
    else if (tpl && (tpl->rank == CREATURE_ELITE_RARE || tpl->rank == CREATURE_ELITE_RAREELITE || tpl->rank == CREATURE_ELITE_WORLDBOSS))
        Observe(killer, "kill", "helped slay the notorious " + killed->GetName(), "boss", killed->GetName(), 70, 2500);
}

void RoleplayScript::OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid /*lootGuid*/)
{
    using namespace OllamaChat::Roleplay;
    if (!player || !item || !item->GetTemplate() || item->GetTemplate()->Quality < ITEM_QUALITY_RARE || !IsBot(player))
        return;
    Observe(player, "loot", "came by something fine: " + item->GetTemplate()->Name1, "rare", item->GetTemplate()->Name1, 50, 2000);
}

void RoleplayScript::OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 /*emoteNum*/, ObjectGuid guid)
{
    using namespace OllamaChat::Roleplay;
    if (!Active() || !Number("OllamaChat.Roleplay.EmoteReplies", 1) || !player || IsBot(player) || !OllamaChat::Promotion::IsOperator(player))
        return;
    EmoteAnswer answer{};
    if (!AnswerFor(textEmote, answer))
        return;
    Player* target = guid.IsPlayer() ? ObjectAccessor::FindPlayer(guid) : nullptr;
    if (target && !IsBot(target))
        target = nullptr;
    if (!target)
    {
        if (urand(0, 99) >= Number("OllamaChat.Roleplay.EmoteNearbyPercent", 50))
            return;   // aimed at nobody: someone nearby notices now and then
        target = NearestBot(player, 7.0f);
    }
    if (!target || !OllamaChat::Ambient::SameSideCanTalk(target, player) || IsGatewayBot(target->GetGUID().GetRawValue()))
        return;
    const uint64_t botGuid = target->GetGUID().GetRawValue();
    const time_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto last = s_emoteAt.find(botGuid);
        if (last != s_emoteAt.end() && now - last->second < 20)
            return;
        s_emoteAt[botGuid] = now;
    }
    const bool speak = urand(0, 99) < Number("OllamaChat.Roleplay.EmoteSpeakPercent", 60) && TryAcquireGatewaySlot();
    nlohmann::json request = {{"mode", "emote"}, {"emote", answer.token}, {"player_name", player->GetName()}, {"bot_guid", botGuid}, {"bot_name", target->GetName()}};
    FillBrief(request, target);
    const uint64_t playerGuid = player->GetGUID().GetRawValue();
    const uint32 back = answer.back;
    std::thread([botGuid, playerGuid, back, speak, request]() {
        struct SlotGuard
        {
            bool held;
            ~SlotGuard() { if (held) ReleaseGatewaySlot(); }
        } slot{speak};
        try
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(urand(700, 1800)));
            OllamaChat::WorldTask::Run([botGuid, playerGuid, back]() -> nlohmann::json {
                Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
                Player* other = ObjectAccessor::FindPlayer(ObjectGuid(playerGuid));
                if (bot && bot->IsInWorld() && bot->IsAlive() && !bot->IsInCombat())
                {
                    if (other)
                        bot->SetFacingToObject(other);
                    bot->HandleEmoteCommand(back);
                }
                return nlohmann::json{};
            }, 3000);
            if (!speak)
                return;
            const nlohmann::json reply = OllamaChat::Ambient::MindPost("/ambient", request, Number("OllamaChat.Ambient.TimeoutSeconds", 15));
            const std::string text = reply.is_object() ? reply.value("text", std::string{}) : std::string{};
            if (text.empty() || text.size() > 220)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(TypingMs(text.size())));
            OllamaChat::WorldTask::Run([botGuid, text]() -> nlohmann::json {
                return nlohmann::json{{"ok", OllamaChat::Ambient::SayInChannel(botGuid, "say", text)}};
            }, 3000);
        }
        catch (const std::exception& error)
        {
            LOG_ERROR("server.loading", "[Ollama Chat Roleplay] emote answer failed: {}", error.what());
        }
    }).detach();
}

void RoleplayScript::OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& /*lang*/, std::string& msg)
{
    if (type == CHAT_MSG_SAY && !msg.empty())
        OllamaChat::Roleplay::NpcReply(player, msg);
}
