#include "mod-ollama-chat_ambient.h"

#include "mod-ollama-chat-utilities.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_gateway.h"
#include "mod-ollama-chat_httpclient.h"
#include "mod-ollama-chat_promotion.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_tools.h"
#include "mod-ollama-chat_worldtask.h"

#include "Bag.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "QueryResult.h"
#include "Random.h"
#include "World.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <functional>
#include <cctype>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace OllamaChat
{
    namespace Ambient
    {
        namespace
        {
            struct LastLine
            {
                uint64_t hash  = 0;
                time_t   at    = 0;
                uint32_t depth = 0;   // 1 = answered a player, 2 = answered that answer, ...
            };

            struct Scene
            {
                time_t   expires = 0;
                uint32_t lines   = 0;
            };

            std::mutex s_mutex;
            std::unordered_map<uint64_t, LastLine> s_lastLine;                        // bot guid -> its latest ambient line
            std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_nextLineAt;   // scene key -> when its next line may go out
            std::unordered_map<uint64_t, time_t>   s_lastReplyAt;                     // bot guid -> when it last answered
            std::unordered_map<uint64_t, Scene>    s_scenes;                          // area -> the conversation going on there
            std::unordered_map<uint64_t, std::pair<time_t, uint32_t>> s_batch;        // heard line -> (when, replies so far)

            struct Partner
            {
                uint64_t botGuid = 0;
                time_t   at      = 0;
            };
            std::unordered_map<std::string, Partner> s_partners;                      // "player|channel kind" -> who answered them
            std::unordered_map<uint64_t, Listing>    s_listings;                      // bot guid -> what it is offering in Trade

            constexpr time_t kLineLifetimeSec = 20;   // an answer can be answered for this long

            bool     Enabled()     { return sConfigMgr->GetOption<bool>("OllamaChat.Ambient.Enable", true); }
            // Every caller passes the documented default, so a key missing from an older conf is not an error. The core logs
            // "Missing property" on each lookup, and a few of these run on every world tick.
            uint32_t Number(const char* key, uint32_t fallback) { return sConfigMgr->GetOption<uint32_t>(key, fallback, false); }

            // ---- how the bots talk: as characters in the lore, or as players ----------------------------------------------
            // The mind service owns the switch (a toggle on its dashboard; roleplay by default) and the module polls it. Until the
            // first answer, and whenever the service cannot be reached, roleplay is assumed: the safe side is a bot that keeps
            // quiet in the realm channel. OllamaChat.Roleplay.Mode forces one or the other without asking.
            std::mutex  s_modeMutex;
            bool        s_roleplay = true;
            bool        s_channelsKnown = false;
            std::string s_roleplayChannels;
            std::atomic<uint64_t> s_droppedStock{0};   // stock lines that were not said because a character would not say them there

            std::string ConfString(const char* key, const char* fallback)
            {
                return sConfigMgr->GetOption<std::string>(key, std::string(fallback), false);
            }

            bool RoleplayOn()
            {
                const std::string forced = ConfString("OllamaChat.Roleplay.Mode", "auto");
                if (forced == "roleplay")
                    return true;
                if (forced == "players")
                    return false;
                std::lock_guard<std::mutex> lock(s_modeMutex);
                return s_roleplay;
            }

            // The channels a roleplaying bot talks in by itself: the mind's list once it has answered, else the conf's.
            std::string RoleplayChannels()
            {
                if (ConfString("OllamaChat.Roleplay.Mode", "auto") != "auto")
                    return ConfString("OllamaChat.Roleplay.Channels", "say,yell,guild");
                std::lock_guard<std::mutex> lock(s_modeMutex);
                return s_channelsKnown ? s_roleplayChannels : ConfString("OllamaChat.Roleplay.Channels", "say,yell,guild");
            }

            // {"ok": true, "chat_mode": "roleplay"|"players", "channels": ["say", ...]} from the mind service (any cast answer carries
            // the mode; only the "mode" op carries the channels).
            void ApplyMode(const nlohmann::json& answer)
            {
                if (!answer.is_object())
                    return;
                const std::string mode = answer.value("chat_mode", std::string{});
                if (mode != "roleplay" && mode != "players")
                    return;
                std::string channels;
                const bool listed = answer.contains("channels") && answer["channels"].is_array();
                if (listed)
                    for (auto const& channel : answer["channels"])
                        if (channel.is_string())
                            channels += (channels.empty() ? "" : ",") + channel.get<std::string>();
                std::lock_guard<std::mutex> lock(s_modeMutex);
                if (s_roleplay != (mode == "roleplay"))
                    LOG_INFO("server.loading", "[Ollama Chat Roleplay] the bots now talk {}", mode == "roleplay" ? "in character" : "as players");
                s_roleplay = mode == "roleplay";
                if (listed)
                {
                    s_roleplayChannels = channels;
                    s_channelsKnown = true;
                }
            }

            // What a character needs to know about where it is and what it is doing, besides its race and class. The brief is
            // read on the world thread (DescribeBotBrief); the request goes out from a worker.
            void AddRoleplayFields(nlohmann::json& request, const nlohmann::json& brief)
            {
                request["gender"] = brief.value("gender", std::string{});
                request["doing"] = brief.value("doing", std::string{});
                for (const char* key : {"quests", "events", "time", "weather", "holidays"})
                    if (brief.contains(key))
                        request[key] = brief[key];
            }

            void AddBrief(nlohmann::json& request, const nlohmann::json& brief)
            {
                request["level"] = brief.value("level", 0);
                for (const char* key : {"class", "race", "zone", "area"})
                    request[key] = brief.value(key, std::string{});
                AddRoleplayFields(request, brief);
            }

            // How long a person takes to type `chars` characters; the time already spent getting the words is taken off.
            uint32_t TypingDelayMs(size_t chars, std::chrono::steady_clock::time_point since)
            {
                // A character speaks, it does not type: roleplay has its own, quicker figures.
                const bool speech = RoleplayOn();
                const uint64_t want = speech
                    ? std::min<uint64_t>(Number("OllamaChat.Roleplay.TypingMaxMs", 3000),
                                         Number("OllamaChat.Roleplay.TypingBaseMs", 300) + chars * Number("OllamaChat.Roleplay.TypingMsPerChar", 22))
                    : std::min<uint64_t>(Number("OllamaChat.Ambient.TypingMaxMs", 4500),
                                         Number("OllamaChat.Ambient.TypingBaseMs", 500) + chars * Number("OllamaChat.Ambient.TypingMsPerChar", 40));
                const uint64_t spent = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count());
                return want > spent ? static_cast<uint32_t>(want - spent) : 0;
            }

            // TypingDelayMs, but never sooner than LineGapMs after the last line said in the same place. The mind answers one bot
            // at a time, yet the wait for its turn counts as typing, so without this two answers can land in the same second.
            uint32_t SendDelayMs(size_t chars, std::chrono::steady_clock::time_point since, uint64_t sceneKey)
            {
                const auto now = std::chrono::steady_clock::now();
                const auto typed = now + std::chrono::milliseconds(TypingDelayMs(chars, since));
                std::lock_guard<std::mutex> lock(s_mutex);
                auto& nextFree = s_nextLineAt[sceneKey];   // a new scene starts at the epoch, which is always in the past
                const auto sendAt = std::max(typed, nextFree);
                nextFree = sendAt + std::chrono::milliseconds(Number("OllamaChat.Ambient.LineGapMs", 2000));
                return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(sendAt - now).count());
            }

            uint64_t HashOf(const std::string& text) { return static_cast<uint64_t>(std::hash<std::string>{}(text)); }

            uint64_t AreaKey(Player* p)
            {
                return (static_cast<uint64_t>(p->GetMapId()) << 32) | static_cast<uint64_t>(p->GetZoneId());
            }

            // Where a conversation is going on. Say, yell and zone talk stay in one zone; the realm, Trade and LFG channels are
            // heard everywhere (with AllowTwoSide.Interaction.Channel = 1, by both factions), so they share one scene.
            uint64_t SceneKey(Player* p, const std::string& kind)
            {
                if (kind == "guild")
                    return 0xFFFFFFFD00000000ull | p->GetGuildId();   // one conversation per guild
                if (kind == "say" || kind == "yell" || kind == "zone")
                    return AreaKey(p);
                const uint64_t channel = kind == "world" ? 1 : kind == "trade" ? 2 : 3;
                return 0xFFFFFFFF00000000ull | channel;
            }

            // Which of the bot's channels this line arrived in: "say", "yell", "zone", "trade", "lfg", "world", or "" to stay out.
            std::string KindOf(ChatChannelSourceLocal source, Channel* channel)
            {
                if (source == SRC_SAY_LOCAL)  return "say";
                if (source == SRC_YELL_LOCAL) return "yell";
                if (source == SRC_GUILD_LOCAL) return "guild";
                if (!channel) return "zone";
                std::string name = channel->GetName();
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
                if (name.find("defense") != std::string::npos || name.find("recruit") != std::string::npos) return "";
                if (name.find("trade") != std::string::npos)         return "trade";
                if (name.find("lookingforgroup") != std::string::npos || name.find("looking for group") != std::string::npos) return "lfg";
                const std::string realm = sConfigMgr->GetOption<std::string>("AiPlayerbot.BroadcastWorldChannelName", "");
                if (!realm.empty())
                {
                    std::string lowRealm = realm;
                    std::transform(lowRealm.begin(), lowRealm.end(), lowRealm.begin(), [](unsigned char c) { return std::tolower(c); });
                    if (name.find(lowRealm) != std::string::npos) return "world";
                }
                return "zone";
            }

            // The conf's list is the operator's hard limit; in roleplay the mind's list narrows it further (a character does not
            // call out in the realm channel, or advertise in Trade).
            bool KindAllowed(const std::string& kind)
            {
                std::string allowed = sConfigMgr->GetOption<std::string>("OllamaChat.Ambient.Channels", "say,yell,zone,trade,lfg,world,guild");
                allowed = "," + allowed + ",";
                if (allowed.find("," + kind + ",") == std::string::npos)
                    return false;
                if (!RoleplayOn())
                    return true;
                return ("," + RoleplayChannels() + ",").find("," + kind + ",") != std::string::npos;
            }

            // "http://host:port/v1/chat/completions" -> "http://host:port/ambient" (or "/cast", ...).
            std::string MindUrl(const std::string& route)
            {
                std::string url = sConfigMgr->GetOption<std::string>("OllamaChat.Ambient.Url", "");
                if (url.empty())
                {
                    url = g_GatewayUrl;
                    const size_t scheme = url.find("://");
                    const size_t path = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
                    url = (path == std::string::npos ? url : url.substr(0, path)) + "/ambient";
                }
                const size_t slash = url.rfind('/');
                return (slash == std::string::npos ? url : url.substr(0, slash)) + route;
            }

            // The mind service's JSON answer to `request` at `route`, or null. Runs on a worker thread.
            nlohmann::json PostMind(const std::string& route, const nlohmann::json& request, uint32_t timeoutSec)
            {
                OllamaHttpClient client;
                client.SetTimeout(static_cast<int>(timeoutSec));
                if (!client.IsAvailable())
                    return nullptr;
                std::vector<std::pair<std::string, std::string>> headers;
                if (!g_GatewayBearerToken.empty())
                    headers.emplace_back("Authorization", "Bearer " + g_GatewayBearerToken);
                const std::string raw = client.Post(MindUrl(route), request.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), headers);
                if (raw.empty())
                    return nullptr;
                try
                {
                    return nlohmann::json::parse(raw);
                }
                catch (const std::exception& error)
                {
                    LOG_ERROR("server.loading", "[Ollama Chat Ambient] unreadable answer from {}: {}", route, error.what());
                    return nullptr;
                }
            }

            nlohmann::json AskMindJson(const nlohmann::json& request)
            {
                return PostMind("/ambient", request, Number("OllamaChat.Ambient.TimeoutSeconds", 15));
            }

            // The line the bot should speak, or "" when it has nothing to add. Runs on a worker thread.
            std::string AskMind(const nlohmann::json& request)
            {
                const nlohmann::json answer = AskMindJson(request);
                return answer.is_object() ? answer.value("text", std::string{}) : std::string{};
            }

            // ---- the regulars ----------------------------------------------------------------------------------
            // The mind service keeps a small cast of bots that are always the ones talking, with friendships between them;
            // this is our copy of who they are, refreshed now and then.
            struct CastFriend
            {
                uint64_t    guid = 0;
                std::string name;
            };
            std::unordered_set<uint64_t>                          s_cast;
            std::unordered_map<uint64_t, std::vector<CastFriend>> s_castFriends;

            bool IsCast(uint64_t guid)
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                return s_cast.count(guid) > 0;
            }

            std::vector<CastFriend> CastFriendsOf(uint64_t guid)
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                auto it = s_castFriends.find(guid);
                return it == s_castFriends.end() ? std::vector<CastFriend>{} : it->second;
            }

            bool AreFriends(uint64_t a, uint64_t b)
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                auto it = s_castFriends.find(a);
                if (it == s_castFriends.end())
                    return false;
                for (auto const& friendOfA : it->second)
                    if (friendOfA.guid == b)
                        return true;
                return false;
            }

            void ApplyCast(const nlohmann::json& answer)
            {
                ApplyMode(answer);
                if (!answer.is_object() || !answer.contains("cast") || !answer["cast"].is_array())
                    return;
                std::unordered_set<uint64_t> members;
                std::unordered_map<uint64_t, std::vector<CastFriend>> friends;
                for (auto const& member : answer["cast"])
                {
                    const uint64_t guid = member.value("guid", static_cast<uint64_t>(0));
                    if (!guid)
                        continue;
                    members.insert(guid);
                    if (member.contains("friends") && member["friends"].is_array())
                        for (auto const& buddy : member["friends"])
                            if (buddy.value("guid", static_cast<uint64_t>(0)))
                                friends[guid].push_back({buddy.value("guid", static_cast<uint64_t>(0)), buddy.value("name", std::string{})});
                }
                std::lock_guard<std::mutex> lock(s_mutex);
                s_cast = std::move(members);
                s_castFriends = std::move(friends);
            }

            // Asking a mind service for a cast is slow, so it never happens on the world thread.
            void PostCastAsync(nlohmann::json request)
            {
                std::thread([request]() {
                    try
                    {
                        ApplyCast(PostMind("/cast", request, 30));
                    }
                    catch (const std::exception& error)
                    {
                        LOG_ERROR("server.loading", "[Ollama Chat Community] cast request failed: {}", error.what());
                    }
                }).detach();
            }

            // A request that has to do something (invite, follow, trade, give, look in the bags) needs the ordinary path with
            // tools; everything else that is said to a bot is conversation, and the fast path answers that.
            bool NeedsTools(const std::string& msg)
            {
                static const std::unordered_set<std::string> actionWords = {
                    "invite", "inv", "invites", "follow", "stay", "come", "trade", "give", "sell", "buy", "equip", "gear", "bag", "bags",
                    "inventory", "gold", "money", "silver", "copper", "mail", "quest", "quests", "heal", "tank", "group", "party",
                    "guild", "summon", "attack", "kill", "stop", "loot", "craft", "repair", "cast", "learn", "train", "teleport",
                    "portal", "mount", "talents", "talent", "spec", "stats", "skills", "professions", "wearing", "equipped", "armor",
                    "weapon", "weapons", "logout", "leave", "join", "accept", "decline", "drop", "destroy", "use", "open", "pull",
                    "flee", "revive", "resurrect", "rez", "release", "formation", "passive", "active", "strategy"};
                std::string word;
                auto hit = [&]() { const bool found = actionWords.count(word) > 0; word.clear(); return found; };
                for (unsigned char c : msg)
                {
                    if (std::isalnum(c))
                        word += static_cast<char>(std::tolower(c));
                    else if (!word.empty() && hit())
                        return true;
                }
                return !word.empty() && hit();
            }

            // ---- playerbots' stock lines ---------------------------------------------------------------------
            // ai_playerbot_texts holds templates such as "Took %quest_link. Time to dive in and get it done." A line a bot is
            // about to say that fits one of them (its fixed pieces in order, %placeholders standing for anything) is stock.
            struct Canned
            {
                std::string              category;
                std::vector<std::string> parts;          // the fixed pieces, in order
                bool                     startsFixed = false;
                bool                     endsFixed   = false;
            };
            std::vector<Canned>                                   s_canned;
            std::unordered_map<std::string, std::vector<uint32_t>> s_cannedByStart;   // first 5 bytes -> templates
            std::vector<uint32_t>                                  s_cannedLoose;      // start with a placeholder or a short piece
            bool                                                   s_cannedLoaded = false;

            void LoadCanned()
            {
                s_cannedLoaded = true;
                QueryResult result = PlayerbotsDatabase.Query("SELECT name, text FROM ai_playerbot_texts WHERE name LIKE 'broadcast%' OR name LIKE 'suggest%'");
                if (!result)
                {
                    LOG_WARN("server.loading", "[Ollama Chat Ambient] no ai_playerbot_texts rows: stock lines will not be recognised");
                    return;
                }
                do
                {
                    Field* fields = result->Fetch();
                    Canned entry;
                    entry.category = fields[0].Get<std::string>();
                    const std::string text = fields[1].Get<std::string>();
                    std::string piece;
                    for (size_t i = 0; i < text.size();)
                    {
                        if (text[i] == '%' && i + 1 < text.size() && (std::islower(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '_'))
                        {
                            size_t j = i + 1;
                            while (j < text.size() && (std::islower(static_cast<unsigned char>(text[j])) || text[j] == '_')) ++j;
                            if (!piece.empty()) entry.parts.push_back(piece);
                            piece.clear();
                            i = j;
                        }
                        else
                            piece += text[i++];
                    }
                    entry.endsFixed = !piece.empty();   // the template ends with fixed text, not with a placeholder
                    if (!piece.empty()) entry.parts.push_back(piece);
                    size_t fixed = 0;
                    for (const std::string& p : entry.parts) fixed += p.size();
                    if (fixed < 8)
                        continue;   // too little to recognise it by
                    entry.startsFixed = !text.empty() && text[0] != '%';
                    const uint32_t index = static_cast<uint32_t>(s_canned.size());
                    if (entry.startsFixed && entry.parts[0].size() >= 5)
                        s_cannedByStart[entry.parts[0].substr(0, 5)].push_back(index);
                    else
                        s_cannedLoose.push_back(index);
                    s_canned.push_back(std::move(entry));
                } while (result->NextRow());
                LOG_INFO("server.loading", "[Ollama Chat Ambient] {} stock chat templates indexed", s_canned.size());
            }

            bool Fits(const Canned& entry, const std::string& msg)
            {
                size_t at = 0;
                for (size_t i = 0; i < entry.parts.size(); ++i)
                {
                    const std::string& part = entry.parts[i];
                    const size_t found = msg.find(part, at);
                    if (found == std::string::npos)
                        return false;
                    if (i == 0 && entry.startsFixed && found != 0)
                        return false;
                    at = found + part.size();
                }
                return !entry.endsFixed || at == msg.size();
            }

            // The template category of a stock line ("broadcast_quest_accepted_generic"), or "" for anything else.
            std::string CannedCategory(const std::string& msg)
            {
                if (!s_cannedLoaded)
                    LoadCanned();
                if (msg.size() >= 5)
                {
                    auto it = s_cannedByStart.find(msg.substr(0, 5));
                    if (it != s_cannedByStart.end())
                        for (uint32_t index : it->second)
                            if (Fits(s_canned[index], msg))
                                return s_canned[index].category;
                }
                for (uint32_t index : s_cannedLoose)
                    if (Fits(s_canned[index], msg))
                        return s_canned[index].category;
                return "";
            }

            // A whitelisted player on the bot's map within `range` yards: someone who would read the line.
            bool OperatorNear(Player* bot, float range)
            {
                for (auto const& entry : ObjectAccessor::GetPlayers())
                {
                    Player* other = entry.second;
                    if (!other || other == bot || !other->IsInWorld() || other->GetMapId() != bot->GetMapId())
                        continue;
                    PlayerbotAI* otherAi = PlayerbotsMgr::instance().GetPlayerbotAI(other);
                    if (otherAi && otherAi->IsBotAI())
                        continue;
                    if (OllamaChat::Promotion::IsOperator(other) && bot->GetDistance(other) <= range)
                        return true;
                }
                return false;
            }

            // The Trade channel this player is in, or null. It is city-only and each faction has its own ("Trade - City" on the
            // server), so someone outside a capital is not in it and cannot speak or hear there. Playerbots' SayToChannel looks for
            // a channel whose name carries the zone's name, which this one never does: a bot's Trade line went nowhere.
            Channel* TradeChannelOf(Player* player)
            {
                ChannelMgr* channels = player ? ChannelMgr::forTeam(player->GetTeamId()) : nullptr;
                if (!channels)
                    return nullptr;
                for (auto const& entry : channels->GetChannels())
                    if (entry.second && entry.second->GetChannelId() == static_cast<uint32>(ChatChannelId::TRADE) && player->IsInChannel(entry.second))
                        return entry.second;
                return nullptr;
            }

            // World thread: put the line in the bot's mouth where it was asked.
            bool Speak(uint64_t botGuid, const std::string& kind, const std::string& text)
            {
                Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
                if (!bot || !bot->IsInWorld() || !bot->IsAlive())
                    return false;
                PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
                if (!ai)
                    return false;
                if (bot->isAFK() && !bot->InBattleground())
                    bot->ToggleAFK();   // a bot that idles far from anyone is flagged <Away>; one that is talking is not
                if (kind == "guild") return ai->SayToGuild(text);
                if (kind == "say")   return ai->Say(text);
                if (kind == "yell")  return ai->Yell(text);
                if (kind == "world") return ai->SayToWorld(text);
                if (kind == "trade")
                {
                    Channel* trade = TradeChannelOf(bot);
                    if (!trade)
                        return false;
                    trade->Say(bot->GetGUID(), text, LANG_UNIVERSAL);
                    return true;
                }
                if (kind == "lfg")   return ai->SayToChannel(text, ChatChannelId::LOOKING_FOR_GROUP);
                return ai->SayToChannel(text, ChatChannelId::GENERAL);
            }

            void ChainFrom(uint64_t speakerGuid, const std::string& kind, const std::string& text);
            bool HandleIn(Player* bot, Player* speaker, const std::string& msg, const std::string& kind, const std::string& channelName,
                          bool speakerIsBot, bool addressed = false);
            void RememberPartnerIn(uint64_t speakerGuid, const std::string& kind, uint64_t botGuid, bool force);

            // Playerbots' channel lines (SayToChannel / SayToWorld) never reach the chat hooks, so the bots that would have
            // heard one are not told. After such a line is said, offer it to a few of them here. /say and /yell do reach the hooks.
            // `answererGuid`: a friend the line was spoken to by name, who answers it whatever the dice say.
            bool SpeakAndChain(uint64_t botGuid, const std::string& kind, const std::string& text, uint64_t answererGuid = 0)
            {
                const bool said = Speak(botGuid, kind, text);
                if (said && answererGuid)
                {
                    Player* speaker = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
                    Player* buddy = ObjectAccessor::FindPlayer(ObjectGuid(answererGuid));
                    if (speaker && buddy)
                        HandleIn(buddy, speaker, text, kind, std::string{}, true, true);
                }
                if (said && kind != "say" && kind != "yell")
                    ChainFrom(botGuid, kind, text);
                return said;
            }
        }

        uint32_t ChainChancePercent()
        {
            return Number("OllamaChat.Ambient.ChainChance", 60);
        }

        bool RoleplayActive()
        {
            return RoleplayOn();
        }

        nlohmann::json MindPost(const std::string& route, const nlohmann::json& request, uint32_t timeoutSec)
        {
            return PostMind(route, request, timeoutSec);
        }

        bool SameSideCanTalk(Player* a, Player* b)
        {
            return !a || !b || a->GetTeamId() == b->GetTeamId() || sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHAT);
        }

        namespace
        {
            bool Rewrite(Player* speaker, const std::string& kind, const std::string& channelName, const std::string& msg);
        }

        bool MaybeRewrite(Player* speaker, uint32_t chatType, const std::string& msg, Channel* channel)
        {
            if (chatType != CHAT_MSG_SAY && chatType != CHAT_MSG_YELL && chatType != CHAT_MSG_CHANNEL)
                return false;
            const ChatChannelSourceLocal source = chatType == CHAT_MSG_SAY ? SRC_SAY_LOCAL : chatType == CHAT_MSG_YELL ? SRC_YELL_LOCAL : SRC_GENERAL_LOCAL;
            return Rewrite(speaker, KindOf(source, channel), channel ? channel->GetName() : std::string{}, msg);
        }

        // Playerbots' channel lines (SayToChannel / SayToWorld) never reach the chat hooks, so a small filter in playerbots
        // (patches/mod-playerbots-outgoing-channel-filter.patch) hands them here: chan is a ChatChannelId, or -1 for the realm channel.
        bool MaybeRewriteChannel(Player* speaker, int chan, const std::string& msg)
        {
            std::string kind;
            switch (chan)
            {
                case -1:                 kind = "world"; break;
                case GENERAL:            kind = "zone";  break;
                case TRADE:              kind = "trade"; break;
                case LOOKING_FOR_GROUP:  kind = "lfg";   break;
                default:                 return Rewrite(speaker, std::string{}, std::string{}, msg);   // local and world defence, guild recruitment: left alone, or dropped in roleplay
            }
            return Rewrite(speaker, kind, std::string{}, msg);
        }

        namespace
        {
        // Roleplay: a stock playerbots line (a loot brag in the realm channel, an LFG advert, a Trade offer) in a channel a
        // character would not speak in is not said at all. True means the line is swallowed. Anything that is not stock chatter
        // (a line a player asked a bot for, the module's own lines) is never touched, and players mode never drops anything.
        bool DropStockLine(Player* speaker, const std::string& msg)
        {
            if (!RoleplayOn() || !Number("OllamaChat.Roleplay.DropStockLines", 1))
                return false;
            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(speaker);
            if (!ai || !ai->IsBotAI() || IsGatewayBot(speaker->GetGUID().GetRawValue()))
                return false;
            if (CannedCategory(msg).empty())
                return false;
            ++s_droppedStock;
            return true;
        }

        bool Rewrite(Player* speaker, const std::string& kind, const std::string& channelName, const std::string& msg)
        {
            if (!Enabled() || !sConfigMgr->GetOption<bool>("OllamaChat.Ambient.RewriteStockLines", true) || !g_GatewayEnable)
                return false;
            if (!speaker || msg.empty())
                return false;
            if (kind.empty() || !KindAllowed(kind))
                return DropStockLine(speaker, msg);
            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(speaker);
            if (!ai || !ai->IsBotAI())
                return false;
            const uint64_t botGuid = speaker->GetGUID().GetRawValue();
            if (IsGatewayBot(botGuid) || !speaker->IsAlive())
                return false;
            const time_t now = time(nullptr);
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                auto own = s_lastLine.find(botGuid);
                if (own != s_lastLine.end() && own->second.hash == HashOf(msg) && now - own->second.at <= kLineLifetimeSec)
                    return false;   // a line this module just put in the bot's mouth (or the stock line it fell back to)
            }
            const std::string category = CannedCategory(msg);
            if (category.empty())
                return false;
            if (!OperatorNear(speaker, static_cast<float>(Number("OllamaChat.Ambient.RewriteRangeYards", 250))))
                return false;   // nobody who matters can read it: stay exactly as playerbots made it
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                auto last = s_lastReplyAt.find(botGuid);
                if (last != s_lastReplyAt.end() && now - last->second < static_cast<time_t>(Number("OllamaChat.Ambient.BotCooldownSec", 20)))
                    return false;   // it spoke a moment ago: let the stock line through rather than swallow it
                s_lastReplyAt[botGuid] = now;
            }
            if (!TryAcquireGatewaySlot())
                return false;

            nlohmann::json brief = DescribeBotBrief(speaker);
            nlohmann::json request = {
                {"mode", "rewrite"},
                {"bot_guid", botGuid},
                {"bot_name", speaker->GetName()},
                {"level", brief.value("level", 0)},
                {"class", brief.value("class", std::string{})},
                {"race", brief.value("race", std::string{})},
                {"zone", brief.value("zone", std::string{})},
                {"area", brief.value("area", std::string{})},
                {"speaker_name", speaker->GetName()},
                {"speaker_guid", botGuid},
                {"speaker_is_bot", true},
                {"channel", kind},
                {"channel_name", channelName},
                {"message", SanitizeUTF8(msg)},
                {"category", category},
                {"scene", std::to_string(SceneKey(speaker, kind)) + ":" + kind},
            };
            AddRoleplayFields(request, brief);
            // Now and then a bot's own remark starts a little conversation among the bots around it.
            if (urand(0, 99) < Number("OllamaChat.Ambient.RewriteScenePercent", 20))
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                Scene& scene = s_scenes[SceneKey(speaker, kind)];
                scene.expires = now + 45;
                scene.lines = 0;
            }
            const std::string original = msg;
            const uint32_t stagger = urand(100, 800);
            std::thread([botGuid, kind, request, original, stagger]() {
                struct SlotGuard { ~SlotGuard() { ReleaseGatewaySlot(); } } slot;
                std::string text;
                auto asked = std::chrono::steady_clock::now();
                try
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(stagger));
                    asked = std::chrono::steady_clock::now();
                    text = AskMind(request);
                }
                catch (const std::exception& error)
                {
                    LOG_ERROR("server.loading", "[Ollama Chat Ambient] rewrite failed: {}", error.what());
                }
                // The mind service was silent or unreachable: the stock line goes out after all, only a little late.
                const bool rewritten = !text.empty();
                if (!rewritten)
                    text = original;
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(TypingDelayMs(text.size(), asked)));
                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    s_lastLine[botGuid] = LastLine{HashOf(text), time(nullptr), 1};
                }
                nlohmann::json done = OllamaChat::WorldTask::Run([botGuid, kind, text]() -> nlohmann::json {
                    return nlohmann::json{{"ok", SpeakAndChain(botGuid, kind, text)}};
                }, 2000);
                if (g_DebugEnabled)
                    LOG_INFO("server.loading", "[Ollama Chat Ambient] rewrite {} {}: '{}' -> '{}' ({})", botGuid, kind, original, text,
                             done.value("ok", false) ? (rewritten ? "said" : "stock line") : "not said");
            }).detach();
            return true;   // the stock line does not go out; the thread above says something in its place
        }
        }   // namespace

        namespace
        {
            std::unordered_map<uint64_t, time_t> s_lastStarterAt;   // area -> when a bot last started a remark there
            uint32_t s_starterElapsedMs = 0;

            bool IsHuman(Player* player)
            {
                PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(player);
                return !(ai && ai->IsBotAI());
            }

            // A random bot in the same zone as `operatorPlayer` that is free to talk, or null. One pass, no allocation.
            Player* PickStarter(Player* operatorPlayer, const std::string& kind, time_t now, uint32_t* candidates = nullptr)
            {
                const bool sameZone = kind == "zone";
                const bool sameGuild = kind == "guild";
                const bool forTrade = kind == "trade";
                const bool inEarshot = kind == "say" || kind == "yell";   // a remark said aloud is heard only by those near enough
                const float earshot = static_cast<float>(kind == "yell" ? Number("OllamaChat.Roleplay.YellRangeYards", 90) : Number("OllamaChat.Roleplay.SayRangeYards", 30));
                Player* chosen = nullptr;
                Player* chosenCast = nullptr;
                uint32_t seen = 0, seenCast = 0;
                const uint32_t cooldown = Number("OllamaChat.Ambient.BotCooldownSec", 20);
                for (auto const& entry : ObjectAccessor::GetPlayers())
                {
                    Player* bot = entry.second;
                    if (!bot || bot == operatorPlayer || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight())
                        continue;
                    if (sameZone && (bot->GetMapId() != operatorPlayer->GetMapId() || bot->GetZoneId() != operatorPlayer->GetZoneId()))
                        continue;
                    if (inEarshot && (bot->GetMapId() != operatorPlayer->GetMapId() || bot->GetDistance(operatorPlayer) > earshot))
                        continue;
                    if (inEarshot && RoleplayOn() && !SameSideCanTalk(bot, operatorPlayer))
                        continue;   // the other side's speech is gibberish to the player
                    if (sameGuild && (!operatorPlayer->GetGuildId() || bot->GetGuildId() != operatorPlayer->GetGuildId()))
                        continue;
                    if (forTrade && (bot->GetTeamId() != operatorPlayer->GetTeamId() || !TradeChannelOf(bot)))
                        continue;   // only a bot standing in a city, on the operator's side, is in the channel the operator hears
                    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
                    if (!ai || !ai->IsBotAI())
                        continue;
                    const uint64_t guid = bot->GetGUID().GetRawValue();
                    if (IsGatewayBot(guid))
                        continue;
                    if (Group* group = bot->GetGroup())
                        if (group->IsMember(operatorPlayer->GetGUID()))
                            continue;   // a companion of the player talks through the party paths
                    {
                        std::lock_guard<std::mutex> lock(s_mutex);
                        auto last = s_lastReplyAt.find(guid);
                        if (last != s_lastReplyAt.end() && now - last->second < static_cast<time_t>(cooldown))
                            continue;
                    }
                    if (urand(0, seen++) == 0)   // reservoir sampling: every candidate is equally likely
                        chosen = bot;
                    if (IsCast(guid) && urand(0, seenCast++) == 0)
                        chosenCast = bot;
                }
                if (candidates)
                    *candidates = seen;
                // The regulars start most of what is said, so it is the same faces that are talking.
                if (chosenCast && urand(0, 99) < Number("OllamaChat.Community.CastStarterPercent", 75))
                    return chosenCast;
                return chosen;
            }

            std::string CoinText(uint32_t copper)
            {
                const uint32_t gold = copper / 10000, silver = (copper / 100) % 100, rest = copper % 100;
                std::string out;
                if (gold)
                    out += std::to_string(gold) + "g";
                if (silver)
                    out += (out.empty() ? "" : " ") + std::to_string(silver) + "s";
                if (rest && !gold)
                    out += (out.empty() ? "" : " ") + std::to_string(rest) + "c";
                return out.empty() ? "1c" : out;
            }

            // The realm has hundreds of placeholder and retired items ("RPGITEM PH - Consumable", "[DEPRECATED]") that a bot that
            // looted anything has in its bags. No player has ever seen one, so nobody puts one up for sale.
            bool IsRealItemName(const std::string& name)
            {
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                static const char* const kNotReal[] = {"rpgitem", "deprecated", "unused", "obsolete", "inform a developer", "ph -", "(test)", "[test]", "zzold"};
                for (const char* marker : kNotReal)
                    if (lower.find(marker) != std::string::npos)
                        return false;
                return !lower.empty();
            }

            // Something this bot really carries and could put up for sale: a tradeable item from its bags that a vendor would pay
            // for. The ask is a few times the vendor price, about how players price mats and drops. The whole stack is offered,
            // because the mail tool sends whole stacks. False when its bags hold nothing worth advertising.
            bool PickListing(Player* bot, Listing& out)
            {
                std::vector<Item*> options;
                auto consider = [&options](Item* item)
                {
                    if (!item || item->IsSoulBound() || item->IsBag())
                        return;
                    ItemTemplate const* proto = item->GetTemplate();
                    if (!proto || !proto->SellPrice || proto->Quality < ITEM_QUALITY_NORMAL || proto->Class == ITEM_CLASS_QUEST
                        || proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM || !IsRealItemName(proto->Name1))
                        return;
                    options.push_back(item);
                };
                for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                    consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
                for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
                    if (Bag* pBag = bot->GetBagByPos(bag))
                        for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
                            consider(pBag->GetItemByPos(slot));
                if (options.empty())
                    return false;

                Item* item = options[urand(0, options.size() - 1)];
                ItemTemplate const* proto = item->GetTemplate();
                static const char* const kQualityColour[] = {"9d9d9d", "ffffff", "1eff00", "0070dd", "a335ee", "ff8000"};
                const uint32 quality = std::min<uint32>(proto->Quality, 5);
                const uint32_t count = item->GetCount();
                uint64_t price = static_cast<uint64_t>(proto->SellPrice) * count * urand(25, 50) / 10;
                price = std::max<uint64_t>(50, (price + 25) / 50 * 50);   // in steps of 50 copper, never less than that
                price = std::min<uint64_t>(price, 2000000000u);

                out = Listing{};
                out.itemId = proto->ItemId;
                out.count = count;
                out.priceCopper = static_cast<uint32_t>(price);
                out.priceText = CoinText(out.priceCopper);
                out.link = std::string("|cff") + kQualityColour[quality] + "|Hitem:" + std::to_string(proto->ItemId)
                    + ":0:0:0:0:0:0:0:0|h[" + proto->Name1 + "]|h|r" + (count > 1 ? " x" + std::to_string(count) : std::string{});
                return true;
            }

            std::string PickKind(Player* operatorPlayer)
            {
                // Mostly the zone channel, sometimes LFG or Trade; a player in a guild hears their guild the most. A channel the
                // operator switched off is skipped.
                struct Option { const char* kind; uint32_t weight; };
                const uint32_t guildWeight = operatorPlayer && operatorPlayer->GetGuildId() ? Number("OllamaChat.Community.GuildChatWeight", 40) : 0;
                const uint32_t tradeWeight = TradeChannelOf(operatorPlayer) ? 10 : 0;   // nobody outside a city hears Trade
                const uint32_t sayWeight = RoleplayOn() ? Number("OllamaChat.Roleplay.SayWeight", 70) : 0;   // a character speaks to those near it
                const Option options[] = {{"say", sayWeight}, {"world", 45}, {"zone", 35}, {"lfg", 10}, {"trade", tradeWeight}, {"guild", guildWeight}};
                uint32_t total = 0;
                for (auto const& option : options)
                    if (KindAllowed(option.kind)) total += option.weight;
                if (!total) return "";
                uint32_t roll = urand(0, total - 1);
                for (auto const& option : options)
                {
                    if (!KindAllowed(option.kind)) continue;
                    if (roll < option.weight) return option.kind;
                    roll -= option.weight;
                }
                return "";
            }

            // The friends of a regular who are around and could hear a line in this channel: who the line might be spoken to.
            nlohmann::json OnlineFriends(Player* bot, Player* operatorPlayer, const std::string& kind)
            {
                nlohmann::json list = nlohmann::json::array();
                for (auto const& buddy : CastFriendsOf(bot->GetGUID().GetRawValue()))
                {
                    Player* other = ObjectAccessor::FindPlayer(ObjectGuid(buddy.guid));
                    if (!other || other == bot || !other->IsInWorld() || !other->IsAlive() || other->IsInCombat() || other->IsInFlight())
                        continue;
                    if ((kind == "zone" || kind == "say" || kind == "yell") && (other->GetMapId() != operatorPlayer->GetMapId() || other->GetZoneId() != operatorPlayer->GetZoneId()))
                        continue;
                    if (kind == "guild" && other->GetGuildId() != operatorPlayer->GetGuildId())
                        continue;
                    list.push_back({{"guid", buddy.guid}, {"name", other->GetName()}});
                }
                return list;
            }

            // ---- the cast, the home guild and the welcome ---------------------------------------------------------
            uint32_t s_castStatusMs = 0, s_castSyncMs = 0;
            bool     s_castSyncedOnce = false;
            std::unordered_map<uint64_t, time_t> s_pendingWelcome;   // operator guid -> when to greet them (under s_mutex)

            // World thread. Who is around, and which guilds the players are in, for the mind service to pick the regulars from.
            nlohmann::json DescribeRealmForCast()
            {
                nlohmann::json candidates = nlohmann::json::array();
                std::unordered_set<uint32_t> homes;
                for (auto const& entry : ObjectAccessor::GetPlayers())
                {
                    Player* p = entry.second;
                    if (!p || !p->IsInWorld())
                        continue;
                    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p);
                    if (ai && ai->IsBotAI())
                    {
                        const uint64_t guid = p->GetGUID().GetRawValue();
                        if (IsGatewayBot(guid))
                            continue;
                        nlohmann::json brief = DescribeBotBrief(p);
                        candidates.push_back({{"guid", guid}, {"name", p->GetName()}, {"team", static_cast<int>(p->GetTeamId())},
                                              {"guild_id", p->GetGuildId()}, {"level", brief.value("level", 0)},
                                              {"class", brief.value("class", std::string{})}, {"race", brief.value("race", std::string{})},
                                              {"gender", brief.value("gender", std::string{})}});
                    }
                    else if (p->GetGuildId() && OllamaChat::Promotion::IsOperator(p))
                        homes.insert(p->GetGuildId());
                }
                return {{"op", "sync"}, {"candidates", candidates}, {"homes", homes}};
            }

            // World thread. Puts a whitelisted player with no guild into one of the regulars' the first time they come online, so
            // that there is a guild chat with familiar faces in it. Remembered in the player's settings, so leaving sticks.
            std::string JoinHomeGuild(Player* player)
            {
                // "core." because, with EnablePlayerSettings off (the default), it is the only source the core saves: under any
                // other name the marker is gone at logout and someone who left the guild would be put back at the next login.
                static const std::string kSetting = "core.ollama_chat.community";
                if (!Number("OllamaChat.Community.JoinHomeGuild", 0) || player->GetGuildId() || player->GetPlayerSetting(kSetting, 0).value)
                    return "";
                std::unordered_map<uint32_t, uint32_t> weight;   // guild id -> how much of the crowd is in it, regulars counting triple
                for (auto const& entry : ObjectAccessor::GetPlayers())
                {
                    Player* bot = entry.second;
                    if (!bot || bot == player || !bot->IsInWorld() || !bot->GetGuildId() || bot->GetTeamId() != player->GetTeamId())
                        continue;
                    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
                    if (!ai || !ai->IsBotAI())
                        continue;
                    weight[bot->GetGuildId()] += IsCast(bot->GetGUID().GetRawValue()) ? 3 : 1;
                }
                uint32_t best = 0, bestWeight = 0;
                for (auto const& [guildId, score] : weight)
                    if (score > bestWeight || (score == bestWeight && guildId < best))
                    {
                        best = guildId;
                        bestWeight = score;
                    }
                Guild* guild = best ? sGuildMgr->GetGuildById(best) : nullptr;
                if (!guild || !guild->AddMember(player->GetGUID()))
                    return "";
                player->UpdatePlayerSetting(kSetting, 0, best);
                LOG_INFO("server.loading", "[Ollama Chat Community] {} joined the guild {} (id {})", player->GetName(), guild->GetName(), best);
                return guild->GetName();
            }

            // World thread. A player who has just come online is greeted by up to two of the regulars, the ones who know them best.
            void GreetPlayer(Player* player, bool joinedGuild)
            {
                const uint32_t guildId = player->GetGuildId();
                const std::string kind = guildId ? "guild" : "world";
                if (!KindAllowed(kind))
                    return;
                std::vector<std::pair<uint64_t, std::string>> greeters;
                std::unordered_map<uint64_t, nlohmann::json> briefs;   // read here, on the world thread, for the worker below
                for (auto const& entry : ObjectAccessor::GetPlayers())
                {
                    Player* bot = entry.second;
                    if (!bot || bot == player || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight())
                        continue;
                    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
                    if (!ai || !ai->IsBotAI() || bot->GetTeamId() != player->GetTeamId())
                        continue;
                    const uint64_t guid = bot->GetGUID().GetRawValue();
                    if (IsGatewayBot(guid) || (guildId && bot->GetGuildId() != guildId) || (!guildId && !IsCast(guid)))
                        continue;
                    greeters.emplace_back(guid, bot->GetName());
                    briefs[guid] = DescribeBotBrief(bot);
                }
                if (greeters.empty())
                    return;
                const uint64_t playerGuid = player->GetGUID().GetRawValue();
                const std::string playerName = player->GetName();
                const uint32_t count = std::max<uint32_t>(1, Number("OllamaChat.Community.WelcomeCount", 2));
                const uint64_t sceneKey = SceneKey(player, kind);   // the greeters are in the player's guild, so this is their scene too
                std::thread([greeters, briefs, playerGuid, playerName, kind, joinedGuild, count, sceneKey]() mutable {
                    try
                    {
                        nlohmann::json candidates = nlohmann::json::array();
                        for (auto const& greeter : greeters)
                            candidates.push_back(greeter.first);
                        const nlohmann::json ranked = PostMind("/cast", {{"op", "rank"}, {"player_guid", playerGuid}, {"candidates", candidates}}, 10);
                        std::vector<uint64_t> order;
                        if (ranked.is_object() && ranked.contains("order") && ranked["order"].is_array())
                            for (auto const& guid : ranked["order"])
                                order.push_back(guid.get<uint64_t>());
                        if (order.empty())
                            for (auto const& greeter : greeters)
                                order.push_back(greeter.first);
                        uint32_t spoken = 0;
                        for (uint64_t botGuid : order)
                        {
                            if (spoken >= count)
                                break;
                            // Someone who has just spoken does not greet in the same breath: the next one on the list does.
                            if (time(nullptr) - LastSpokeAt(botGuid) < 10)
                                continue;
                            std::string botName;
                            for (auto const& greeter : greeters)
                                if (greeter.first == botGuid)
                                    botName = greeter.second;
                            std::this_thread::sleep_for(std::chrono::milliseconds(spoken == 0 ? urand(1500, 3500) : urand(3000, 7000)));
                            const auto asked = std::chrono::steady_clock::now();
                            nlohmann::json welcome = {{"mode", "welcome"}, {"bot_guid", botGuid}, {"bot_name", botName},
                                                      {"player_guid", playerGuid}, {"player_name", playerName},
                                                      {"channel", kind}, {"joined", joinedGuild}};
                            auto brief = briefs.find(botGuid);
                            if (brief != briefs.end())
                                AddBrief(welcome, brief->second);
                            const nlohmann::json line = PostMind("/ambient", welcome, 15);
                            const std::string text = line.is_object() ? line.value("text", std::string{}) : std::string{};
                            if (text.empty())
                                continue;
                            // Typed like any other line, and never within LineGapMs of the last one said in this guild or channel.
                            std::this_thread::sleep_for(std::chrono::milliseconds(SendDelayMs(text.size(), asked, sceneKey)));
                            nlohmann::json done = OllamaChat::WorldTask::Run([botGuid, kind, text]() -> nlohmann::json {
                                return nlohmann::json{{"ok", SpeakAndChain(botGuid, kind, text)}};
                            }, 2000);
                            if (done.value("ok", false))
                            {
                                ++spoken;
                                RememberPartnerIn(playerGuid, kind, botGuid, false);   // if they answer, it is this one they talk to
                                std::lock_guard<std::mutex> lock(s_mutex);
                                s_lastLine[botGuid] = LastLine{HashOf(text), time(nullptr), 1};
                            }
                        }
                    }
                    catch (const std::exception& error)
                    {
                        LOG_ERROR("server.loading", "[Ollama Chat Community] welcome failed: {}", error.what());
                    }
                }).detach();
            }

            uint32_t s_modeMs = 1000000;   // the first world tick asks at once
            std::atomic<bool> s_modePolling{false};

            // Every few seconds, in the background, ask the mind service how the bots should talk, so that a switch on its
            // dashboard reaches the realm without a restart.
            void PollChatMode(uint32_t diff)
            {
                s_modeMs += diff;
                if (s_modeMs < Number("OllamaChat.Roleplay.PollSeconds", 10) * 1000 || s_modePolling.exchange(true))
                    return;
                s_modeMs = 0;
                std::thread([]() {
                    try
                    {
                        ApplyMode(PostMind("/cast", {{"op", "mode"}}, 5));
                    }
                    catch (const std::exception& error)
                    {
                        LOG_ERROR("server.loading", "[Ollama Chat Roleplay] mode request failed: {}", error.what());
                    }
                    s_modePolling = false;
                }).detach();
            }

            void CommunityTick(uint32_t diff)
            {
                s_castStatusMs += diff;
                s_castSyncMs += diff;
                const uint32_t syncMs = (s_castSyncedOnce ? Number("OllamaChat.Community.SyncMinutes", 10) * 60000 : 20000);
                if (s_castSyncMs >= syncMs)
                {
                    s_castSyncMs = 0;
                    nlohmann::json realm = DescribeRealmForCast();
                    if (realm["candidates"].size() >= 30)     // a realm still logging its bots in has no cast to pick from yet
                    {
                        s_castSyncedOnce = true;
                        s_castStatusMs = 0;
                        PostCastAsync(realm);
                    }
                }
                else if (s_castStatusMs >= 60000)
                {
                    s_castStatusMs = 0;
                    PostCastAsync({{"op", "status"}});
                }

                std::vector<uint64_t> due;
                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    const time_t now = time(nullptr);
                    for (auto it = s_pendingWelcome.begin(); it != s_pendingWelcome.end();)
                    {
                        if (it->second <= now)
                        {
                            due.push_back(it->first);
                            it = s_pendingWelcome.erase(it);
                        }
                        else
                            ++it;
                    }
                }
                for (uint64_t guid : due)
                {
                    Player* player = ObjectAccessor::FindPlayer(ObjectGuid(guid));
                    if (!player || !player->IsInWorld())
                        continue;
                    const bool joined = !JoinHomeGuild(player).empty();
                    if (joined)
                        s_castSyncMs = syncMs;   // the guild the player joined is where the regulars should come from
                    GreetPlayer(player, joined);
                }
            }
        }

        std::string RoleplayContextJson(Player* bot)
        {
            if (!bot)
                return "{}";
            nlohmann::json context = DescribeBotBrief(bot);
            context.erase("name");
            context.erase("health_pct");
            return context.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        }

        std::string PartyCallout(uint64_t botGuid, const std::string& botName, const std::string& kind, const std::string& mob,
                                 const std::string& contextJson)
        {
            if (!Enabled() || !g_GatewayEnable)
                return {};
            nlohmann::json request = {{"mode", "combat"}, {"bot_guid", botGuid}, {"bot_name", botName}, {"kind", kind}, {"mob", mob}};
            if (!contextJson.empty())
            {
                const nlohmann::json context = nlohmann::json::parse(contextJson, nullptr, false);
                if (context.is_object())
                    AddBrief(request, context);   // what the character is, so a bot first heard in a fight is still itself
            }
            const nlohmann::json answer = PostMind("/ambient", request, Number("OllamaChat.Director.CalloutTimeoutSeconds", 3));
            if (!answer.is_object() || !answer.contains("text") || !answer["text"].is_string())
                return {};
            std::string text = answer["text"].get<std::string>();
            if (text.size() > 200)
                text.clear();   // a line that long is not a call: the director says its own
            return text;
        }

        void NoteLogin(Player* player)
        {
            if (!player || !Enabled() || !Number("OllamaChat.Community.Enable", 1) || !OllamaChat::Promotion::IsOperator(player))
                return;
            std::lock_guard<std::mutex> lock(s_mutex);
            s_pendingWelcome[player->GetGUID().GetRawValue()] = time(nullptr) + static_cast<time_t>(Number("OllamaChat.Community.WelcomeDelaySec", 12));
        }

        void Tick(uint32_t diff)
        {
            if (Enabled() && g_GatewayEnable)
            {
                PollChatMode(diff);
                OllamaChat::Roleplay::Tick(diff);
            }
            if (Enabled() && g_GatewayEnable && Number("OllamaChat.Community.Enable", 1))
                CommunityTick(diff);
            s_starterElapsedMs += diff;
            const uint32_t intervalSec = Number("OllamaChat.Ambient.StarterIntervalSec", 40);
            if (!intervalSec || s_starterElapsedMs < intervalSec * 1000)
                return;
            s_starterElapsedMs = 0;
            if (!Enabled() || !g_GatewayEnable)
                return;

            const time_t now = time(nullptr);
            std::unordered_set<uint64_t> areasDone;
            for (auto const& entry : ObjectAccessor::GetPlayers())
            {
                Player* operatorPlayer = entry.second;
                if (!operatorPlayer || !operatorPlayer->IsInWorld() || !IsHuman(operatorPlayer) || !OllamaChat::Promotion::IsOperator(operatorPlayer))
                    continue;
                if (urand(0, 99) >= Number("OllamaChat.Ambient.StarterPercent", 100))
                    continue;
                const std::string kind = PickKind(operatorPlayer);
                if (kind.empty())
                    continue;
                const uint64_t areaKey = SceneKey(operatorPlayer, kind);
                if (!areasDone.insert(areaKey).second)
                    continue;
                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    auto scene = s_scenes.find(areaKey);
                    if (scene != s_scenes.end() && now <= scene->second.expires)
                        continue;   // people are already talking here: wait for them to finish
                    auto last = s_lastStarterAt.find(areaKey);
                    if (last != s_lastStarterAt.end() && now - last->second < static_cast<time_t>(Number("OllamaChat.Ambient.StarterMinGapSec", 45)))
                        continue;
                }
                uint32_t nearby = 0;
                Player* bot = PickStarter(operatorPlayer, kind, now, &nearby);
                if (!bot || ((kind == "zone" || kind == "guild") && nearby < 2))
                    continue;   // a zone or a guild with one bot in it is not a conversation
                Listing listing;
                if (kind == "trade" && !PickListing(bot, listing))
                    continue;   // a bot does not advertise what it does not have
                if (!TryAcquireGatewaySlot())
                    continue;

                const uint64_t botGuid = bot->GetGUID().GetRawValue();
                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    s_lastStarterAt[areaKey] = now;
                    s_lastReplyAt[botGuid] = now;
                }
                nlohmann::json brief = DescribeBotBrief(bot);
                nlohmann::json request = {
                    {"mode", "start"},
                    {"bot_guid", botGuid},
                    {"bot_name", bot->GetName()},
                    {"level", brief.value("level", 0)},
                    {"class", brief.value("class", std::string{})},
                    {"race", brief.value("race", std::string{})},
                    {"zone", brief.value("zone", std::string{})},
                    {"area", brief.value("area", std::string{})},
                    {"channel", kind},
                    {"scene", std::to_string(areaKey) + ":" + kind},
                };
                AddRoleplayFields(request, brief);
                if (listing.itemId)
                    request["listing"] = {{"link", listing.link}, {"price", listing.priceText}};
                nlohmann::json buddies = OnlineFriends(bot, operatorPlayer, kind);
                if (!buddies.empty())
                    request["friends"] = std::move(buddies);
                const uint32_t stagger = urand(0, 1500);
                std::thread([botGuid, kind, request, areaKey, stagger, listing]() {
                    struct SlotGuard { ~SlotGuard() { ReleaseGatewaySlot(); } } slot;
                    std::string text;
                    uint64_t answererGuid = 0;
                    auto asked = std::chrono::steady_clock::now();
                    try
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(stagger));
                        asked = std::chrono::steady_clock::now();
                        const nlohmann::json reply = AskMindJson(request);
                        if (reply.is_object())
                        {
                            text = reply.value("text", std::string{});
                            answererGuid = reply.value("addressed_guid", static_cast<uint64_t>(0));
                        }
                    }
                    catch (const std::exception& error)
                    {
                        LOG_ERROR("server.loading", "[Ollama Chat Ambient] starter failed: {}", error.what());
                    }
                    if (text.empty())
                        return;   // the bank had nothing for this bot: silence is fine
                    std::this_thread::sleep_for(std::chrono::milliseconds(SendDelayMs(text.size(), asked, areaKey)));
                    const time_t spokeAt = time(nullptr);
                    {
                        std::lock_guard<std::mutex> lock(s_mutex);
                        s_lastLine[botGuid] = LastLine{HashOf(text), spokeAt, 1};
                        Scene& scene = s_scenes[areaKey];
                        scene.expires = spokeAt + 45;   // the bots around it may answer, like after a rewritten stock line
                        scene.lines = 0;
                    }
                    if (listing.itemId)
                    {
                        std::lock_guard<std::mutex> lock(s_mutex);
                        s_listings[botGuid] = listing;
                        s_listings[botGuid].at = spokeAt;   // the offer stands from the moment it is said
                    }
                    nlohmann::json done = OllamaChat::WorldTask::Run([botGuid, kind, text, answererGuid]() -> nlohmann::json {
                        return nlohmann::json{{"ok", SpeakAndChain(botGuid, kind, text, answererGuid)}};
                    }, 2000);
                    if (listing.itemId && !done.value("ok", false))
                        ClearListing(botGuid);   // it never went out: nobody can be answering it
                    if (g_DebugEnabled)
                        LOG_INFO("server.loading", "[Ollama Chat Ambient] starter {} {}: '{}' ({})", botGuid, kind, text,
                                 done.value("ok", false) ? "said" : "not said");
                }).detach();
            }
        }

        bool IsSceneLine(uint64_t speakerGuid, const std::string& msg)
        {
            if (!Enabled())
                return false;
            std::lock_guard<std::mutex> lock(s_mutex);
            auto it = s_lastLine.find(speakerGuid);
            return it != s_lastLine.end() && it->second.hash == HashOf(msg) && time(nullptr) - it->second.at <= kLineLifetimeSec;
        }

        namespace
        {
        std::string PartnerKey(uint64_t speakerGuid, const std::string& kind)
        {
            return std::to_string(speakerGuid) + "|" + kind;
        }

        void RememberPartnerIn(uint64_t speakerGuid, const std::string& kind, uint64_t botGuid, bool force)
        {
            const time_t window = Number("OllamaChat.Ambient.PartnerWindowSec", 120);
            if (!window || kind.empty())
                return;
            const time_t now = time(nullptr);
            std::lock_guard<std::mutex> lock(s_mutex);
            Partner& partner = s_partners[PartnerKey(speakerGuid, kind)];
            if (!force && partner.botGuid && partner.botGuid != botGuid && now - partner.at <= window)
                return;   // whoever answered first keeps the conversation
            partner = Partner{botGuid, now};
            if (s_partners.size() > 512)
                for (auto it = s_partners.begin(); it != s_partners.end();)
                    it = (now - it->second.at > window) ? s_partners.erase(it) : std::next(it);
        }
        }   // namespace

        time_t LastSpokeAt(uint64_t botGuid)
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto it = s_lastLine.find(botGuid);
            return it == s_lastLine.end() ? 0 : it->second.at;
        }

        std::string ChannelKind(ChatChannelSourceLocal source, Channel* channel)
        {
            return KindOf(source, channel);
        }

        bool SayInChannel(uint64_t botGuid, const std::string& kind, const std::string& text)
        {
            return Speak(botGuid, kind, text);
        }

        bool GetListing(uint64_t botGuid, Listing& out)
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto it = s_listings.find(botGuid);
            if (it == s_listings.end())
                return false;
            if (time(nullptr) - it->second.at > static_cast<time_t>(Number("OllamaChat.Ambient.ListingMinutes", 30)) * 60)
            {
                s_listings.erase(it);
                return false;
            }
            out = it->second;
            return true;
        }

        void ClearListing(uint64_t botGuid)
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_listings.erase(botGuid);
        }

        uint64_t PartnerOf(Player* speaker, ChatChannelSourceLocal source, Channel* channel)
        {
            const time_t window = Number("OllamaChat.Ambient.PartnerWindowSec", 120);
            const std::string kind = KindOf(source, channel);
            if (!speaker || !window || kind.empty())
                return 0;
            std::lock_guard<std::mutex> lock(s_mutex);
            auto it = s_partners.find(PartnerKey(speaker->GetGUID().GetRawValue(), kind));
            return it != s_partners.end() && time(nullptr) - it->second.at <= window ? it->second.botGuid : 0;
        }

        void RememberPartner(Player* speaker, ChatChannelSourceLocal source, Channel* channel, uint64_t botGuid, bool force)
        {
            if (speaker)
                RememberPartnerIn(speaker->GetGUID().GetRawValue(), KindOf(source, channel), botGuid, force);
        }

        bool IsInviteRequest(const std::string& msg)
        {
            bool invite = false, toMe = false;
            std::string word;
            auto flush = [&]()
            {
                if (word == "invite" || word == "invites" || word == "inv")
                    invite = true;
                else if (word == "me" || word == "us" || word == "pls" || word == "plz" || word == "please")
                    toMe = true;
                word.clear();
            };
            for (unsigned char c : msg)
            {
                if (std::isalnum(c))
                    word += static_cast<char>(std::tolower(c));
                else
                    flush();
            }
            flush();
            return invite && toMe;
        }

        namespace
        {
        bool HandleIn(Player* bot, Player* speaker, const std::string& msg, const std::string& kind, const std::string& channelName,
                      bool speakerIsBot, bool addressed)
        {
            if (!Enabled() || !g_GatewayEnable || !bot || !speaker || bot == speaker)
                return false;
            if (kind.empty() || !KindAllowed(kind))
                return false;

            const uint64_t botGuid = bot->GetGUID().GetRawValue();
            if (IsGatewayBot(botGuid) || !bot->IsAlive() || bot->IsInFlight())
                return false;   // the fleet has its own wiring; the dead and the flying say nothing

            uint32_t depth = 1;
            bool directed = addressed;   // spoken to this bot (by name, or in a conversation it is in): it always answers
            if (speakerIsBot)
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                auto it = s_lastLine.find(speaker->GetGUID().GetRawValue());
                if (it == s_lastLine.end() || it->second.hash != HashOf(msg) || time(nullptr) - it->second.at > kLineLifetimeSec)
                    return false;   // an ordinary bot line (or an old one): not part of a conversation we started
                depth = it->second.depth + 1;
                if (depth > Number("OllamaChat.Ambient.ChainMaxDepth", 2) + 1)
                    return true;
            }
            else
            {
                if (!OllamaChat::Promotion::IsOperator(speaker))
                    return false;   // only whitelisted players start a conversation: every answer costs a model call
                if (OllamaChat::Promotion::MentionsName(msg, bot->GetName()))
                    directed = true;
                if (directed && NeedsTools(msg))
                    return false;   // asked to DO something: the ordinary (promoted, tool-using) path answers it
                if (RoleplayOn() && !SameSideCanTalk(bot, speaker))
                    return true;    // it is gibberish to the bot (and the player would read none of the answer): a character does not reply to it
                if (Group* group = bot->GetGroup())
                    if (group->IsMember(speaker->GetGUID()))
                        return false;   // a party companion is spoken to through the party paths
            }

            const time_t now = time(nullptr);
            const uint64_t areaKey = SceneKey(bot, kind);
            uint32_t staggerIndex = 0;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                auto last = s_lastReplyAt.find(botGuid);
                if (!directed && last != s_lastReplyAt.end() && now - last->second < static_cast<time_t>(Number("OllamaChat.Ambient.BotCooldownSec", 20)))
                    return true;   // this bot spoke a moment ago: quiet, and the line is still ours
                Scene& scene = s_scenes[areaKey];
                if (!speakerIsBot)
                {
                    scene.expires = now + static_cast<time_t>(Number("OllamaChat.Ambient.SceneWindowSec", 90));
                    scene.lines = 0;
                }
                else if (now > scene.expires)
                    return true;
                if (scene.lines >= Number("OllamaChat.Ambient.MaxLinesPerScene", 8))
                    return true;
                ++scene.lines;
                s_lastReplyAt[botGuid] = now;

                const uint64_t heardKey = HashOf(std::to_string(speaker->GetGUID().GetRawValue()) + "|" + msg);
                auto& batch = s_batch[heardKey];
                if (now - batch.first > 3) batch = {now, 0};
                staggerIndex = batch.second++;
                if (s_batch.size() > 512)
                    for (auto it = s_batch.begin(); it != s_batch.end();)
                        it = (now - it->second.first > 10) ? s_batch.erase(it) : std::next(it);
            }

            if (!TryAcquireGatewaySlot())
                return true;

            nlohmann::json brief = DescribeBotBrief(bot);
            nlohmann::json request = {
                {"bot_guid", botGuid},
                {"bot_name", bot->GetName()},
                {"level", brief.value("level", 0)},
                {"class", brief.value("class", std::string{})},
                {"race", brief.value("race", std::string{})},
                {"zone", brief.value("zone", std::string{})},
                {"area", brief.value("area", std::string{})},
                {"speaker_guid", speaker->GetGUID().GetRawValue()},
                {"speaker_name", speaker->GetName()},
                {"speaker_is_bot", speakerIsBot},
                {"channel", kind},
                {"channel_name", channelName},
                {"message", SanitizeUTF8(msg)},
                {"depth", depth},
                {"addressed", directed},
                {"distance", static_cast<double>(bot->GetDistance(speaker))},
                {"scene", std::to_string(areaKey) + ":" + kind},
            };
            AddRoleplayFields(request, brief);

            // Somebody who is spoken to looks up and thinks: a bot's own thinking pose while the words are fetched.
            if (directed && RoleplayOn() && Number("OllamaChat.Roleplay.AckEmote", 1) && !bot->IsInCombat())
                bot->HandleEmoteCommand(EMOTE_ONESHOT_QUESTION);
            const bool speech = RoleplayOn();
            const uint32_t stagger = static_cast<uint32_t>(Number("OllamaChat.Ambient.StaggerMs", 1600)) * staggerIndex
                + urand(speech ? Number("OllamaChat.Roleplay.JitterMinMs", 250) : Number("OllamaChat.Ambient.JitterMinMs", 600),
                        speech ? Number("OllamaChat.Roleplay.JitterMaxMs", 900) : Number("OllamaChat.Ambient.JitterMaxMs", 1800));
            const uint32_t maxChars = Number("OllamaChat.Ambient.MaxLineChars", 200);

            const uint64_t speakerGuid = speaker->GetGUID().GetRawValue();
            std::thread([botGuid, kind, request, stagger, depth, maxChars, speakerGuid, speakerIsBot, directed, areaKey]() {
                struct SlotGuard { ~SlotGuard() { ReleaseGatewaySlot(); } } slot;
                try
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(stagger));
                    const auto asked = std::chrono::steady_clock::now();
                    std::string text = AskMind(request);
                    if (text.empty())
                        return;
                    if (text.size() > maxChars)
                    {
                        text.resize(maxChars);
                        const size_t space = text.find_last_of(' ');
                        if (space != std::string::npos && space > maxChars / 2)
                            text.resize(space);
                    }
                    // Typing takes a moment, and never instant; the time the answer took to arrive counts towards it.
                    std::this_thread::sleep_for(std::chrono::milliseconds(SendDelayMs(text.size(), asked, areaKey)));

                    const uint64_t hash = HashOf(text);
                    {
                        std::lock_guard<std::mutex> lock(s_mutex);
                        s_lastLine[botGuid] = LastLine{hash, time(nullptr), depth};
                    }
                    // On the world thread, so the chat hook that hears this line (and lets the next bot answer it) runs there too.
                    nlohmann::json done = OllamaChat::WorldTask::Run([botGuid, kind, text]() -> nlohmann::json {
                        return nlohmann::json{{"ok", SpeakAndChain(botGuid, kind, text)}};
                    }, 2000);
                    if (!speakerIsBot && done.value("ok", false))
                        RememberPartnerIn(speakerGuid, kind, botGuid, directed);   // they answered a player: they carry on with them
                    if (g_DebugEnabled)
                        LOG_INFO("server.loading", "[Ollama Chat Ambient] {} {} (depth {}): '{}' -> {}", botGuid, kind, depth, text,
                                 done.value("ok", false) ? "said" : "not said");
                }
                catch (const std::exception& error)
                {
                    LOG_ERROR("server.loading", "[Ollama Chat Ambient] failed: {}", error.what());
                }
            }).detach();
            return true;
        }

        // After a bot's channel line was said: a few other bots may answer it, like the ones that hear /say do.
        void ChainFrom(uint64_t speakerGuid, const std::string& kind, const std::string& text)
        {
            Player* speaker = ObjectAccessor::FindPlayer(ObjectGuid(speakerGuid));
            if (!speaker || !speaker->IsInWorld())
                return;
            const uint32_t chance = ChainChancePercent();
            // One or two answer each line, so a room reads as people taking turns and not everyone piling on the first remark.
            const uint32_t wanted = urand(1, std::max<uint32_t>(1, std::min<uint32_t>(2, Number("OllamaChat.MaxBotsToPick", 3))));
            std::vector<Player*> chosen;
            uint32_t seen = 0;
            bool haveCast;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                haveCast = !s_cast.empty();
            }
            for (auto const& entry : ObjectAccessor::GetPlayers())
            {
                Player* bot = entry.second;
                if (!bot || bot == speaker || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight())
                    continue;
                if (kind == "zone" && (bot->GetMapId() != speaker->GetMapId() || bot->GetZoneId() != speaker->GetZoneId()))
                    continue;   // the zone channel only carries within its zone
                if (kind == "guild" && (!speaker->GetGuildId() || bot->GetGuildId() != speaker->GetGuildId()))
                    continue;   // guild chat is read by the guild
                PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
                if (!ai || !ai->IsBotAI() || IsGatewayBot(bot->GetGUID().GetRawValue()))
                    continue;
                // A friend of the speaker nearly always answers, the regulars answer more often than the crowd.
                const uint64_t botId = bot->GetGUID().GetRawValue();
                uint32_t botChance = chance;
                if (AreFriends(speakerGuid, botId))
                    botChance = 100;
                else if (IsCast(botId))
                    botChance = std::min<uint32_t>(100, chance + 15);
                else if (haveCast)
                    botChance = chance * 2 / 3;
                if (urand(0, 99) >= botChance)
                    continue;
                // reservoir sampling of `wanted` bots, each equally likely
                if (chosen.size() < wanted)
                    chosen.push_back(bot);
                else
                {
                    const uint32_t slot = urand(0, seen);
                    if (slot < wanted)
                        chosen[slot] = bot;
                }
                ++seen;
            }
            for (Player* bot : chosen)
                HandleIn(bot, speaker, text, kind, std::string{}, true);
        }
        }   // namespace

        bool Handle(Player* bot, Player* speaker, const std::string& msg, ChatChannelSourceLocal source,
                    Channel* channel, bool speakerIsBot, bool addressed)
        {
            if (source != SRC_SAY_LOCAL && source != SRC_YELL_LOCAL && source != SRC_GENERAL_LOCAL && source != SRC_GUILD_LOCAL)
                return false;
            return HandleIn(bot, speaker, msg, KindOf(source, channel), channel ? channel->GetName() : std::string{}, speakerIsBot,
                            addressed);
        }
    }
}

// Defined here and declared weak in playerbots (patches/mod-playerbots-outgoing-channel-filter.patch): playerbots hands the
// lines its bots send to a public channel to this filter before sending them.
std::function<bool(Player*, int, std::string const&)> g_ollamaOutgoingChannelFilter;

namespace
{
    struct OutgoingChannelFilterInstaller
    {
        OutgoingChannelFilterInstaller()
        {
            g_ollamaOutgoingChannelFilter = [](Player* bot, int chan, std::string const& msg) {
                return OllamaChat::Ambient::MaybeRewriteChannel(bot, chan, msg);
            };
        }
    } s_installOutgoingChannelFilter;
}

CommunityScript::CommunityScript() : PlayerScript("CommunityScript") {}

void CommunityScript::OnPlayerLogin(Player* player)
{
    OllamaChat::Ambient::NoteLogin(player);
}
