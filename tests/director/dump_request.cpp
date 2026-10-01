// Reads one fight (JSON on stdin), prints the exact request the combat director would send to jev for it:
// the state, the questions (wording from prompts/jev_questions.json), which enemy got which label and what was offered.
// It is the real mod-ollama-chat_director_core.h, so bench_director.py measures what the game sends, not a copy of it.
//
//   g++ -std=c++17 -I src -I deps tests/director/dump_request.cpp -o /tmp/dump_request
//   echo '{"members":[...],"enemies":[...]}' | /tmp/dump_request prompts/jev_questions.json [--scores]
//
// members: [{name, class, role (tank|healer|melee|ranged|player), bot, alive, hp, power, mana, can_cc, target}]
// enemies: [{name, level, rank, hp, casting, interruptible, target}]     (target = the name of a member / an enemy)
// state:   {focus: enemy name already marked, cc: enemy name already crowd-controlled, posture: "conserve"} (optional)

#include "mod-ollama-chat_director_core.h"

#include <fstream>
#include <iostream>

using namespace Director;

static Role RoleFrom(const std::string& s)
{
    if (s == "tank") return Role::Tank;
    if (s == "healer") return Role::Healer;
    if (s == "ranged") return Role::Ranged;
    if (s == "player") return Role::Human;
    return Role::Melee;
}

int main(int argc, char** argv)
{
    nlohmann::json in = nlohmann::json::parse(std::cin);
    nlohmann::json tmpl;
    if (argc > 1)
    {
        std::ifstream f(argv[1]);
        nlohmann::json all = nlohmann::json::parse(f);
        if (all.contains("director")) tmpl = all["director"];
    }

    Snapshot s;
    std::map<std::string, int> memberIdx;
    for (const auto& m : in["members"])
    {
        Member d;
        d.name = m.value("name", "?");
        d.guid = 1000 + s.members.size();
        d.cls = m.value("class", "warrior");
        d.role = RoleFrom(m.value("role", "melee"));
        d.isBot = m.value("bot", d.role != Role::Human);
        d.alive = m.value("alive", true);
        d.hpPct = m.value("hp", 100);
        d.powerPct = m.value("power", 100);
        d.usesMana = m.value("mana", d.role == Role::Healer);
        d.canCc = m.value("can_cc", false);
        memberIdx[d.name] = static_cast<int>(s.members.size());
        s.members.push_back(d);
    }
    std::map<std::string, uint64_t> enemyGuid;
    for (const auto& e : in["enemies"])
    {
        Enemy d;
        d.name = e.value("name", "?");
        d.guid = 5000 + s.enemies.size();
        d.level = e.value("level", 20);
        d.rank = e.value("rank", "normal");
        d.hpPct = e.value("hp", 100);
        d.casting = e.value("casting", "");
        d.interruptible = e.value("interruptible", false);
        auto it = memberIdx.find(e.value("target", ""));
        d.targetMember = it == memberIdx.end() ? -1 : it->second;
        enemyGuid[d.name] = d.guid;
        s.enemies.push_back(d);
    }
    for (const auto& m : in["members"])
    {
        auto it = enemyGuid.find(m.value("target", ""));
        if (it != enemyGuid.end()) s.members[static_cast<size_t>(memberIdx[m.value("name", "?")])].targetGuid = it->second;
    }

    GroupState st;
    if (in.contains("state"))
    {
        auto g = [&](const char* key) -> uint64_t { auto it = enemyGuid.find(in["state"].value(key, "")); return it == enemyGuid.end() ? 0 : it->second; };
        st.skullGuid = g("focus");
        st.moonGuid = g("cc");
        if (in["state"].value("posture", "") == "conserve") st.posture = Posture::Conserve;
    }

    const Tuning t;
    const std::vector<int> ranked = RankEnemies(s);
    const Offer offer = WhatToAsk(s, ranked, st, t, 1000000);

    nlohmann::json labels = nlohmann::json::object();
    for (size_t i = 0; i < ranked.size(); ++i) labels[EnemyLabel(i)] = s.enemies[static_cast<size_t>(ranked[i])].name;

    nlohmann::json out{{"worth_consulting", WorthConsulting(s, t)},
                       {"offer", {{"focus", offer.askFocus}, {"cc", offer.askCc}, {"posture", offer.askPosture}}},
                       {"labels", labels},
                       {"state", BuildState(s, ranked, st, argc > 2 && std::string(argv[2]) == "--scores")},
                       {"questions", BuildQuestions(s, ranked, offer, st, tmpl)}};
    std::cout << out.dump() << std::endl;
    return 0;
}
