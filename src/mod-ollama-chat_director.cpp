#include "mod-ollama-chat_director.h"

#include "mod-ollama-chat_ambient.h"
#include "mod-ollama-chat_director_core.h"
#include "mod-ollama-chat_jev.h"
#include "mod-ollama-chat_playerprefs.h"
#include "mod-ollama-chat_tactical.h"
#include "mod-ollama-chat_worldtask.h"

#include "ChatHelper.h"
#include "Config.h"
#include "Creature.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Random.h"
#include "Spell.h"
#include "SpellInfo.h"

#include <fmt/core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace OllamaChat
{
    namespace CombatDirector
    {
        namespace Dir = ::Director;

        namespace
        {
            constexpr uint8 kSkullSlot = 7;   // Group target icons: 4 = moon, 7 = skull
            constexpr uint8 kMoonSlot  = 4;
            constexpr int64_t kStaleInFlightMs = 10000;   // a model call that never came back must not wedge the group
            constexpr int64_t kForgetGroupMs   = 60000;

            uint32_t Number(const char* key, uint32_t fallback) { return sConfigMgr->GetOption<uint32_t>(key, fallback, false); }

            // "core." because, with EnablePlayerSettings off (the default), it is the only source the core saves: under any other
            // name the choice is gone at logout.
            const std::string kOffSetting = "core.ollama_chat.director_off";

            int64_t NowMs()
            {
                return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            }

            // World thread only, except s_recent (under s_logMutex).
            struct FightRecord
            {
                Dir::GroupState state;
                std::unordered_set<uint64_t> savedManaAdded;   // healers whose `save mana` WE switched on
                int64_t startedMs = 0;                          // when the first consult of this fight was due
                uint64_t lastSpeaker = 0;                       // the bot that made the last announcement, so the voice changes
                int64_t emptySinceMs = 0;                       // since when no enemy has been attacking the party (0 = one is)
            };
            // A player's say over a mark outlives the fight record: a chain pull, or a tick in which nothing was attacking, must not
            // launder it. group guid -> the yields still running when the last record ended.
            struct CarriedYield { int64_t skullUntil = 0; int64_t moonUntil = 0; };
            std::unordered_map<uint64_t, CarriedYield> s_carriedYield;
            constexpr int64_t kEndGraceMs = 4000;   // how long an empty snapshot must last before a fight counts as over

            std::unordered_map<uint64_t, FightRecord> s_fights;   // group guid -> what the director has done there
            std::deque<int64_t> s_callTimesMs;                     // for MaxCallsPerHour
            uint32_t s_elapsedMs = 0;

            std::mutex s_logMutex;
            std::deque<nlohmann::json> s_recent;
            std::string s_lastParty;   // under s_logMutex: the party of the most recent fight, as logged at its start
            std::atomic<uint64_t> s_totalConsults{0};
            std::atomic<uint64_t> s_yieldTicks{0};
            std::atomic<uint64_t> s_overrides{0};
            std::atomic<uint64_t> s_ccReleased{0};   // moon marks taken off because the crowd control was not holding
            std::unordered_map<uint64_t, int64_t> s_forceCc;   // human guid -> when the request lapses; world thread only
            std::unordered_set<uint64_t> s_forceConserve;   // human guids; world thread only (the command and Tick both run there)
            std::atomic<int64_t>  s_conserving{0};          // healers carrying a `save mana` the director switched on
            std::atomic<uint64_t> s_postureMismatches{0};   // times the engine's strategy list disagreed with what was just asked
            std::atomic<uint32_t> s_openFights{0};   // s_fights.size(), mirrored for readers off the world thread

            void Remember(nlohmann::json entry)
            {
                std::lock_guard<std::mutex> lock(s_logMutex);
                s_recent.push_back(std::move(entry));
                while (s_recent.size() > 50) s_recent.pop_front();
            }

            // Stock classes by name; Conquest of Azeroth's own (ids 12+) from the client's class table, as the rest of the module does.
            std::string ClassName(uint8 c)
            {
                switch (c)
                {
                    case CLASS_WARRIOR:      return "warrior";
                    case CLASS_PALADIN:      return "paladin";
                    case CLASS_HUNTER:       return "hunter";
                    case CLASS_ROGUE:        return "rogue";
                    case CLASS_PRIEST:       return "priest";
                    case CLASS_DEATH_KNIGHT: return "death knight";
                    case CLASS_SHAMAN:       return "shaman";
                    case CLASS_MAGE:         return "mage";
                    case CLASS_WARLOCK:      return "warlock";
                    case CLASS_DRUID:        return "druid";
                }
                const std::string named = ChatHelper::FormatClass(c);
                return named.empty() ? std::string("unknown") : named;
            }

            // Playerbots aims crowd control at the moon mark for these stock classes (polymorph, sap, freezing trap, banish and fear,
            // entangling roots, hibernate, cyclone). Priests are left out on purpose: their only trigger is Shackle Undead, which does
            // nothing to an ordinary mob, so a moon mark would be wasted on them. The Conquest of Azeroth class AI is spell-driven and
            // does not read `rti cc` at all, so those classes are never offered as crowd-controllers.
            bool CanCrowdControl(uint8 c)
            {
                return c == CLASS_MAGE || c == CLASS_ROGUE || c == CLASS_HUNTER || c == CLASS_WARLOCK || c == CLASS_DRUID;
            }

            bool IsBot(Player* p)
            {
                PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p);
                return ai && ai->IsBotAI();
            }

            Dir::Tuning ReadTuning()
            {
                Dir::Tuning t;
                t.minConfidence = Jev::MinConfidenceFor(Jev::kSiteDirector);
                t.minEnemies    = static_cast<int>(Number("OllamaChat.Director.MinEnemies", 2));
                t.focusHoldMs   = static_cast<int64_t>(Number("OllamaChat.Director.FocusHoldSec", 6)) * 1000;
                t.yieldMs       = static_cast<int64_t>(Number("OllamaChat.Director.YieldSec", 20)) * 1000;
                t.allowCc       = Number("OllamaChat.Director.CrowdControl", 1) != 0;
                return t;
            }

            // The party and the enemies engaged with it, as the model will see them. World thread.
            // False for fights the director does not direct: raids, battlegrounds, anything with a player on the other side.
            bool Gather(Player* human, Group* group, Dir::Snapshot& snap)
            {
                if (group->isRaidGroup() || group->isBGGroup() || group->isBFGroup())
                    return false;

                std::unordered_map<uint64_t, int> memberIndex;
                std::vector<Player*> present;
                for (auto const& slot : group->GetMemberSlots())
                {
                    Player* m = ObjectAccessor::FindPlayer(slot.guid);
                    if (!m || !m->IsInWorld() || m->GetMapId() != human->GetMapId())
                        continue;   // a member in another zone is not in this fight

                    Dir::Member d;
                    d.guid  = m->GetGUID().GetRawValue();
                    d.name  = m->GetName();
                    d.cls   = ClassName(m->getClass());
                    d.canCc = CanCrowdControl(m->getClass());
                    d.isBot = IsBot(m);
                    d.alive = m->IsAlive();
                    d.hpPct = static_cast<int>(m->GetHealthPct());
                    if (Unit* attacking = m->GetVictim())
                        d.targetGuid = attacking->GetGUID().GetRawValue();
                    const Powers power = m->getPowerType();
                    const uint32 maxPower = m->GetMaxPower(power);
                    d.usesMana = power == POWER_MANA;
                    d.powerPct = maxPower ? static_cast<int>(100.0f * m->GetPower(power) / maxPower) : 100;
                    if (d.isBot)
                    {
                        PlayerbotAI* botAi = PlayerbotsMgr::instance().GetPlayerbotAI(m);
                        d.manaSaving = botAi && botAi->HasStrategy("save mana", BOT_STATE_COMBAT);
                    }
                    if (!d.isBot)                    d.role = Dir::Role::Human;
                    else if (PlayerbotAI::IsTank(m)) d.role = Dir::Role::Tank;
                    else if (PlayerbotAI::IsHeal(m)) d.role = Dir::Role::Healer;
                    else if (PlayerbotAI::IsRanged(m)) d.role = Dir::Role::Ranged;
                    else                             d.role = Dir::Role::Melee;

                    memberIndex[d.guid] = static_cast<int>(snap.members.size());
                    snap.members.push_back(std::move(d));
                    present.push_back(m);
                }

                std::unordered_set<uint64_t> seen;
                for (Player* m : present)
                {
                    for (Unit* u : m->getAttackers())
                    {
                        if (!u || !u->IsInWorld() || !u->IsAlive() || !seen.insert(u->GetGUID().GetRawValue()).second)
                            continue;
                        if (u->IsPlayer())
                            return false;   // PvP is not this director's job
                        Creature* c = u->ToCreature();
                        if (!c || c->IsTotem() || c->IsTrigger())
                            continue;

                        Dir::Enemy e;
                        e.guid  = c->GetGUID().GetRawValue();
                        e.name  = c->GetName();
                        e.level = c->GetLevel();
                        e.hpPct = static_cast<int>(c->GetHealthPct());
                        if (c->isWorldBoss() || c->IsDungeonBoss())
                            e.rank = "boss";
                        else
                        {
                            switch (c->GetCreatureTemplate()->rank)
                            {
                                case CREATURE_ELITE_ELITE:
                                case CREATURE_ELITE_RAREELITE: e.rank = "elite"; break;
                                case CREATURE_ELITE_RARE:      e.rank = "rare";  break;
                                default: break;
                            }
                        }
                        if (Spell* cast = c->GetCurrentSpell(CURRENT_GENERIC_SPELL))
                        {
                            if (SpellInfo const* info = cast->GetSpellInfo())
                            {
                                const char* spellName = info->SpellName[LocaleConstant::LOCALE_enUS];
                                if (spellName && *spellName)
                                {
                                    e.casting = spellName;
                                    e.interruptible = (info->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) != 0;
                                }
                            }
                        }
                        if (Unit* victim = c->GetVictim())
                        {
                            auto it = memberIndex.find(victim->GetGUID().GetRawValue());
                            if (it != memberIndex.end())
                                e.targetMember = it->second;
                        }
                        snap.enemies.push_back(std::move(e));
                    }
                }
                // getAttackers() is an ordered set of pointers: sort so a consult does not depend on allocation order.
                std::sort(snap.enemies.begin(), snap.enemies.end(), [](const Dir::Enemy& a, const Dir::Enemy& b) { return a.guid < b.guid; });
                return true;
            }

            // What is on the board in `slot`, if it points at something still alive. A skull left on last pull's corpse
            // is nobody's mark; without this it would read as "a human's" and lock the director out of every fight.
            uint64_t LiveMark(Group* group, Player* human, uint8 slot)
            {
                const uint64_t guid = group->GetTargetIcon(slot).GetRawValue();
                if (!guid)
                    return 0;
                Unit* u = ObjectAccessor::GetUnit(*human, ObjectGuid(guid));
                return u && u->IsAlive() ? guid : 0;
            }

            void SetMark(Group* group, Player* human, uint8 slot, uint64_t enemyGuid)
            {
                group->SetTargetIcon(slot, human->GetGUID(), ObjectGuid(enemyGuid));
            }

            void ClearMark(Group* group, Player* human, uint8 slot)
            {
                group->SetTargetIcon(slot, human->GetGUID(), ObjectGuid::Empty);
            }

            // Healers ration mana through playerbots' own `save mana` strategy (it vetoes heals that would mostly
            // overheal once mana is under AiPlayerbot.SaveManaThreshold). Remember the ones we switched on, so
            // putting things back never strips a strategy somebody else gave the bot.
            void ApplyPosture(Group* group, FightRecord& fight, Dir::Posture posture)
            {
                for (auto const& slot : group->GetMemberSlots())
                {
                    Player* m = ObjectAccessor::FindPlayer(slot.guid);
                    if (!m || !m->IsInWorld() || !IsBot(m) || !PlayerbotAI::IsHeal(m))
                        continue;
                    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(m);
                    const uint64_t guid = m->GetGUID().GetRawValue();
                    bool changed = false;
                    bool wantOn = false;
                    if (posture == Dir::Posture::Conserve)
                    {
                        if (ai->HasStrategy("save mana", BOT_STATE_COMBAT) && !fight.savedManaAdded.count(guid))
                            LOG_INFO("server.loading", "[Ollama Chat Director] {} already had `save mana` in combat: left as it is", m->GetName());
                        else if (!ai->HasStrategy("save mana", BOT_STATE_COMBAT))
                        {
                            ai->ChangeStrategy("+save mana", BOT_STATE_COMBAT);
                            fight.savedManaAdded.insert(guid);
                            ++s_conserving;
                            changed = wantOn = true;
                        }
                    }
                    else if (fight.savedManaAdded.erase(guid))
                    {
                        ai->ChangeStrategy("-save mana", BOT_STATE_COMBAT);
                        --s_conserving;
                        changed = true;
                    }
                    if (!changed)
                        continue;
                    // Trust, but look: the engine's own list must now say what was asked, or the director is not doing what it says.
                    if (ai->HasStrategy("save mana", BOT_STATE_COMBAT) != wantOn)
                    {
                        ++s_postureMismatches;
                        LOG_WARN("server.loading", "[Ollama Chat Director] {} asked to {} `save mana` but the bot's combat strategies disagree",
                                 m->GetName(), wantOn ? "add" : "remove");
                    }
                    else
                        LOG_INFO("server.loading", "[Ollama Chat Director] {}: `save mana` {}", m->GetName(), wantOn ? "on" : "off");
                }
            }

            // A fight record is going away: whatever it still claims to have switched on (a healer that logged out, a group that
            // is gone) is no longer anybody's to take back.
            void DropRecord(FightRecord& fight)
            {
                s_conserving -= static_cast<int64_t>(fight.savedManaAdded.size());
                fight.savedManaAdded.clear();
            }

            // One line of party chat from one of the bots, so the player hears the plan. The speaker is a random living bot, not the
            // one that spoke last when there is a choice: a party where the tank calls every target is one voice, not a group. The
            // mind service has the bot say it in its own voice (a worker thread asks for it); `plain` is what goes out when it has nothing.
            void Announce(Group* group, FightRecord& fight, int64_t now, const std::string& kind, const std::string& mob, const std::string& plain)
            {
                Dir::GroupState& st = fight.state;
                if (!Number("OllamaChat.Director.Announce", 1) || now - st.lastAnnounceMs < 8000)
                    return;
                std::vector<Player*> bots;
                for (auto const& slot : group->GetMemberSlots())
                {
                    Player* m = ObjectAccessor::FindPlayer(slot.guid);
                    if (m && m->IsInWorld() && m->IsAlive() && IsBot(m))
                        bots.push_back(m);
                }
                if (bots.size() > 1)
                    bots.erase(std::remove_if(bots.begin(), bots.end(), [&](Player* b) { return b->GetGUID().GetRawValue() == fight.lastSpeaker; }), bots.end());
                if (bots.empty())
                    return;
                Player* speaker = bots[urand(0, static_cast<uint32>(bots.size()) - 1)];
                fight.lastSpeaker = speaker->GetGUID().GetRawValue();
                st.lastAnnounceMs = now;   // the slot is taken now; the line arrives a moment later
                const uint64_t speakerGuid = speaker->GetGUID().GetRawValue();
                const std::string speakerName = speaker->GetName();
                const std::string context = OllamaChat::Ambient::RoleplayContextJson(speaker);   // read here, on the world thread
                std::thread([speakerGuid, speakerName, kind, mob, plain, context]() {
                    std::string text = OllamaChat::Ambient::PartyCallout(speakerGuid, speakerName, kind, mob, context);
                    if (text.empty())
                        text = plain;
                    OllamaChat::WorldTask::Run([speakerGuid, text]() -> nlohmann::json {
                        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(speakerGuid));
                        PlayerbotAI* ai = bot && bot->IsInWorld() && bot->IsAlive() ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
                        if (ai)
                            ai->SayToParty(text);
                        return nlohmann::json{};
                    }, 3000);
                }).detach();
            }

            struct Line { const char* before; const char* after; };

            std::string FocusLine(const std::string& name)
            {
                static const Line kLines[] = {{"Focus the ", "."}, {"Kill the ", " first."}, {"", " first, then the rest."}, {"Burn down the ", "."}};
                const Line& l = kLines[urand(0, 3)];
                return std::string(l.before) + name + l.after;
            }

            std::string CcLine(const std::string& name)
            {
                static const Line kLines[] = {{"Crowd-controlling the ", "."}, {"Keep the ", " locked down."}, {"Someone hold the ", "."}};
                const Line& l = kLines[urand(0, 2)];
                return std::string(l.before) + name + l.after;
            }

            // The human left mid-fight and nobody will end it: if the group is still there, give its healers their strategies back.
            void ReleaseStrayPosture(uint64_t groupGuid, FightRecord& fight)
            {
                if (fight.savedManaAdded.empty())
                    return;
                if (Group* group = sGroupMgr->GetGroupByGUID(ObjectGuid(groupGuid).GetCounter()))
                    ApplyPosture(group, fight, Dir::Posture::Push);
            }

            // The fight is over (or the group is gone): take back what we put up.
            void EndFight(uint64_t groupGuid, Group* group, Player* human)
            {
                auto it = s_fights.find(groupGuid);
                if (it == s_fights.end())
                    return;
                FightRecord& fight = it->second;
                LOG_INFO("server.loading", "[Ollama Chat Director] group {}: fight over after {:.1f} s, {} consults",
                         groupGuid, (NowMs() - fight.startedMs) / 1000.0, fight.state.calls);
                if (group && human)
                {
                    if (fight.state.skullGuid && group->GetTargetIcon(kSkullSlot).GetRawValue() == fight.state.skullGuid)
                        ClearMark(group, human, kSkullSlot);
                    if (fight.state.moonGuid && group->GetTargetIcon(kMoonSlot).GetRawValue() == fight.state.moonGuid)
                        ClearMark(group, human, kMoonSlot);
                    if (fight.state.posture == Dir::Posture::Conserve || !fight.savedManaAdded.empty())
                        ApplyPosture(group, fight, Dir::Posture::Push);
                }
                if (fight.state.skullYieldUntil > NowMs() || fight.state.moonYieldUntil > NowMs())
                    s_carriedYield[groupGuid] = CarriedYield{fight.state.skullYieldUntil, fight.state.moonYieldUntil};
                DropRecord(fight);
                s_fights.erase(it);
            }

            // Marks we put on units that have since died or left: let go of them before comparing with the board.
            // A crowd-controlled mob is not attacking anybody, so it is not in the snapshot: ask the unit, not the list.
            void RetireGoneMarks(Group* group, Player* human, Dir::GroupState& st)
            {
                auto gone = [&](uint64_t guid) {
                    Unit* u = guid ? ObjectAccessor::GetUnit(*human, ObjectGuid(guid)) : nullptr;
                    return guid && (!u || !u->IsAlive());
                };
                if (gone(st.skullGuid))
                {
                    if (group->GetTargetIcon(kSkullSlot).GetRawValue() == st.skullGuid) ClearMark(group, human, kSkullSlot);
                    st.skullGuid = 0;
                }
                if (gone(st.moonGuid))
                {
                    if (group->GetTargetIcon(kMoonSlot).GetRawValue() == st.moonGuid) ClearMark(group, human, kMoonSlot);
                    st.moonGuid = 0;
                }
            }

            // Compare the board with what we put on it, and let go of what is gone. A player taking one of OUR marks off or moving it is
            // counted (a mark that died first is not an override: the director let go of it itself).
            void ObserveMarks(Group* group, Player* human, Dir::GroupState& st, int64_t now, int64_t yieldMs)
            {
                RetireGoneMarks(group, human, st);
                const uint64_t skull = LiveMark(group, human, kSkullSlot);
                const uint64_t moon = LiveMark(group, human, kMoonSlot);
                if ((st.skullGuid && skull != st.skullGuid) || (st.moonGuid && moon != st.moonGuid))
                    ++s_overrides;
                Dir::NoteMarks(st, skull, moon, now, yieldMs);
            }

            struct Request
            {
                uint64_t                humanGuid = 0;
                uint64_t                groupGuid = 0;
                Dir::Snapshot           snap;
                std::vector<int>        ranked;
                Dir::Offer              offer;
                Dir::Tuning             tuning;
                nlohmann::json          state;
                nlohmann::json          questions;
                uint32_t                timeoutMs = 1500;
                int                     onFocus = 0;      // members already attacking the current focus when this was asked
                int                     attackers = 0;    // members attacking anything
            };

            Dir::Answers ParseAnswers(const Jev::Result& r)
            {
                Dir::Answers a;
                if (const Jev::Answer* f = r.Find("focus"))   { a.focus = f->choice;   a.focusConf = f->confidence; }
                if (const Jev::Answer* c = r.Find("cc"))      { a.cc = c->choice;      a.ccConf = c->confidence; }
                if (const Jev::Answer* p = r.Find("posture")) { a.posture = p->choice; a.postureConf = p->confidence; }
                return a;
            }

            // World thread: the model has answered (or failed). The answer is about a moment ago, so it is
            // re-resolved against the board as it is now before anything changes.
            nlohmann::json Apply(const Request& req, const Jev::Result& r)
            {
                auto fightIt = s_fights.find(req.groupGuid);
                if (fightIt == s_fights.end())
                    return nlohmann::json{{"skipped", "fight over"}};   // the fight ended while the model was thinking
                FightRecord& fight = fightIt->second;
                Dir::GroupState& st = fight.state;
                st.inFlight = false;

                if (!r.ok)
                {
                    LOG_DEBUG("server.loading", "[Ollama Chat Director] no answer: {}", r.error);
                    Remember({{"at", time(nullptr)}, {"error", r.error}, {"latency_ms", r.latencyMs}});
                    return nlohmann::json{{"ok", false}};
                }

                Player* human = ObjectAccessor::FindPlayer(ObjectGuid(req.humanGuid));
                Group* group = human && human->IsInWorld() ? human->GetGroup() : nullptr;
                if (!group || group->GetGUID().GetRawValue() != req.groupGuid)
                    return nlohmann::json{{"skipped", "group changed"}};

                ++s_totalConsults;
                const int64_t now = NowMs();
                Dir::Tuning tuning = req.tuning;
                if (r.minConfidence > 0.0f)
                    tuning.minConfidence = r.minConfidence;   // the bar of the generation that served this answer

                ObserveMarks(group, human, st, now, tuning.yieldMs);
                const Dir::Offer offer = Dir::WhatToAsk(req.snap, req.ranked, st, tuning, now);
                const Dir::Answers answers = ParseAnswers(r);
                Dir::Plan plan = Dir::Resolve(req.snap, req.ranked, offer, st, answers, tuning, now);

                // The enemy has to still be fighting: marking a corpse or a mob that evaded helps nobody.
                auto fighting = [&](uint64_t guid) {
                    Unit* u = ObjectAccessor::GetUnit(*human, ObjectGuid(guid));
                    return u && u->IsAlive() && u->IsInCombat();
                };
                if (plan.setFocus && !fighting(plan.focusGuid)) { plan.setFocus = false; plan.declined = "the enemy is no longer fighting"; }
                if (plan.setCc && !fighting(plan.ccGuid))       plan.setCc = false;

                if (plan.clearCc && st.moonGuid)
                {
                    if (group->GetTargetIcon(kMoonSlot).GetRawValue() == st.moonGuid) ClearMark(group, human, kMoonSlot);
                    st.moonGuid = 0;
                }
                if (plan.setFocus)
                {
                    SetMark(group, human, kSkullSlot, plan.focusGuid);
                    st.skullGuid = plan.focusGuid;
                    st.skullSetAtMs = now;
                    Announce(group, fight, now, "focus", plan.focusName, FocusLine(plan.focusName));
                }
                if (plan.setCc)
                {
                    SetMark(group, human, kMoonSlot, plan.ccGuid);
                    st.moonGuid = plan.ccGuid;
                    st.moonSetAtMs = now;
                    Announce(group, fight, now, "cc", plan.ccName, CcLine(plan.ccName));
                }
                if (plan.changePosture)
                {
                    st.posture = plan.posture;
                    ApplyPosture(group, fight, plan.posture);
                }

                nlohmann::json entry{
                    {"at", time(nullptr)}, {"latency_ms", r.latencyMs}, {"cost_usd", r.costUsd}, {"enemies", req.snap.enemies.size()},
                    {"focus_followed_by", req.onFocus}, {"attackers", req.attackers},
                    {"asked", {{"focus", req.offer.askFocus}, {"cc", req.offer.askCc}, {"posture", req.offer.askPosture}}},
                    {"said", {{"focus", answers.focus}, {"focus_conf", answers.focusConf}, {"cc", answers.cc}, {"cc_conf", answers.ccConf},
                              {"posture", answers.posture}, {"posture_conf", answers.postureConf}}},
                    {"applied", {{"focus", plan.setFocus ? plan.focusName : ""}, {"cc", plan.setCc ? plan.ccName : ""},
                                 {"release_cc", plan.clearCc}, {"posture", plan.changePosture ? Dir::PostureName(plan.posture) : ""}}},
                    {"declined", plan.declined}};
                Remember(entry);
                if (!plan.Empty())
                    LOG_INFO("server.loading", "[Ollama Chat Director] group {}: t+{:.1f}s focus={} cc={} release_cc={} posture={} ({} ms; {} of {} attackers were on the old focus)",
                             req.groupGuid, (now - fight.startedMs) / 1000.0, plan.setFocus ? plan.focusName : "-", plan.setCc ? plan.ccName : "-", plan.clearCc,
                             plan.changePosture ? Dir::PostureName(plan.posture) : "-", r.latencyMs, req.onFocus, req.attackers);
                else if (!plan.declined.empty() && plan.declined != "already the focus")
                    LOG_INFO("server.loading", "[Ollama Chat Director] group {}: t+{:.1f}s jev picked {} ({:.2f}) but it was not applied: {}",
                             req.groupGuid, (now - fight.startedMs) / 1000.0, answers.focus, answers.focusConf, plan.declined);
                else if (st.skullGuid)
                    LOG_INFO("server.loading", "[Ollama Chat Director] group {}: t+{:.1f}s keeping the focus, {} of {} attackers are on it ({} ms)",
                             req.groupGuid, (now - fight.startedMs) / 1000.0, req.onFocus, req.attackers, r.latencyMs);
                else
                    LOG_DEBUG("server.loading", "[Ollama Chat Director] group {}: no change ({} ms)", req.groupGuid, r.latencyMs);
                return entry;
            }

            // True while another call may be made: MaxCallsPerHour, rolling.
            bool UnderHourlyCap(int64_t now)
            {
                while (!s_callTimesMs.empty() && now - s_callTimesMs.front() > 3600 * 1000)
                    s_callTimesMs.pop_front();
                return s_callTimesMs.size() < Number("OllamaChat.Director.MaxCallsPerHour", 900);
            }

            void ConsultGroup(Player* human, Group* group, int64_t now)
            {
                const uint64_t groupGuid = group->GetGUID().GetRawValue();
                Dir::Snapshot snap;
                if (!Gather(human, group, snap))
                {
                    EndFight(groupGuid, group, human);
                    return;
                }
                auto open = s_fights.find(groupGuid);
                if (snap.enemies.empty())
                {
                    // Nothing is attacking us. A tick of that is not the end of a fight (an enemy that evades, one that is held, a gap between two
                    // pulls): the fight is over once it has stayed that way for a few seconds, and then the marks come off.
                    if (open == s_fights.end())
                        return;
                    if (!open->second.emptySinceMs)
                        open->second.emptySinceMs = now;
                    if (now - open->second.emptySinceMs >= kEndGraceMs)
                        EndFight(groupGuid, group, human);
                    return;
                }
                if (open != s_fights.end())
                    open->second.emptySinceMs = 0;

                const Dir::Tuning tuning = ReadTuning();
                if (!Dir::WorthConsulting(snap, tuning))
                    return;

                FightRecord& fight = s_fights[groupGuid];
                if (!fight.startedMs)
                {
                    fight.startedMs = now;
                    std::string party;   // who the director thinks it is directing: a role is the engine's call, and a surprise here explains a lot
                    for (const Dir::Member& m : snap.members)
                        party += fmt::format("{}{} {}/{}/hp{}%/power{}%", party.empty() ? "" : ", ", m.name, m.cls, Dir::RoleName(m.role), m.hpPct, m.powerPct);
                    LOG_INFO("server.loading", "[Ollama Chat Director] group {}: fight begins, party: {}", groupGuid, party);
                    {
                        std::lock_guard<std::mutex> lock(s_logMutex);
                        s_lastParty = party;
                    }
                    auto carried = s_carriedYield.find(groupGuid);
                    if (carried != s_carriedYield.end())
                    {
                        fight.state.skullYieldUntil = std::max(fight.state.skullYieldUntil, carried->second.skullUntil);
                        fight.state.moonYieldUntil = std::max(fight.state.moonYieldUntil, carried->second.moonUntil);
                        s_carriedYield.erase(carried);
                    }
                }
                Dir::GroupState& st = fight.state;
                if (st.inFlight && now - st.lastConsultMs > kStaleInFlightMs)
                    st.inFlight = false;

                ObserveMarks(group, human, st, now, tuning.yieldMs);
                if (Dir::CcNotHolding(snap, st, now, tuning.ccGraceMs))
                {
                    // A moon mark the party will not shoot past, on an enemy that is attacking: take it off, and never offer it again.
                    LOG_INFO("server.loading", "[Ollama Chat Director] group {}: t+{:.1f}s crowd control is not holding on {}, mark released", groupGuid,
                             (now - fight.startedMs) / 1000.0, snap.enemies[static_cast<size_t>(Dir::EnemyIndexForGuid(snap, st.moonGuid))].name);
                    ClearMark(group, human, kMoonSlot);
                    st.ccFailed.push_back(st.moonGuid);
                    st.moonGuid = 0;
                    ++s_ccReleased;
                }
                auto forcedAsk = s_forceCc.find(human->GetGUID().GetRawValue());
                if (forcedAsk != s_forceCc.end() && now >= forcedAsk->second)
                    s_forceCc.erase(forcedAsk);      // nobody to crowd-control within the wait: the request lapses
                else if (forcedAsk != s_forceCc.end())
                {
                    // Waits for a fight that has something to crowd-control (three enemies and a living bot that can), up to 30 s.
                    const std::vector<int> forcedRank = Dir::RankEnemies(snap);
                    const std::vector<int> forced = Dir::CcCandidates(snap, forcedRank, st.skullGuid, st.ccFailed);
                    if (!forced.empty())
                    {
                        s_forceCc.erase(forcedAsk);
                        const Dir::Enemy& e = snap.enemies[static_cast<size_t>(forced[0])];
                        LOG_INFO("server.loading", "[Ollama Chat Director] group {}: t+{:.1f}s crowd control forced by a test hook: moon on {}", groupGuid,
                                 (now - fight.startedMs) / 1000.0, e.name);
                        SetMark(group, human, kMoonSlot, e.guid);
                        st.moonGuid = e.guid;
                        st.moonSetAtMs = now;
                    }
                }
                if (s_forceConserve.erase(human->GetGUID().GetRawValue()))
                {
                    LOG_INFO("server.loading", "[Ollama Chat Director] group {}: conserve forced by a test hook", groupGuid);
                    st.posture = Dir::Posture::Conserve;
                    ApplyPosture(group, fight, Dir::Posture::Conserve);
                }
                if (!Dir::FocusAllowed(st, now))
                    ++s_yieldTicks;   // the player's mark: counted so a test can tell "stayed out of the way" from "was not looking"

                if (st.inFlight || now - st.lastConsultMs < static_cast<int64_t>(Number("OllamaChat.Director.IntervalMs", 2500)))
                    return;
                if (st.calls >= Number("OllamaChat.Director.MaxCallsPerFight", 30) || !UnderHourlyCap(now))
                    return;

                const std::vector<int> ranked = Dir::RankEnemies(snap);
                const Dir::Offer offer = Dir::WhatToAsk(snap, ranked, st, tuning, now);
                if (!offer.askFocus && !offer.askCc && !offer.askPosture)
                    return;

                auto req = std::make_shared<Request>();
                req->humanGuid = human->GetGUID().GetRawValue();
                req->groupGuid = groupGuid;
                req->state     = Dir::BuildState(snap, ranked, st, /*withScores=*/true);
                req->questions = Dir::BuildQuestions(snap, ranked, offer, st, Jev::QuestionTemplate(Jev::kSiteDirector));
                req->snap      = std::move(snap);
                req->ranked    = ranked;
                req->offer     = offer;
                req->tuning    = tuning;
                req->timeoutMs = Number("OllamaChat.Director.TimeoutMs", 1500);
                Dir::Followers(req->snap, st.skullGuid, req->onFocus, req->attackers);

                st.inFlight = true;
                st.lastConsultMs = now;
                ++st.calls;
                s_callTimesMs.push_back(now);

                // Jev::Decide is a blocking HTTP call: never on the world thread. The answer comes back through WorldTask.
                std::thread([req]() {
                    const Jev::Result r = Jev::Decide(req->state, req->questions, req->timeoutMs, Jev::kSiteDirector);
                    OllamaChat::WorldTask::Run([req, r]() { return Apply(*req, r); }, 5000);
                }).detach();
            }
        }

        void Tick(uint32_t diff)
        {
            if (!Number("OllamaChat.Director.Enable", 0))
                return;
            s_elapsedMs += diff;
            if (s_elapsedMs < 1000)
                return;
            s_elapsedMs = 0;
            if (!Jev::EnabledFor(Jev::kSiteDirector))
            {
                // Said once per switch-on: a director that is enabled but silent is a puzzle nobody should have to solve.
                static bool warned = false;
                if (!warned)
                {
                    LOG_WARN("server.loading", "[Ollama Chat Director] OllamaChat.Director.Enable = 1 but the jev director site is off: "
                             "needs OllamaChat.Jev.Enable = 1, OllamaChat.Jev.Director.Enable = 1 and a jev key. Doing nothing.");
                    warned = true;
                }
                return;
            }

            const int64_t now = NowMs();
            std::unordered_set<uint64_t> seen;
            for (auto const& entry : ObjectAccessor::GetPlayers())
            {
                Player* human = entry.second;
                if (!human || !human->IsInWorld() || !IsWhitelistedHumanPlayer(human, true))
                    continue;
                if (human->GetSession() && IsAccountOptedOut(human->GetSession()->GetAccountId()))
                    continue;   // `.ollama optout`: no bot behaviour directed at this player, the director included
                if (IsOffFor(human))
                    continue;   // `.ollama director off`
                Group* group = human->GetGroup();
                if (!group || !seen.insert(group->GetGUID().GetRawValue()).second)
                    continue;
                ConsultGroup(human, group, now);
            }

            // A group that vanished mid-fight (disbanded, everyone logged out) leaves a record nobody will end.
            for (auto it = s_fights.begin(); it != s_fights.end();)
            {
                if (!seen.count(it->first) && now - it->second.state.lastConsultMs > kForgetGroupMs)
                {
                    ReleaseStrayPosture(it->first, it->second);
                    DropRecord(it->second);
                    it = s_fights.erase(it);
                }
                else
                    ++it;
            }
            for (auto it = s_carriedYield.begin(); it != s_carriedYield.end();)
            {
                if (it->second.skullUntil <= now && it->second.moonUntil <= now)
                    it = s_carriedYield.erase(it);
                else
                    ++it;
            }
            s_openFights.store(static_cast<uint32_t>(s_fights.size()));
        }

        bool IsOffFor(Player* player)
        {
            return player && player->GetPlayerSetting(kOffSetting, 0).value != 0;
        }

        void ForceCcFor(Player* player)
        {
            if (player)
                s_forceCc[player->GetGUID().GetRawValue()] = NowMs() + 30000;
        }

        void ForceConserveFor(Player* player)
        {
            if (player)
                s_forceConserve.insert(player->GetGUID().GetRawValue());
        }

        void SetOffFor(Player* player, bool off)
        {
            if (player)
                player->UpdatePlayerSetting(kOffSetting, 0, off ? 1 : 0);
        }

        nlohmann::json Counters()
        {
            std::string party;
            {
                std::lock_guard<std::mutex> lock(s_logMutex);
                party = s_lastParty;
            }
            return nlohmann::json{{"last_party", party}, {"consults", s_totalConsults.load()}, {"yield_ticks", s_yieldTicks.load()}, {"overrides", s_overrides.load()}, {"cc_released", s_ccReleased.load()},
                                  {"conserving", s_conserving.load()}, {"posture_mismatches", s_postureMismatches.load()},
                                  {"fights_open", s_openFights.load()}};
        }

        nlohmann::json RecentDecisions(std::size_t max)
        {
            std::lock_guard<std::mutex> lock(s_logMutex);
            nlohmann::json out = nlohmann::json::array();
            const std::size_t start = s_recent.size() > max ? s_recent.size() - max : 0;
            for (std::size_t i = start; i < s_recent.size(); ++i)
                out.push_back(s_recent[i]);
            return out;
        }
    }
}
