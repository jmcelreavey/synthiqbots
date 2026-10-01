#ifndef MOD_OLLAMA_CHAT_DIRECTOR_CORE_H
#define MOD_OLLAMA_CHAT_DIRECTOR_CORE_H

// Combat director — the pure half. No AzerothCore, no network, no clock: a bare
// g++ compiles this for tests/director/, the same split as mod-ollama-chat_jev_core.h.
//
// The director watches a party fight and makes three calls on top of the
// playerbots engine, which still owns every spell and every step:
//   focus   which enemy the DPS should burn (a skull mark: playerbots' dps target
//           returns the skull-marked unit first)
//   cc      which enemy to crowd-control (a moon mark: bots leave it alone and the
//           CC classes cast on it)
//   posture push (engine default) or conserve (healers save mana)
//
// "Rules cut, Choice picks": this file decides what may be asked and what may be
// applied (who is worth asking about, which options exist, when a human's mark
// wins, how fast the focus may flip). Jev only picks among what survives.

#include "mod-ollama-chat_jev_core.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace Director
{
    constexpr size_t kMaxEnemies = 6;   // labelled E1..E6; the rest are not worth a token

    enum class Role { Human, Tank, Healer, Melee, Ranged };
    enum class Posture { Push, Conserve };

    inline const char* RoleName(Role r)
    {
        switch (r)
        {
            case Role::Human:  return "player";
            case Role::Tank:   return "tank";
            case Role::Healer: return "healer";
            case Role::Melee:  return "melee";
            case Role::Ranged: return "ranged";
        }
        return "?";
    }

    inline const char* PostureName(Posture p) { return p == Posture::Conserve ? "conserve" : "push"; }

    struct Member
    {
        uint64_t    guid      = 0;
        std::string name;
        std::string cls;                 // "mage", "warrior", "Reaper" ... only ever shown to the model
        bool        canCc     = false;   // has a crowd-control spell playerbots aims at the moon mark
        Role        role      = Role::Melee;
        bool        isBot     = false;
        bool        alive     = true;
        int         hpPct     = 100;
        int         powerPct  = 100;     // mana / rage / energy
        bool        usesMana  = false;
        uint64_t    targetGuid = 0;      // what this member is attacking now (0 = nothing)
        bool        manaSaving = false;  // already has playerbots' `save mana` in combat (so conserving would change nothing)
    };

    struct Enemy
    {
        uint64_t    guid          = 0;
        std::string name;
        int         level         = 1;
        std::string rank          = "normal";   // normal | elite | rare | boss
        int         hpPct         = 100;
        std::string casting;                     // spell name, empty when not casting
        bool        interruptible = false;
        int         targetMember  = -1;          // index into Snapshot::members, -1 when unknown
    };

    struct Snapshot
    {
        std::vector<Member> members;
        std::vector<Enemy>  enemies;
    };

    // What the director itself put on the board, and the clocks that keep it polite.
    // Times are monotonic milliseconds supplied by the caller.
    struct GroupState
    {
        uint64_t skullGuid        = 0;   // the enemy WE marked for focus (0 = none)
        uint64_t moonGuid         = 0;   // the enemy WE marked for CC
        int64_t  moonSetAtMs      = 0;
        std::vector<uint64_t> ccFailed;   // enemies whose crowd control did not hold: never offered again this fight
        int64_t  skullSetAtMs     = 0;
        int64_t  skullYieldUntil  = 0;   // a human owns the skull until then
        int64_t  moonYieldUntil   = 0;
        Posture  posture          = Posture::Push;
        uint32_t calls            = 0;   // model calls this fight
        int64_t  lastConsultMs    = 0;
        int64_t  lastAnnounceMs   = 0;
        bool     inFlight         = false;
    };

    struct Tuning
    {
        float   minConfidence = 0.6f;
        int     minEnemies    = 2;      // a one-mob pull is not worth a call
        int     hurtPct       = 50;     // ... unless somebody is this low
        int64_t focusHoldMs   = 6000;   // the focus may not flip faster than this
        int     finishPct     = 30;     // a focus this low is finished, not abandoned
        int64_t yieldMs       = 20000;  // how long a human's mark stays theirs
        int     manaLowPct    = 60;     // a healer below this may save mana
        int     dangerPct     = 40;     // a member this low and under attack outranks what the player is hitting
        int64_t ccGraceMs     = 4000;   // time a crowd-control cast gets to land before "it is still attacking" means it did not
        bool    allowCc       = true;   // OllamaChat.Director.CrowdControl
    };

    // ---- who is worth asking about -------------------------------------------

    // How many living members are attacking `guid` right now, out of how many are attacking anything at all.
    inline void Followers(const Snapshot& s, uint64_t guid, int& onIt, int& fighting)
    {
        onIt = fighting = 0;
        for (const Member& m : s.members)
        {
            if (!m.alive || !m.targetGuid) continue;
            ++fighting;
            if (m.targetGuid == guid) ++onIt;
        }
    }

    inline bool AnyLiveBot(const Snapshot& s)
    {
        for (const Member& m : s.members)
            if (m.isBot && m.alive) return true;
        return false;
    }

    // A single normal mob against a healthy party is the engine's to handle.
    inline bool WorthConsulting(const Snapshot& s, const Tuning& t)
    {
        if (s.enemies.empty() || !AnyLiveBot(s)) return false;
        if (static_cast<int>(s.enemies.size()) >= t.minEnemies) return true;
        for (const Enemy& e : s.enemies)
            if (e.rank != "normal") return true;
        for (const Member& m : s.members)
            if (m.alive && m.hpPct < t.hurtPct) return true;
        return false;
    }

    // ---- which enemies get a label --------------------------------------------

    inline int EnemyPriority(const Snapshot& s, const Enemy& e)
    {
        int score = 0;
        if (!e.casting.empty()) score += e.interruptible ? 30 : 20;
        if (e.rank == "boss") score += 10;
        else if (e.rank != "normal") score += 8;
        if (e.targetMember >= 0 && static_cast<size_t>(e.targetMember) < s.members.size())
        {
            const Member& victim = s.members[static_cast<size_t>(e.targetMember)];
            switch (victim.role)
            {
                case Role::Healer: score += 40; break;
                case Role::Human:
                case Role::Ranged: score += 25; break;
                case Role::Melee:  score += 10; break;
                case Role::Tank:   break;
            }
            if (victim.hpPct < 40) score += 20;
        }
        score += (100 - std::max(0, std::min(100, e.hpPct))) / 5;   // finishing blows
        return score;
    }

    // Indexes into s.enemies, most worth killing first, at most kMaxEnemies. E1 is ranked[0].
    inline std::vector<int> RankEnemies(const Snapshot& s)
    {
        std::vector<int> idx;
        for (size_t i = 0; i < s.enemies.size(); ++i) idx.push_back(static_cast<int>(i));
        std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
            return EnemyPriority(s, s.enemies[static_cast<size_t>(a)]) > EnemyPriority(s, s.enemies[static_cast<size_t>(b)]);
        });
        if (idx.size() > kMaxEnemies) idx.resize(kMaxEnemies);
        return idx;
    }

    inline std::string EnemyLabel(size_t rankedPos) { return "E" + std::to_string(rankedPos + 1); }

    // Label -> index into s.enemies, or -1.
    inline int EnemyIndexForLabel(const std::vector<int>& ranked, const std::string& label)
    {
        for (size_t i = 0; i < ranked.size(); ++i)
            if (EnemyLabel(i) == label) return ranked[i];
        return -1;
    }

    inline int EnemyIndexForGuid(const Snapshot& s, uint64_t guid)
    {
        if (!guid) return -1;
        for (size_t i = 0; i < s.enemies.size(); ++i)
            if (s.enemies[i].guid == guid) return static_cast<int>(i);
        return -1;
    }

    // ---- the player's own target -----------------------------------------------

    // The enemy a human member is attacking right now (index into s.enemies), -1 when none.
    inline int HumanTargetEnemy(const Snapshot& s)
    {
        for (const Member& m : s.members)
            if (m.role == Role::Human && m.alive && m.targetGuid)
            {
                const int idx = EnemyIndexForGuid(s, m.targetGuid);
                if (idx >= 0) return idx;
            }
        return -1;
    }

    // A living member below `dangerPct` that some enemy is attacking.
    inline bool SomeoneInDanger(const Snapshot& s, int dangerPct)
    {
        for (const Enemy& e : s.enemies)
        {
            if (e.targetMember < 0 || static_cast<size_t>(e.targetMember) >= s.members.size()) continue;
            const Member& m = s.members[static_cast<size_t>(e.targetMember)];
            if (m.alive && m.hpPct < dangerPct) return true;
        }
        return false;
    }

    // ---- what may be offered --------------------------------------------------

    inline bool AnyLiveCcBot(const Snapshot& s)
    {
        for (const Member& m : s.members)
            if (m.isBot && m.alive && m.canCc) return true;
        return false;
    }

    // Crowd control needs three mobs or more (two is a fight, not a puzzle), a bot that can do it,
    // and a target that is not the focus, a boss, or an enemy it already failed on.
    inline std::vector<int> CcCandidates(const Snapshot& s, const std::vector<int>& ranked, uint64_t focusGuid,
                                         const std::vector<uint64_t>& failed = {})
    {
        std::vector<int> out;
        if (s.enemies.size() < 3 || !AnyLiveCcBot(s)) return out;
        for (int i : ranked)
        {
            const Enemy& e = s.enemies[static_cast<size_t>(i)];
            if (e.guid == focusGuid || e.rank == "boss") continue;
            if (std::find(failed.begin(), failed.end(), e.guid) != failed.end()) continue;
            out.push_back(i);
        }
        return out;
    }

    // The engine's DPS and tank target choosers skip a unit carrying the moon mark outright. So a moon mark on an enemy that is
    // attacking somebody (the crowd control never landed, or broke) leaves the party unwilling to fight back at it: it has to come
    // off. A crowd-controlled enemy is not attacking anybody, so it is not in the snapshot; being in it, once the cast had its
    // grace, means it is not held.
    inline bool CcNotHolding(const Snapshot& s, const GroupState& st, int64_t nowMs, int64_t graceMs)
    {
        return st.moonGuid && nowMs - st.moonSetAtMs >= graceMs && EnemyIndexForGuid(s, st.moonGuid) >= 0;
    }

    inline bool AnyHealerLowOnMana(const Snapshot& s, int manaLowPct)
    {
        for (const Member& m : s.members)
            if (m.isBot && m.alive && m.role == Role::Healer && m.usesMana && m.powerPct < manaLowPct && !m.manaSaving) return true;
        return false;
    }

    // ---- a human's mark is theirs ---------------------------------------------

    // Call every consult with what is on the board. A mark the director did not put there (or
    // one it put there that somebody moved or cleared) is a human's: hands off for yieldMs.
    inline void NoteMarks(GroupState& st, uint64_t skullOnBoard, uint64_t moonOnBoard, int64_t nowMs, int64_t yieldMs)
    {
        if (skullOnBoard != st.skullGuid)
        {
            st.skullGuid = 0;   // ours is gone or was replaced: not ours to clear any more
            st.skullYieldUntil = nowMs + yieldMs;
        }
        if (moonOnBoard != st.moonGuid)
        {
            st.moonGuid = 0;
            st.moonYieldUntil = nowMs + yieldMs;
        }
    }

    inline bool FocusAllowed(const GroupState& st, int64_t nowMs) { return nowMs >= st.skullYieldUntil; }
    inline bool CcAllowed(const GroupState& st, int64_t nowMs)    { return nowMs >= st.moonYieldUntil; }

    // ---- the request ----------------------------------------------------------

    struct Offer
    {
        bool askFocus   = false;
        bool askCc      = false;
        bool askPosture = false;
        std::vector<int> ccCandidates;   // indexes into s.enemies
    };

    inline Offer WhatToAsk(const Snapshot& s, const std::vector<int>& ranked, const GroupState& st, const Tuning& t, int64_t nowMs)
    {
        Offer o;
        o.askFocus = FocusAllowed(st, nowMs) && ranked.size() >= 2;   // one enemy: nothing to choose
        if (t.allowCc && CcAllowed(st, nowMs))
        {
            o.ccCandidates = CcCandidates(s, ranked, st.skullGuid, st.ccFailed);
            o.askCc = !o.ccCandidates.empty();
        }
        o.askPosture = st.posture == Posture::Conserve || AnyHealerLowOnMana(s, t.manaLowPct);
        return o;
    }

    inline std::string Describe(const Snapshot& s, const Enemy& e)
    {
        std::string d = e.name + " (lvl " + std::to_string(e.level);
        if (e.rank != "normal") d += " " + e.rank;
        d += ", " + std::to_string(e.hpPct) + "% hp)";
        if (!e.casting.empty()) d += std::string(", casting ") + e.casting + (e.interruptible ? " [interruptible]" : "");
        if (e.targetMember >= 0 && static_cast<size_t>(e.targetMember) < s.members.size())
        {
            const Member& m = s.members[static_cast<size_t>(e.targetMember)];
            d += ", attacking " + m.name + " (" + RoleName(m.role) + ", " + std::to_string(m.hpPct) + "% hp)";
        }
        return d;
    }

    inline nlohmann::json BuildState(const Snapshot& s, const std::vector<int>& ranked, const GroupState& st, bool withScores = false)
    {
        nlohmann::json party = nlohmann::json::array();
        for (const Member& m : s.members)
        {
            nlohmann::json row{{"name", m.name}, {"class", m.cls}, {"role", RoleName(m.role)},
                               {"alive", m.alive}, {"hp", m.hpPct}, {"power", m.powerPct}};
            // what they are hitting, in the enemies' own labels so "E2" means the same thing on both sides
            for (size_t i = 0; m.targetGuid && i < ranked.size(); ++i)
                if (s.enemies[static_cast<size_t>(ranked[i])].guid == m.targetGuid) row["attacking"] = EnemyLabel(i);
            party.push_back(row);
        }
        nlohmann::json enemies = nlohmann::json::array();
        for (size_t i = 0; i < ranked.size(); ++i)
        {
            const Enemy& e = s.enemies[static_cast<size_t>(ranked[i])];
            nlohmann::json row{{"id", EnemyLabel(i)}, {"name", e.name}, {"level", e.level}, {"rank", e.rank}, {"hp", e.hpPct}};
            if (withScores) row["priority"] = EnemyPriority(s, e);   // the rules' own score: how worth-killing they think it is
            if (!e.casting.empty()) row["casting"] = e.casting;
            if (e.targetMember >= 0 && static_cast<size_t>(e.targetMember) < s.members.size())
                row["attacking"] = s.members[static_cast<size_t>(e.targetMember)].name;
            if (e.guid == st.skullGuid) row["marked"] = "focus";
            else if (e.guid == st.moonGuid) row["marked"] = "crowd-controlled";
            enemies.push_back(row);
        }
        return {{"party", party}, {"enemies", enemies}, {"enemy_count", s.enemies.size()}};
    }

    // `tmpl` is the "director" object of prompts/jev_questions.json (may be null: built-in wording).
    inline nlohmann::json BuildQuestions(const Snapshot& s, const std::vector<int>& ranked, const Offer& o,
                                         const GroupState& st, const nlohmann::json& tmpl)
    {
        auto sub = [&](const char* id) -> nlohmann::json {
            return tmpl.is_object() && tmpl.contains(id) ? tmpl[id] : nlohmann::json(nullptr);
        };
        nlohmann::json q = nlohmann::json::object();
        if (o.askFocus)
        {
            nlohmann::json crit = nlohmann::json::object();
            for (size_t i = 0; i < ranked.size(); ++i)
                crit[EnemyLabel(i)] = Describe(s, s.enemies[static_cast<size_t>(ranked[i])]);
            if (EnemyIndexForGuid(s, st.skullGuid) >= 0) crit["keep"] = "Keep the current focus; it is the right one or nearly dead.";
            else                                        crit["keep"] = "Leave the engine's own target choice alone.";
            nlohmann::json t = sub("focus");
            q["focus"] = Jev::Choice(t.is_object() && t.contains("instructions") ? t["instructions"]
                                                                                  : nlohmann::json("Which enemy should the party kill first?"), crit);
        }
        if (o.askCc)
        {
            nlohmann::json crit = nlohmann::json::object();
            for (size_t i = 0; i < ranked.size(); ++i)
            {
                const int idx = ranked[i];
                if (std::find(o.ccCandidates.begin(), o.ccCandidates.end(), idx) == o.ccCandidates.end()) continue;
                crit[EnemyLabel(i)] = Describe(s, s.enemies[static_cast<size_t>(idx)]);
            }
            crit["none"] = EnemyIndexForGuid(s, st.moonGuid) >= 0 ? "Release the crowd-controlled enemy." : "Nothing needs crowd control.";
            nlohmann::json t = sub("cc");
            q["cc"] = Jev::Choice(t.is_object() && t.contains("instructions") ? t["instructions"]
                                                                               : nlohmann::json("Should one enemy be crowd-controlled?"), crit);
        }
        if (o.askPosture)
        {
            nlohmann::json t = sub("posture");
            q["posture"] = Jev::ChoiceFromTemplate(t, std::vector<std::string>{"push", "conserve"});
        }
        return q;
    }

    // ---- what the model said, and what actually happens -----------------------

    struct Answers
    {
        std::string focus;   float focusConf   = 0;
        std::string cc;      float ccConf      = 0;
        std::string posture; float postureConf = 0;
    };

    struct Plan
    {
        bool        setFocus = false;
        uint64_t    focusGuid = 0;
        std::string focusName;
        bool        setCc = false;
        bool        clearCc = false;
        uint64_t    ccGuid = 0;
        std::string ccName;
        bool        changePosture = false;
        Posture     posture = Posture::Push;
        std::string declined;   // why a pick that was confident enough was NOT applied ("" when nothing was declined): for the log

        bool Empty() const { return !setFocus && !setCc && !clearCc && !changePosture; }
    };

    // Turns answers into changes. Everything the model says is checked again here: a label that does
    // not exist, a confidence under the bar, a flip inside the hold time, a focus about to die, a mark
    // a human owns — none of them get through.
    inline Plan Resolve(const Snapshot& s, const std::vector<int>& ranked, const Offer& o, const GroupState& st,
                        const Answers& a, const Tuning& t, int64_t nowMs)
    {
        Plan p;

        if (o.askFocus && !a.focus.empty() && a.focus != "keep" && a.focusConf >= t.minConfidence)
        {
            const int idx = EnemyIndexForLabel(ranked, a.focus);
            if (idx >= 0)
            {
                const Enemy& pick = s.enemies[static_cast<size_t>(idx)];
                const int cur = EnemyIndexForGuid(s, st.skullGuid);
                const bool finishing = cur >= 0 && s.enemies[static_cast<size_t>(cur)].hpPct <= t.finishPct;
                const bool held      = cur >= 0 && nowMs - st.skullSetAtMs < t.focusHoldMs;
                // The player is already hitting something: the party is not pulled off it for a marginal gain, only for somebody in danger.
                const int playersTarget = HumanTargetEnemy(s);
                const bool pullsOffPlayer = playersTarget >= 0 && idx != playersTarget && !SomeoneInDanger(s, t.dangerPct);
                if (pick.guid == st.skullGuid)   p.declined = "already the focus";
                else if (pullsOffPlayer)         p.declined = "the player is attacking " + s.enemies[static_cast<size_t>(playersTarget)].name;
                else if (finishing)              p.declined = "finishing the current focus";
                else if (held)                   p.declined = "the current focus is being held";
                if (p.declined.empty())
                {
                    p.setFocus = true;
                    p.focusGuid = pick.guid;
                    p.focusName = pick.name;
                    // One icon per unit: marking the crowd-controlled mob for the kill takes its moon off (the core
                    // does the same), so the director's own record has to let go of it too.
                    if (pick.guid == st.moonGuid) p.clearCc = true;
                }
            }
        }

        if (o.askCc && !a.cc.empty() && a.ccConf >= t.minConfidence)
        {
            if (a.cc == "none")
            {
                if (st.moonGuid) p.clearCc = true;
            }
            else
            {
                const int idx = EnemyIndexForLabel(ranked, a.cc);
                const bool offered = idx >= 0 && std::find(o.ccCandidates.begin(), o.ccCandidates.end(), idx) != o.ccCandidates.end();
                // Never CC what we are about to kill (a focus picked in this very plan included).
                if (offered && s.enemies[static_cast<size_t>(idx)].guid != st.moonGuid
                    && s.enemies[static_cast<size_t>(idx)].guid != (p.setFocus ? p.focusGuid : st.skullGuid))
                {
                    p.setCc = true;
                    p.ccGuid = s.enemies[static_cast<size_t>(idx)].guid;
                    p.ccName = s.enemies[static_cast<size_t>(idx)].name;
                }
            }
        }

        const bool postureWord = a.posture == "push" || a.posture == "conserve";
        if (o.askPosture && postureWord && a.postureConf >= t.minConfidence)
        {
            const Posture want = a.posture == "conserve" ? Posture::Conserve : Posture::Push;
            if (want != st.posture)
            {
                p.changePosture = true;
                p.posture = want;
            }
        }
        return p;
    }
}

#endif // MOD_OLLAMA_CHAT_DIRECTOR_CORE_H
