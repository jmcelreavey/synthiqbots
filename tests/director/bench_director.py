#!/usr/bin/env python3
"""Calibration bench for the combat director's jev questions.

Labelled fights (a sensible focus target for each, written by hand) are turned into the exact request the game sends, by the
real mod-ollama-chat_director_core.h (tests/director/dump_request.cpp), and posted to the Decisions API. The bench prints, for
each wording of the `focus` question, how often jev picks an acceptable answer and how confident it is, so
`OllamaChat.Jev.Director.MinConfidence` and the wording in prompts/jev_questions.json are chosen from numbers.

    g++ -std=c++17 -I src -I deps tests/director/dump_request.cpp -o /tmp/dump_request
    python3 tests/director/bench_director.py --key-file ~/src/coa-minds/typesafe.key [--variants current,hint] [--repeat 2]

A fight with no acceptable target listed ("any") checks the opposite failure: jev must NOT be confident about a coin flip.
Cost: ~1.1k input tokens a request at $0.042/M.
"""
from __future__ import annotations

import argparse
import copy
import json
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True   # importing bench_jev must not leave a __pycache__ in the repo
REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests" / "jev"))
import bench_jev  # noqa: E402  (its call() is the one the other sites are benched with)

QUESTIONS_FILE = REPO / "prompts" / "jev_questions.json"
DUMP = "/tmp/dump_request"


def member(name, role, cls, hp=100, power=100, target=None, **extra):
    return dict({"name": name, "role": role, "class": cls, "hp": hp, "power": power, "target": target or ""}, **extra)


def party(tank_target="", healer_hp=100, healer_power=70, mage_hp=100, rogue_hp=100, human_hp=100, mage_target="", rogue_target="",
          healer_target="", human_target=""):
    return [member("Tallestis", "tank", "warrior", 90, 40, tank_target),
            member("Mira", "healer", "priest", healer_hp, healer_power, healer_target),
            member("Zed", "ranged", "mage", mage_hp, 80, mage_target, can_cc=True),
            member("Rex", "melee", "rogue", rogue_hp, 100, rogue_target),
            member("Raz", "player", "warrior", human_hp, 60, human_target)]


def mob(name, hp=100, target="", rank="normal", casting="", interruptible=False, level=20):
    return {"name": name, "hp": hp, "target": target, "rank": rank, "casting": casting, "interruptible": interruptible, "level": level}


# (name, fight, acceptable focus answers: enemy names, "keep", or None for "no clear answer")
SCENARIOS = [
    ("caster hits the healer", dict(members=party("Thug A", healer_target=""),
        enemies=[mob("Thug A", 100, "Tallestis"), mob("Thug B", 100, "Tallestis"), mob("Conjurer", 100, "Mira", casting="Frostbolt", interruptible=True)]),
     {"Conjurer"}),
    ("nearly dead one first", dict(members=party("Thug A"),
        enemies=[mob("Thug A", 100, "Tallestis"), mob("Thug B", 8, "Tallestis")]),
     {"Thug B"}),
    ("a dying healer's attacker", dict(members=party("Thug A", healer_hp=22),
        enemies=[mob("Thug A", 100, "Tallestis"), mob("Brute", 100, "Mira")]),
     {"Brute"}),
    ("the mage is being chewed", dict(members=party("Thug A", mage_hp=38),
        enemies=[mob("Thug A", 100, "Tallestis"), mob("Shaman", 100, "Zed", casting="Lightning Bolt")]),
     {"Shaman"}),
    ("an enemy healer", dict(members=party("Brute"),
        enemies=[mob("Brute", 100, "Tallestis"), mob("Defias Priest", 100, "Tallestis", casting="Heal", interruptible=True)]),
     {"Defias Priest"}),
    ("the rogue is being chewed", dict(members=party("Thug A", rogue_hp=33),
        enemies=[mob("Thug A", 100, "Tallestis"), mob("Brute", 100, "Rex")]),
     {"Brute"}),
    ("the player is being chewed", dict(members=party("Bear", human_hp=30),
        enemies=[mob("Bear", 100, "Tallestis"), mob("Wolf", 100, "Raz")]),
     {"Wolf"}),
    ("adds before the boss", dict(members=party("Boss"),
        enemies=[mob("Boss", 100, "Tallestis", rank="boss", level=22), mob("Acolyte A", 100, "Mira", casting="Shadow Bolt"),
                 mob("Acolyte B", 100, "Zed", casting="Shadow Bolt")]),
     {"Acolyte A", "Acolyte B"}),
    ("a nearly dead normal beside a healthy elite", dict(members=party("Ogre"),
        enemies=[mob("Ogre", 100, "Tallestis", rank="elite", level=22), mob("Gnoll", 15, "Tallestis")]),
     {"Gnoll", "keep"}),
    ("keep a good focus", dict(members=party("Thug A"), state={"focus": "Conjurer"},
        enemies=[mob("Thug A", 100, "Tallestis"), mob("Conjurer", 70, "Mira", casting="Frostbolt", interruptible=True)]),
     {"keep", "Conjurer"}),
    ("drop a bad focus", dict(members=party("Thug A", healer_hp=30), state={"focus": "Thug A"},
        enemies=[mob("Thug A", 100, "Tallestis"), mob("Conjurer", 100, "Mira", casting="Frostbolt", interruptible=True)]),
     {"Conjurer"}),
    ("finish the focus", dict(members=party("Thug A"), state={"focus": "Thug A"},
        enemies=[mob("Thug A", 18, "Tallestis"), mob("Conjurer", 100, "Mira", casting="Frostbolt")]),
     {"keep", "Thug A"}),
    # The player is already hitting something. (Whether the party may be pulled off it is a rule in code, not in the wording:
    # asking the model to "respect the player" made it passive everywhere, 17 of 32 acted on instead of 24.)
    ("the player's target yields to a dying healer", dict(members=party("Thug A", healer_hp=25, human_target="Thug A"),
        enemies=[mob("Thug A", 100, "Tallestis"), mob("Brute", 100, "Mira")]),
     {"Brute"}),
    ("the player is on the nearly dead one", dict(members=party("Thug A", human_target="Thug B"),
        enemies=[mob("Thug A", 100, "Tallestis"), mob("Thug B", 10, "Tallestis"), mob("Thug C", 100, "Tallestis")]),
     {"Thug B", "keep"}),
    ("three the same", dict(members=party("Thug A"),
        enemies=[mob("Thug A", 100, "Tallestis"), mob("Thug B", 100, "Tallestis"), mob("Thug C", 100, "Tallestis")]),
     None),
    ("two the same, one tank", dict(members=party("Wolf A"),
        enemies=[mob("Wolf A", 100, "Tallestis"), mob("Wolf B", 100, "Tallestis")]),
     None),
    ("a caster on the tank", dict(members=party("Brute"),
        enemies=[mob("Brute", 100, "Tallestis"), mob("Shaman", 100, "Tallestis", casting="Lightning Bolt", interruptible=True)]),
     {"Shaman"}),
]

# Crowd control: a focus is already marked, the mage can sheep. Acceptable answers are enemy names or "none".
CC_SCENARIOS = [
    ("sheep the caster hitting the healer", dict(members=party("Brute"), state={"focus": "Brute"},
        enemies=[mob("Brute", 100, "Tallestis"), mob("Wolf", 100, "Tallestis"), mob("Shaman", 100, "Mira", casting="Lightning Bolt")]),
     {"Shaman"}),
    ("three the same: no need", dict(members=party("Thug A"), state={"focus": "Thug A"},
        enemies=[mob("Thug A", 100, "Tallestis"), mob("Thug B", 100, "Tallestis"), mob("Thug C", 100, "Tallestis")]),
     {"none"}),
    ("sheep what is chewing the mage", dict(members=party("Brute", mage_hp=40), state={"focus": "Brute"},
        enemies=[mob("Brute", 100, "Tallestis"), mob("Acolyte", 100, "Zed", casting="Shadow Bolt"), mob("Wolf", 100, "Tallestis")]),
     {"Acolyte"}),
    ("never the nearly dead one", dict(members=party("Conjurer"), state={"focus": "Conjurer"},
        enemies=[mob("Conjurer", 60, "Mira", casting="Frostbolt"), mob("Thug B", 10, "Tallestis"), mob("Wolf", 100, "Tallestis")]),
     {"none", "Wolf"}),
    ("a winning party needs none", dict(members=party("Thug A"), state={"focus": "Thug A"},
        enemies=[mob("Thug A", 30, "Tallestis"), mob("Rat", 20, "Tallestis"), mob("Rat B", 25, "Tallestis")]),
     {"none"}),
]

# Mana posture: asked when a healer is low on mana.
POSTURE_SCENARIOS = [
    ("low mana, a long fight ahead", dict(members=party("Brute", healer_power=25),
        enemies=[mob("Brute", 100, "Tallestis", rank="elite"), mob("Wolf", 100, "Tallestis"), mob("Wolf B", 100, "Tallestis")]),
     {"conserve"}),
    ("low mana, one mob nearly dead", dict(members=party("Thug A", healer_power=25),
        enemies=[mob("Thug A", 10, "Tallestis")]),
     {"push"}),
    ("low mana but the party is dying", dict(members=party("Brute", healer_power=25, healer_hp=30, mage_hp=35),
        enemies=[mob("Brute", 100, "Mira"), mob("Wolf", 100, "Zed")]),
     {"push", "conserve"}),
    ("mana at 55, two fresh enemies", dict(members=party("Brute", healer_power=55),
        enemies=[mob("Brute", 100, "Tallestis"), mob("Wolf", 100, "Tallestis")]),
     {"push", "conserve"}),
    ("low mana, three fresh elites", dict(members=party("Ogre A", healer_power=20),
        enemies=[mob("Ogre A", 100, "Tallestis", rank="elite"), mob("Ogre B", 100, "Tallestis", rank="elite"), mob("Ogre C", 100, "Tallestis", rank="elite")]),
     {"conserve"}),
]

FOCUS_HINT = ("Enemies are listed best-target-first by a simple score (casters, enemies attacking a healer or a hurt member, nearly dead "
              "ones). Answer E1 unless something in the snapshot gives a clear reason for another; answer `keep` only when the "
              "current focus is fine or nothing stands out.")


def variants(base: dict) -> dict[str, dict]:
    out = {"current": base}

    hint = copy.deepcopy(base)
    hint["director"]["focus"]["instructions"]["policy"].insert(0, FOCUS_HINT)
    out["hint"] = hint

    terse = copy.deepcopy(base)
    terse["director"]["focus"]["instructions"] = {
        "question": "Which enemy should this party kill first? The engine handles the fighting; you pick the target.",
        "policy": ["Priority: 1) an enemy attacking a healer, a caster or anyone below 40% hp; 2) a caster, especially with an "
                   "interruptible cast; 3) an enemy nearly dead; 4) anything else. Bosses and elites are not priority.",
                   "If no enemy stands out, or the current focus is fine, answer keep."]}
    out["terse"] = terse

    scored = copy.deepcopy(base)
    scored["director"]["focus"]["instructions"]["policy"].insert(
        1, "`priority` on each enemy is a score from simple rules (higher = more worth killing first). Use it to break ties, not to override a clear reason above.")
    out["scored"] = scored

    # crowd control / posture alternatives, tried on their own questions only
    cc_terse = copy.deepcopy(base)
    cc_terse["director"]["cc"]["instructions"] = {
        "question": "Should one enemy (not the focus) be crowd-controlled by the party's casters? Pick it, or `none`.",
        "policy": ["Pick an enemy that is dangerous and not being handled: a caster attacking a healer or a hurt member, or a hard "
                   "hitter that has not been picked up.",
                   "`none` when the enemies are all alike, weak, or nearly dead, or when the party is clearly winning. Never pick an "
                   "enemy that is nearly dead."]}
    out["cc_terse"] = cc_terse
    post_terse = copy.deepcopy(base)
    post_terse["director"]["posture"]["instructions"] = {
        "question": "A healer is low on mana. `conserve` makes healers ration mana for the rest of the fight; `push` heals normally.",
        "policy": ["conserve when several enemies, elites or a long fight remain and the party is not in danger right now.",
                   "push when the fight is nearly won, or when people are in danger and healing cannot wait."]}
    out["post_terse"] = post_terse
    return out


def dump(fight: dict, scores: bool = False) -> dict:
    p = subprocess.run([DUMP, str(QUESTIONS_FILE)] + (["--scores"] if scores else []), input=json.dumps(fight), capture_output=True, text=True)
    if p.returncode:
        raise SystemExit("dump_request failed: " + p.stderr[:300])
    return json.loads(p.stdout)


def with_wording(request: dict, wording: dict) -> dict:
    """The dump's focus question, re-built with another wording file (the options stay the dump's)."""
    q = copy.deepcopy(request["questions"])
    if "focus" in q:
        q["focus"]["instructions"] = wording["director"]["focus"]["instructions"]
    return q


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--key-file", required=True)
    ap.add_argument("--variants", default="current,hint,terse")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--others", action="store_true", help="also bench the crowd-control and mana-posture questions")
    ap.add_argument("--url", default=bench_jev.DEFAULT_URL)
    ap.add_argument("--model", default=bench_jev.DEFAULT_MODEL)
    args = ap.parse_args()

    raw = Path(args.key_file).read_text().strip()
    key = raw.split("=", 1)[1].strip() if "=" in raw.splitlines()[0] else raw
    wordings = variants(json.loads(QUESTIONS_FILE.read_text()))

    for name in [v for v in args.variants.split(",") if v]:
        rows = []
        scores = name.endswith("+scores")           # "current+scores": the same wording, the state carries the rules' priority per enemy
        wording = wordings[name.removesuffix("+scores")]
        for title, fight, good in SCENARIOS:
            req = dump(fight, scores)
            if not req["offer"]["focus"]:
                print(f"  ! {title}: focus not offered, skipped")
                continue
            for _ in range(args.repeat):
                resp, ms = bench_jev.call(args.url, key, args.model,
                                          req["state"], with_wording(req, wording), 10.0)
                ans = resp["answers"]["focus"]
                pick = req["labels"].get(ans["choice"], ans["choice"])   # label -> enemy name; "keep" stays
                rows.append({"title": title, "pick": pick, "conf": ans["confidence"], "good": good, "ms": ms,
                             "probs": {req["labels"].get(k, k): round(v, 2) for k, v in ans["probabilities"].items()}})
        report(name, rows)

    if args.others:
        w = variants(json.loads(QUESTIONS_FILE.read_text()))
        run_other("cc", CC_SCENARIOS, {"current": w["current"], "terse": w["cc_terse"]}, key, args)
        run_other("posture", POSTURE_SCENARIOS, {"current": w["current"], "terse": w["post_terse"]}, key, args)
    return 0


def best_enemy(probs: dict) -> tuple[str, float]:
    """The enemy with the highest probability and that probability (the `keep` option is not an enemy)."""
    enemies = {k: v for k, v in probs.items() if k != "keep"}
    name = max(enemies, key=enemies.get)
    return name, enemies[name]


def run_other(question: str, scenarios, wordings: dict, key: str, args) -> None:
    """cc / posture: per wording, the pick and its confidence per fight, and how many were acceptable."""
    for name, wording in wordings.items():
        print(f"\n=== {question} wording: {name}")
        right = total = 0
        for title, fight, good in scenarios:
            req = dump(fight)
            if not req["offer"][question]:
                print(f"  ! {title}: {question} not offered, skipped")
                continue
            q = copy.deepcopy(req["questions"])
            if "instructions" in wording["director"][question]:
                q[question]["instructions"] = wording["director"][question]["instructions"]
            resp, _ = bench_jev.call(args.url, key, args.model, req["state"], q, 10.0)
            ans = resp["answers"][question]
            pick = req["labels"].get(ans["choice"], ans["choice"])
            ok = pick in good
            right += ok
            total += 1
            print(f"  {'OK' if ok else 'XX'} {title:<40} -> {pick:<12} conf {ans['confidence']:.2f}  "
                  f"{ {req['labels'].get(k, k): round(v, 2) for k, v in ans['probabilities'].items()} }")
        print(f"  acceptable {right}/{total}")


def report(name: str, rows: list[dict]) -> None:
    print(f"\n=== wording: {name}")
    for r in rows:
        ok = "  " if r["good"] is None else ("OK" if r["pick"] in r["good"] else "XX")
        print(f"  {ok} {r['title']:<46} -> {r['pick']:<14} conf {r['conf']:.2f}  {r['probs']}")
    labelled = [r for r in rows if r["good"] is not None]
    blind = [r for r in rows if r["good"] is None]
    print(f"  argmax acceptable {sum(r['pick'] in r['good'] for r in labelled)}/{len(labelled)}")
    # Two ways to decide whether to act: the API's `confidence`, or the probability of the best enemy when it beats `keep`.
    for label, acts in (("confidence", lambda r, bar: r["pick"] != "keep" and r["conf"] >= bar),
                        ("probability", lambda r, bar: best_enemy(r["probs"])[1] >= bar
                         and best_enemy(r["probs"])[1] > r["probs"].get("keep", 0))):
        for bar in (0.4, 0.5, 0.6, 0.7):
            acted = [r for r in labelled if acts(r, bar)]
            right = sum(1 for r in acted if (r["pick"] if label == "confidence" else best_enemy(r["probs"])[0]) in r["good"])
            fooled = sum(1 for r in blind if acts(r, bar))
            print(f"  {label:<11} bar {bar:.1f}: acts on {len(acted):>2}/{len(labelled)} labelled fights, {right:>2} acceptable; "
                  f"acts on {fooled}/{len(blind)} coin-flip fights")


if __name__ == "__main__":
    raise SystemExit(main())
