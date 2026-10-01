// Standalone g++ harness for the combat director's rules (mod-ollama-chat_director_core.h).
// The model only picks among what the rules offer and every answer is re-checked before it touches the
// game, so each rule below is something a wrong or greedy answer must not get past.
//
//   g++ -std=c++17 -I src -I deps tests/director/harness_director.cpp -o /tmp/hd && /tmp/hd

#include "mod-ollama-chat_director_core.h"

#include <cstdio>

using namespace Director;

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_failures; } } while (0)

static Member Bot(const char* name, const char* cls, Role role, int hp = 100, int power = 100, bool mana = false)
{
    Member m; m.guid = std::hash<std::string>{}(name); m.name = name; m.cls = cls; m.role = role;
    m.isBot = true; m.hpPct = hp; m.powerPct = power; m.usesMana = mana;
    const std::string c = cls;   // the stock classes playerbots crowd-controls with
    m.canCc = c == "mage" || c == "rogue" || c == "hunter" || c == "warlock" || c == "priest" || c == "druid";
    return m;
}
static Member Human(const char* name)
{
    Member m; m.guid = std::hash<std::string>{}(name); m.name = name; m.cls = "warrior"; m.role = Role::Human; return m;
}
static Enemy Mob(const char* name, int hp = 100, int victim = -1, const char* rank = "normal", const char* casting = "")
{
    Enemy e; e.guid = std::hash<std::string>{}(name) | 1; e.name = name; e.level = 18; e.hpPct = hp; e.targetMember = victim;
    e.rank = rank; e.casting = casting; return e;
}

// tank, healer, mage, human
static Snapshot Party()
{
    Snapshot s;
    s.members = {Bot("Tallestis", "warrior", Role::Tank), Bot("Mira", "priest", Role::Healer, 100, 100, true),
                 Bot("Zed", "mage", Role::Ranged, 100, 100, true), Human("Raz")};
    return s;
}

static Tuning T() { return Tuning{}; }

static void TestConsultGate()
{
    Snapshot s = Party();
    CHECK(!WorthConsulting(s, T()), "no enemies: nothing to ask");

    s.enemies = {Mob("Defias Thug")};
    CHECK(!WorthConsulting(s, T()), "one normal mob against a healthy party is the engine's");

    s.enemies.push_back(Mob("Defias Rogue"));
    CHECK(WorthConsulting(s, T()), "two mobs is a fight worth directing");

    s.enemies = {Mob("Hogger", 100, 0, "elite")};
    CHECK(WorthConsulting(s, T()), "a lone elite is worth directing");

    s.enemies = {Mob("Defias Thug")};
    s.members[1].hpPct = 30;
    CHECK(WorthConsulting(s, T()), "a hurt party member makes one mob worth a look");
    s.members[1].alive = false;
    CHECK(!WorthConsulting(s, T()), "a dead member's hp does not count");

    Snapshot noBots = Party();
    for (Member& m : noBots.members) m.isBot = false;
    noBots.enemies = {Mob("a"), Mob("b"), Mob("c")};
    CHECK(!WorthConsulting(noBots, T()), "no bot to direct, no call");
}

static void TestRanking()
{
    Snapshot s = Party();
    s.enemies = {Mob("Thug", 100, 0), Mob("Conjurer", 100, 1, "normal", "Frostbolt"), Mob("Brute", 100, 0), Mob("Rogue", 100, 2)};
    const std::vector<int> r = RankEnemies(s);
    CHECK(r.size() == 4, "all four are labelled");
    CHECK(s.enemies[static_cast<size_t>(r[0])].name == "Conjurer", "a caster hitting the healer is E1");
    CHECK(s.enemies[static_cast<size_t>(r[1])].name == "Rogue", "then the one on the mage");
    CHECK(EnemyIndexForLabel(r, "E1") == r[0], "E1 maps to the top-ranked");
    CHECK(EnemyIndexForLabel(r, "E9") == -1, "an unknown label maps to nothing");

    s.enemies.clear();
    for (int i = 0; i < 10; ++i) s.enemies.push_back(Mob(("m" + std::to_string(i)).c_str()));
    CHECK(RankEnemies(s).size() == kMaxEnemies, "capped at kMaxEnemies");

    Snapshot casters = Party();
    casters.enemies = {Mob("Brute", 100, 0), Mob("Shaman", 100, 0, "normal", "Lightning Bolt")};
    const std::vector<int> cr = RankEnemies(casters);
    CHECK(casters.enemies[static_cast<size_t>(cr[0])].name == "Shaman", "all else equal a caster goes before a brute");

    Snapshot fin = Party();
    fin.enemies = {Mob("Fresh", 100, 0), Mob("Nearly", 10, 0)};
    const std::vector<int> fr = RankEnemies(fin);
    CHECK(fin.enemies[static_cast<size_t>(fr[0])].name == "Nearly", "all else equal the nearly dead one goes first");
}

static void TestCcOffer()
{
    Snapshot s = Party();
    s.enemies = {Mob("a"), Mob("b")};
    std::vector<int> r = RankEnemies(s);
    CHECK(CcCandidates(s, r, 0).empty(), "two mobs is a fight, not a puzzle");

    s.enemies = {Mob("a"), Mob("b"), Mob("c", 100, -1, "boss")};
    r = RankEnemies(s);
    const std::vector<int> c = CcCandidates(s, r, s.enemies[0].guid);
    bool sawBoss = false, sawFocus = false;
    for (int i : c) { sawBoss |= s.enemies[static_cast<size_t>(i)].rank == "boss"; sawFocus |= s.enemies[static_cast<size_t>(i)].guid == s.enemies[0].guid; }
    CHECK(!sawBoss, "a boss is never offered for crowd control");
    CHECK(!sawFocus, "the focus is never offered for crowd control");
    CHECK(c.size() == 1, "only b remains");

    Snapshot noCc;
    noCc.members = {Bot("Tallestis", "warrior", Role::Tank), Bot("Mira", "paladin", Role::Healer), Human("Raz")};
    noCc.enemies = {Mob("a"), Mob("b"), Mob("c")};
    CHECK(CcCandidates(noCc, RankEnemies(noCc), 0).empty(), "no CC class in the party, no CC offer");

    Snapshot deadCc = Party();
    deadCc.members[2].alive = false;
    deadCc.members[1].alive = false;   // the priest can shackle; she is dead too
    deadCc.enemies = {Mob("a"), Mob("b"), Mob("c")};
    CHECK(CcCandidates(deadCc, RankEnemies(deadCc), 0).empty(), "a dead mage does not crowd-control");
}

static void TestHumanMarks()
{
    GroupState st;
    NoteMarks(st, 0, 0, 1000, 20000);
    CHECK(st.skullYieldUntil == 0 && st.moonYieldUntil == 0, "an empty board is nobody's");

    NoteMarks(st, 777, 0, 1000, 20000);
    CHECK(!FocusAllowed(st, 1001), "a skull the director did not place is the human's");
    CHECK(FocusAllowed(st, 21001), "and stays theirs only for the yield window");
    CHECK(CcAllowed(st, 1001), "the moon is untouched by a skull");

    GroupState own;
    own.skullGuid = 555;
    NoteMarks(own, 555, 0, 5000, 20000);
    CHECK(FocusAllowed(own, 5001) && own.skullGuid == 555, "our own mark on the board is not a yield");

    NoteMarks(own, 888, 0, 6000, 20000);
    CHECK(!FocusAllowed(own, 6001) && own.skullGuid == 0, "a human replacing our skull takes it, and we forget ours");

    GroupState cleared;
    cleared.skullGuid = 555;
    NoteMarks(cleared, 0, 0, 7000, 20000);
    CHECK(!FocusAllowed(cleared, 7001), "a human clearing our skull is a yield too");

    GroupState moon;
    NoteMarks(moon, 0, 999, 1000, 20000);
    CHECK(!CcAllowed(moon, 1001) && FocusAllowed(moon, 1001), "a human's moon blocks CC only");
}

static void TestWhatToAsk()
{
    Snapshot s = Party();
    s.enemies = {Mob("a"), Mob("b"), Mob("c")};
    const std::vector<int> r = RankEnemies(s);
    GroupState st;

    Offer o = WhatToAsk(s, r, st, T(), 1000);
    CHECK(o.askFocus && o.askCc, "three mobs and a mage: focus and CC are on the table");
    CHECK(!o.askPosture, "full mana: posture is not worth a question");

    s.members[1].powerPct = 20;
    CHECK(WhatToAsk(s, r, st, T(), 1000).askPosture, "a healer low on mana makes posture a question");
    s.members[1].manaSaving = true;
    CHECK(!WhatToAsk(s, r, st, T(), 1000).askPosture, "a healer that already saves mana: conserving changes nothing, so nobody is asked");
    s.members[1].manaSaving = false;
    s.members[1].powerPct = 100;
    st.posture = Posture::Conserve;
    CHECK(WhatToAsk(s, r, st, T(), 1000).askPosture, "while conserving, the question stays so it can be undone");

    GroupState yielded;
    yielded.skullYieldUntil = 5000;
    CHECK(!WhatToAsk(s, r, yielded, T(), 1000).askFocus, "the human's skull: no focus question");

    Snapshot one = Party();
    one.enemies = {Mob("Hogger", 100, 0, "elite")};
    CHECK(!WhatToAsk(one, RankEnemies(one), GroupState{}, T(), 1000).askFocus, "one enemy: nothing to choose between");
}

static void TestResolve()
{
    Snapshot s = Party();
    s.enemies = {Mob("Thug", 100, 0), Mob("Conjurer", 100, 1, "normal", "Frostbolt"), Mob("Rogue", 100, 2)};
    const std::vector<int> r = RankEnemies(s);
    const uint64_t conjurer = s.enemies[1].guid;
    GroupState st;
    Offer o = WhatToAsk(s, r, st, T(), 100000);

    Answers a; a.focus = "E1"; a.focusConf = 0.9f;
    Plan p = Resolve(s, r, o, st, a, T(), 100000);
    CHECK(p.setFocus && p.focusGuid == conjurer && p.focusName == "Conjurer", "a confident, valid focus is applied");

    a.focusConf = 0.4f;
    CHECK(Resolve(s, r, o, st, a, T(), 100000).Empty(), "under the confidence bar: nothing happens");
    a.focusConf = 0.9f;

    a.focus = "keep";
    CHECK(Resolve(s, r, o, st, a, T(), 100000).Empty(), "keep changes nothing");
    a.focus = "E9";
    CHECK(Resolve(s, r, o, st, a, T(), 100000).Empty(), "a label that does not exist changes nothing");
    a.focus = "tank";
    CHECK(Resolve(s, r, o, st, a, T(), 100000).Empty(), "a word that is not a label changes nothing");

    // the same pick again is not a change
    st.skullGuid = conjurer; st.skullSetAtMs = 1000;
    a.focus = "E1";
    CHECK(!Resolve(s, r, o, st, a, T(), 100000).setFocus, "re-picking the current focus is a no-op");

    // a declined pick says why (on a copy of the state: the checks below depend on `st`)
    GroupState why = st;
    why.skullGuid = s.enemies[static_cast<size_t>(r[0])].guid; why.skullSetAtMs = 99999;
    a.focus = "E2";
    CHECK(Resolve(s, r, o, why, a, T(), 100000).declined.find("held") != std::string::npos, "a pick refused for the hold says so");
    a.focus = "E1";
    CHECK(Resolve(s, r, o, why, a, T(), 100000).declined == "already the focus", "re-picking the focus says so");

    // hold time: a fresh focus may not flip
    a.focus = "E2";
    st.skullSetAtMs = 98000;
    CHECK(!Resolve(s, r, o, st, a, T(), 100000).setFocus, "the focus does not flip inside the hold time");
    CHECK(Resolve(s, r, o, st, a, T(), 105000).setFocus, "after the hold time it may");

    // finishing: a nearly dead focus is not abandoned
    s.enemies[1].hpPct = 20;
    st.skullSetAtMs = 1000;
    CHECK(!Resolve(s, r, o, st, a, T(), 100000).setFocus, "a focus at 20% is finished, not abandoned");
    s.enemies[1].hpPct = 100;

    // focusing the crowd-controlled mob releases its moon
    GroupState ccd;
    ccd.moonGuid = s.enemies[static_cast<size_t>(r[1])].guid;   // E2, the one the answer below picks
    Plan rel = Resolve(s, r, WhatToAsk(s, r, ccd, T(), 100000), ccd, a, T(), 100000);
    CHECK(rel.setFocus && rel.clearCc, "the focus landing on the crowd-controlled mob releases the CC");

    // the focus is gone from the board: the hold does not apply to nothing
    st.skullGuid = 12345; st.skullSetAtMs = 99999;
    CHECK(Resolve(s, r, o, st, a, T(), 100000).setFocus, "a focus that left the fight does not hold the next one");
}

static void TestResolveCc()
{
    Snapshot s = Party();
    s.enemies = {Mob("Conjurer", 100, 1, "normal", "Frostbolt"), Mob("Rogue", 100, 2), Mob("Thug", 100, 0)};
    const std::vector<int> r = RankEnemies(s);
    GroupState st;
    Offer o = WhatToAsk(s, r, st, T(), 100000);
    CHECK(o.askCc, "CC is offered");

    Answers a; a.cc = "E2"; a.ccConf = 0.9f;
    Plan p = Resolve(s, r, o, st, a, T(), 100000);
    CHECK(p.setCc && p.ccGuid == s.enemies[static_cast<size_t>(r[1])].guid, "a valid CC pick is applied");

    // never CC what this very plan is about to focus
    a.focus = "E2"; a.focusConf = 0.9f;
    p = Resolve(s, r, o, st, a, T(), 100000);
    CHECK(p.setFocus && !p.setCc, "CC and focus picking the same mob: the focus wins, no CC");

    // nor the current focus
    a.focus = "keep";
    st.skullGuid = s.enemies[static_cast<size_t>(r[1])].guid;
    Offer o2 = WhatToAsk(s, r, st, T(), 100000);
    p = Resolve(s, r, o2, st, a, T(), 100000);
    CHECK(!p.setCc, "the current focus is never crowd-controlled");

    // release
    GroupState held;
    held.moonGuid = s.enemies[2].guid;
    Offer o3 = WhatToAsk(s, r, held, T(), 100000);
    Answers rel; rel.cc = "none"; rel.ccConf = 0.9f;
    CHECK(Resolve(s, r, o3, held, rel, T(), 100000).clearCc, "'none' releases a mark we placed");
    GroupState bare;
    CHECK(!Resolve(s, r, WhatToAsk(s, r, bare, T(), 100000), bare, rel, T(), 100000).clearCc, "'none' with nothing placed clears nothing");

    // an answer to a question that was not asked never releases a mark either
    Offer notAsked;
    CHECK(!Resolve(s, r, notAsked, held, rel, T(), 100000).clearCc, "'none' to an unasked CC question releases nothing");

    // a boss or a 2-mob fight never reaches CC: the offer is what bounds the answer
    Snapshot two = Party();
    two.enemies = {Mob("a"), Mob("b")};
    const std::vector<int> r2 = RankEnemies(two);
    Answers sneaky; sneaky.cc = "E1"; sneaky.ccConf = 1.0f;
    CHECK(!Resolve(two, r2, WhatToAsk(two, r2, GroupState{}, T(), 100000), GroupState{}, sneaky, T(), 100000).setCc,
          "a CC answer to a question that was never asked is ignored");
}

static void TestResolvePosture()
{
    Snapshot s = Party();
    s.enemies = {Mob("a"), Mob("b")};
    s.members[1].powerPct = 15;
    const std::vector<int> r = RankEnemies(s);
    GroupState st;
    Offer o = WhatToAsk(s, r, st, T(), 100000);
    Answers a; a.posture = "conserve"; a.postureConf = 0.9f;
    Plan p = Resolve(s, r, o, st, a, T(), 100000);
    CHECK(p.changePosture && p.posture == Posture::Conserve, "conserve is applied when offered");

    st.posture = Posture::Conserve;
    CHECK(!Resolve(s, r, WhatToAsk(s, r, st, T(), 100000), st, a, T(), 100000).changePosture, "same posture again is a no-op");
    a.posture = "push";
    CHECK(Resolve(s, r, WhatToAsk(s, r, st, T(), 100000), st, a, T(), 100000).changePosture, "push undoes it");

    a.posture = "retreat";
    CHECK(!Resolve(s, r, WhatToAsk(s, r, st, T(), 100000), st, a, T(), 100000).changePosture, "an option that was never offered is ignored");

    s.members[1].powerPct = 100;
    GroupState fresh;
    a.posture = "conserve";
    CHECK(!Resolve(s, r, WhatToAsk(s, r, fresh, T(), 100000), fresh, a, T(), 100000).changePosture, "posture is not asked at full mana, so not applied");
}

static std::string LabelFor(const Snapshot& s, const std::vector<int>& ranked, const char* name)
{
    for (size_t i = 0; i < ranked.size(); ++i)
        if (s.enemies[static_cast<size_t>(ranked[i])].name == name) return EnemyLabel(i);
    return "?";
}

static void TestPlayersTarget()
{
    Snapshot s = Party();
    s.enemies = {Mob("Thug A", 100, 0), Mob("Shaman", 100, 0, "normal", "Lightning Bolt"), Mob("Thug C", 100, 0)};
    s.members[3].targetGuid = s.enemies[0].guid;   // Raz is hitting Thug A
    CHECK(HumanTargetEnemy(s) == 0, "the enemy the player is hitting is found");

    std::vector<int> r = RankEnemies(s);
    GroupState st;
    Offer o = WhatToAsk(s, r, st, T(), 100000);
    Answers a; a.focus = LabelFor(s, r, "Shaman"); a.focusConf = 0.9f;
    CHECK(!Resolve(s, r, o, st, a, T(), 100000).setFocus, "the party is not pulled off the player's target for a caster on the tank");
    CHECK(Resolve(s, r, o, st, a, T(), 100000).declined.find("the player is attacking Thug A") != std::string::npos, "and the log says whom the player is attacking");

    a.focus = LabelFor(s, r, "Thug A");
    CHECK(Resolve(s, r, o, st, a, T(), 100000).setFocus, "the player's own target may be the focus: the bots join in");

    // somebody in danger outranks what the player is doing
    s.members[1].hpPct = 25;
    s.enemies[1].targetMember = 1;   // the shaman is on the healer
    r = RankEnemies(s);
    o = WhatToAsk(s, r, st, T(), 100000);
    a.focus = LabelFor(s, r, "Shaman");
    CHECK(Resolve(s, r, o, st, a, T(), 100000).setFocus, "a hurt healer under attack outranks the player's target");

    // ... but a hurt member nobody is attacking does not
    s.enemies[1].targetMember = 0;
    r = RankEnemies(s);
    o = WhatToAsk(s, r, st, T(), 100000);
    a.focus = LabelFor(s, r, "Shaman");
    CHECK(!Resolve(s, r, o, st, a, T(), 100000).setFocus, "a hurt member nobody is attacking is not an emergency");

    // no player target: no restriction
    s.members[3].targetGuid = 0;
    CHECK(HumanTargetEnemy(s) == -1, "no player target is no restriction");
    CHECK(Resolve(s, r, o, st, a, T(), 100000).setFocus, "with the player on nothing the focus is free");
}

static void TestCcThatIsNotHolding()
{
    Snapshot s = Party();
    s.enemies = {Mob("Conjurer", 100, 1), Mob("Rogue", 100, 2), Mob("Thug", 100, 0)};
    GroupState st;
    st.moonGuid = s.enemies[1].guid;   // the Rogue carries our moon
    st.moonSetAtMs = 10000;

    // The Rogue is in the snapshot, i.e. attacking somebody: the engine will not shoot at a moon-marked unit, so this mark must go.
    CHECK(!CcNotHolding(s, st, 11000, 4000), "a cast gets its grace before 'it is still attacking' counts");
    CHECK(CcNotHolding(s, st, 14000, 4000), "after the grace an attacking enemy with our moon on it is not held");

    // A held enemy attacks nobody, so it is not in the snapshot.
    Snapshot held = s;
    held.enemies.erase(held.enemies.begin() + 1);
    CHECK(!CcNotHolding(held, st, 20000, 4000), "a crowd-controlled enemy is not in the snapshot and stays marked");

    GroupState none;
    CHECK(!CcNotHolding(s, none, 99999, 4000), "no moon of ours, nothing to release");

    // ... and once it failed it is never offered again
    GroupState failed;
    failed.ccFailed = {s.enemies[1].guid};
    const std::vector<int> r = RankEnemies(s);
    for (int i : CcCandidates(s, r, 0, failed.ccFailed))
        CHECK(s.enemies[static_cast<size_t>(i)].guid != s.enemies[1].guid, "an enemy whose crowd control failed is not offered again");
    CHECK(CcCandidates(s, r, 0, failed.ccFailed).size() == 2, "the others still are");
    Offer o = WhatToAsk(s, r, failed, T(), 100000);
    for (int i : o.ccCandidates)
        CHECK(s.enemies[static_cast<size_t>(i)].guid != s.enemies[1].guid, "WhatToAsk honours the failed list");
}

static void TestFollowers()
{
    Snapshot s = Party();
    s.enemies = {Mob("a"), Mob("b")};
    s.members[0].targetGuid = s.enemies[0].guid;
    s.members[2].targetGuid = s.enemies[0].guid;
    s.members[1].targetGuid = 0;   // the healer attacks nothing
    s.members[3].targetGuid = s.enemies[1].guid;
    int on = 0, all = 0;
    Followers(s, s.enemies[0].guid, on, all);
    CHECK(on == 2 && all == 3, "two of the three attackers are on the focus");
    s.members[2].alive = false;
    Followers(s, s.enemies[0].guid, on, all);
    CHECK(on == 1 && all == 2, "a dead member is not counted");

    const nlohmann::json state = BuildState(s, RankEnemies(s), GroupState{});
    bool sawAttacking = false;
    for (const auto& row : state["party"]) sawAttacking |= row.contains("attacking");
    CHECK(sawAttacking, "the state says what each member is attacking");
}

static void TestRequestShape()
{
    Snapshot s = Party();
    s.enemies = {Mob("Conjurer", 100, 1, "normal", "Frostbolt"), Mob("Rogue", 100, 2), Mob("Thug", 100, 0)};
    s.enemies[0].interruptible = true;
    const std::vector<int> r = RankEnemies(s);
    GroupState st;
    Offer o = WhatToAsk(s, r, st, T(), 100000);

    const nlohmann::json state = BuildState(s, r, st);
    CHECK(state["enemies"].size() == 3 && state["enemies"][0]["id"] == "E1", "state labels the enemies E1..");
    CHECK(state["party"].size() == 4, "state lists the whole party");
    CHECK(state["enemies"][0]["attacking"] == "Mira", "state says who each enemy is hitting");

    const nlohmann::json q = BuildQuestions(s, r, o, st, nlohmann::json(nullptr));
    CHECK(q.contains("focus") && q.contains("cc") && !q.contains("posture"), "only the asked questions are built");
    CHECK(q["focus"]["criteria"].contains("E1") && q["focus"]["criteria"].contains("E3") && q["focus"]["criteria"].contains("keep"),
          "focus options are the labels plus keep");
    CHECK(q["cc"]["criteria"].contains("none"), "cc always has a way to say no");
    CHECK(q["focus"]["criteria"]["E1"].get<std::string>().find("[interruptible]") != std::string::npos, "an interruptible cast is called out");

    Offer none;
    CHECK(BuildQuestions(s, r, none, st, nlohmann::json(nullptr)).empty(), "nothing offered, nothing asked");
}

int main()
{
    TestConsultGate();
    TestRanking();
    TestCcOffer();
    TestHumanMarks();
    TestWhatToAsk();
    TestResolve();
    TestResolveCc();
    TestResolvePosture();
    TestPlayersTarget();
    TestCcThatIsNotHolding();
    TestFollowers();
    TestRequestShape();
    if (g_failures) { std::printf("%d check(s) failed\n", g_failures); return 1; }
    std::printf("director harness: all checks passed\n");
    return 0;
}
