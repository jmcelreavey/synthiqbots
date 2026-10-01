#!/usr/bin/env python3
"""End-to-end check of the combat director (docs/director.md) on an AzerothCore realm running this module and mod-playerbots.

A clientless player joins a party of playerbots, hostile mobs are spawned on top of the party, and the run asserts on what that
player sees: the raid marks the director puts up (MSG_RAID_TARGET_UPDATE), the party-chat line that announces the plan, and that the
marks come down again. In a second fight the player takes the skull off and the run checks the director leaves it off.

The test account must be a GM (the run revives itself, goes to the bots, makes itself invulnerable and spawns mobs with chat
commands). The realm needs OllamaChat.Director.Enable = 1, OllamaChat.Jev.Director.Enable = 1 and a jev key, and its remote console
on (Ra.Enable = 1): `ollama director status` tells the run whether the director is on and how many times it has looked.

    python3 tools/e2e/director_run.py --account TESTER --password secret --characters-db coa_test_minds_characters \\
        --world-db coa_test_minds_world --defaults-file ~/.config/coa-dev/client.cnf --ra-user minds --ra-password minds \\
        [--bots 4] [--mobs 4820,127,127,2351] [--fight-wait 60]

The options are minds_run.py's, plus --bots (party size), --mobs (creature_template entries to spawn; by default one elite and three
ordinary hostile mobs around the bots' level are picked from the world database) and --fight-wait.

Scenarios:
  D1  a fight starts: the director puts the skull on one of the mobs
  D2  a bot says the plan in party chat within a few seconds of it
  D3  the marks are gone when the fight is over
  D4  the player takes the skull off: the director leaves it off for the yield (SKIP when the fight ended before it could be seen)
  D5  `.ollama director off`: a fight puts up no mark and the director is not even consulted (the switch is put back afterwards)
  D6  the mana posture: forced to `conserve` the healers carry `save mana` (the engine agrees) and none is left on after the fight
One JSON line per scenario, then a summary; exit 0 when nothing failed. Every scenario is a model's decision about a real fight, so a
fight with nothing to choose between can SKIP, and a FAIL deserves a second run before it is believed.
"""
from __future__ import annotations

import argparse
import json
import random
import re
import sys
import time

import minds_run
from wowclient.world import CHAT_PARTY

SKULL = 7
YIELD_SEC = 20            # OllamaChat.Director.YieldSec
HORDE_RACES = (2, 5, 6, 8, 10)
ALLIANCE_RACES = (1, 3, 4, 7, 11)
HOSTILE_FACTIONS = "14, 16, 18, 22"
# Names that are not ordinary mobs on realms that ship placeholder or test creatures.
NOT_A_MOB = r"RPG|PH |Test|DND|Dummy|Trigger|Target|\\[|<"   # \\[ because MySQL unescapes the string once before the regex sees it


def rows_all(zones: dict) -> list:
    """Every candidate bot in every zone: a known healer need not be in the biggest crowd, since an invite works from anywhere."""
    return [bot for crowd in zones.values() for bot in crowd]


class DirectorRun(minds_run.Run):
    def __init__(self, args):
        super().__init__(args)
        self.bots: list[tuple[int, str, int, int]] = []
        self.known_healers: set[str] = set()   # bots the director has reported as healers in an earlier fight of this run

    # ---- the realm -------------------------------------------------------------------------------------
    def status(self) -> dict:
        """The director's switches and counters, from the worldserver console."""
        text = minds_run.console(self.args, "ollama director status")
        on = re.search(r"enable=(\w+), jev site effective=(\w+)", text)
        counters = re.search(r"consults answered=(\d+), yield ticks=(\d+), overrides=(\d+); fights open=(\d+), "
                             r"healers conserving=(\d+), posture mismatches=(\d+)", text)
        names = ("consults", "yield_ticks", "overrides", "fights_open", "conserving", "posture_mismatches")
        party = re.search(r"last fight's party: (.*)", text)
        if party:
            self.known_healers.update(re.findall(r"([A-Z][A-Za-z']+ Bot) [^,]*?/healer/", party.group(1)))
        out = {"on": bool(on) and on.group(1) == "true" and on.group(2) == "true", "text": text,
               "party": party.group(1).strip() if party else ""}
        out.update({name: int(counters.group(i + 1)) if counters else None for i, name in enumerate(names)})
        return out

    def pick_bots(self, shuffle: bool = False, want_healer: bool = False):
        """Up to --bots online playerbots of the test character's faction, in the same zone, preferring different classes.
        `shuffle` picks a different set (a retry: the first bots may have had no healer, say). `want_healer` brings along a bot the
        director reported as a healer earlier in this run, when one is online and free (healing is a spec: only the director knows)."""
        a = self.args
        race = int(minds_run.mysql(a, f"SELECT race FROM {a.characters_db}.characters WHERE guid = {self.me.guid}")[0][0])
        races = ", ".join(str(r) for r in (HORDE_RACES if race in HORDE_RACES else ALLIANCE_RACES))
        rows = minds_run.mysql(a, (
            f"SELECT c.guid, c.name, c.class, c.level, c.map, c.zone FROM {a.characters_db}.characters c "
            f"WHERE c.online = 1 AND c.guid != {self.me.guid} AND c.race IN ({races}) AND c.level >= 10 "
            f"AND c.guid NOT IN (SELECT memberGuid FROM {a.characters_db}.group_member)"))
        zones: dict[tuple, list] = {}
        for guid, name, cls, level, map_id, zone in rows:
            zones.setdefault((map_id, zone), []).append((int(guid), name, int(cls), int(level)))
        crowd = max(zones.values(), key=len, default=[])
        if shuffle:
            random.shuffle(crowd)
        chosen, seen = [], set()
        for bot in (crowd if shuffle else sorted(crowd, key=lambda b: b[2])):
            if bot[2] not in seen:
                chosen.append(bot)
                seen.add(bot[2])
        chosen += [b for b in crowd if b not in chosen]
        if want_healer:
            healer = next((b for b in rows_all(zones) if b[1] in self.known_healers), None)
            if healer:
                chosen = [healer] + [b for b in chosen if b != healer]
        self.bots = chosen[:a.bots]
        if len(self.bots) < 2:
            raise RuntimeError("fewer than two online bots of the player's faction share a zone")

    def pick_mobs(self) -> list[str]:
        a = self.args
        if a.mobs:
            return [m for m in a.mobs.split(",") if m]
        level = max(b[3] for b in self.bots)   # the strongest bot sets the pace: a fight over in 8 s gives the director no time
        pick = lambda rank, lo, hi, n, extra="": [r[0] for r in minds_run.mysql(a, (  # noqa: E731
            f"SELECT t.entry FROM {a.world_db}.creature_template t WHERE t.`rank` {rank} AND t.faction IN ({HOSTILE_FACTIONS}) "
            f"AND t.minlevel BETWEEN {lo} AND {hi} AND t.npcflag = 0 AND t.HealthModifier <= 4 AND t.name NOT REGEXP '{NOT_A_MOB}' "
            f"{extra} ORDER BY RAND() LIMIT {n}"))]
        # A fight with something to choose: one spellcaster among melee mobs. Identical mobs are a coin flip, where the right
        # answer is "leave it to the engine" and the director rightly marks nothing.
        casters = pick("IN (0, 1)", level - 8, level + 3, 1, f"AND t.unit_class IN (2, 8) AND EXISTS (SELECT 1 FROM {a.world_db}.creature_template_spell s "
                                                    f"WHERE s.CreatureID = t.entry)")
        melee = pick("= 0", level - 6, level, 3, "AND t.unit_class = 1")
        mobs = casters + melee
        if not casters or len(melee) < 2:
            raise RuntimeError("no suitable hostile mobs in the world database: pass --mobs")
        return mobs

    # ---- a fight -----------------------------------------------------------------------------------------
    def party_up(self):
        a = self.args
        c = self.client
        c.say(".ollama director on")        # a run that died in D5 must not leave the switch off
        c.say(".revive")
        c.leave_group()
        time.sleep(2)
        c.say(".appear %s" % self.bots[0][1])
        time.sleep(4)
        for _, name, _, _ in self.bots:
            c.invite(name)
            time.sleep(1.5)
        deadline = time.monotonic() + 30
        joined = set()
        while time.monotonic() < deadline:
            joined = {r[0] for r in minds_run.mysql(a, (
                f"SELECT c.name FROM {a.characters_db}.group_member gm JOIN {a.characters_db}.characters c ON c.guid = gm.memberGuid "
                f"WHERE gm.guid = (SELECT guid FROM {a.characters_db}.group_member WHERE memberGuid = {self.me.guid})"))}
            if all(b[1] in joined for b in self.bots):
                break
            time.sleep(2)
        if not any(b[1] in joined for b in self.bots):
            raise RuntimeError("no bot accepted the party invite")
        time.sleep(8)                       # bots run to their master
        c.say(".cheat god on")              # the player is a bystander: the bots fight, and bots assist a master in combat
        time.sleep(1)

    def spawn(self, mobs) -> float:
        t0 = time.monotonic()
        for entry in mobs:
            self.client.say(".npc add temp %s" % entry)
            time.sleep(0.6)
        return t0

    def first_skull(self, t0: float, timeout: float):
        return self.client.wait_until(lambda: next((e for e in self.client.events_since_mono(t0, "raid_target")
                                                    if e.data["slot"] == SKULL and e.data["target"]), None), timeout)

    def wait_for_skull(self, t0: float, before: dict) -> tuple:
        """(skull event or None, what happened). Stops early once the director has followed a fight and let go of it.
        'what' is 'skull', 'unsure' (it followed a fight and jev never was confident) or 'no fight' (it never saw one)."""
        deadline = t0 + self.args.fight_wait
        saw_fight = False
        while time.monotonic() < deadline:
            skull = self.first_skull(t0, 1.0)
            if skull:
                return skull, "skull"
            state = self.status()
            if state["fights_open"]:
                saw_fight = True
            elif saw_fight:
                break                       # the fight came and went
        gained = (self.status()["consults"] or 0) - (before["consults"] or 0)
        return None, ("unsure" if saw_fight or gained else "no fight")

    def skull_events(self, t0: float):
        return [e for e in self.client.events_since_mono(t0, "raid_target") if e.data["slot"] == SKULL]

    def end_fight(self):
        self.client.leave_group()
        time.sleep(1.5)

    # ---- scenarios -----------------------------------------------------------------------------------
    def d1_to_d3(self, mobs):
        """One fight: the skull goes up (D1), a bot announces it (D2), and the skull is gone afterwards (D3)."""
        before = self.status()
        t0 = self.spawn(mobs)
        skull, what = self.wait_for_skull(t0, before)
        if skull:
            self.record("D1", "PASS", mobs=mobs, seconds_to_skull=round(skull.mono - t0, 1), bots=[b[1] for b in self.bots])
        elif what == "unsure":
            # Not a failure of the director: it followed the fight and jev never made a call it was sure of (or the fight was too short).
            self.record("D1", "SKIP", why="the director followed the fight but jev was never confident enough to mark", mobs=mobs,
                        retry=True)
        else:
            self.record("D1", "FAIL", why="the director never saw a fight to follow (enemies not attacking the party?)", mobs=mobs)
        if not skull:
            self.record("D2", "SKIP", why="no skull")
            self.record("D3", "SKIP", why="no skull")
            return
        # a bot's own line, in party chat, a moment after the mark (the line is fetched from the mind service's bank)
        said = self.client.wait_until(lambda: next((c for c in self.client.chat_since_mono(skull.mono - 1)
                                                    if c.chat_type == CHAT_PARTY and c.sender_guid != self.me.guid), None), 8)
        self.record("D2", "PASS" if said else "FAIL", line=said.text if said else None, from_bot=said.sender_name if said else None)
        # The fight is over when the director has let go of it (a second or two after the last enemy dies): the last thing
        # that happened to the skull must then be a clear.
        deadline = time.monotonic() + self.args.fight_end_wait
        while time.monotonic() < deadline and self.status()["fights_open"] != 0:
            time.sleep(2)
        if self.status()["fights_open"] != 0:
            self.record("D3", "SKIP", why="the fight was still going after %d s" % self.args.fight_end_wait)
            return
        time.sleep(1)                       # the clear travels as a packet after the director's tick
        events = self.skull_events(t0)
        cleared = bool(events) and events[-1].data["target"] == 0
        self.record("D3", "PASS" if cleared else "FAIL", skull_events=len(events), last="cleared" if cleared else "still up")

    def d4_yield(self, mobs):
        """The player takes the skull off a second after it appears: it stays off for YieldSec."""
        before = self.status()
        t0 = self.spawn(mobs)
        skull, what = self.wait_for_skull(t0, before)
        if not skull:
            self.record("D4", "SKIP", why="the director put up no skull (%s)" % what, retry=True)
            return
        time.sleep(0.4)                     # quickly: a mark whose target dies first is already gone, and there is nothing to yield to
        self.client.set_raid_mark(SKULL, 0)
        cleared_at = time.monotonic()
        # Fresh enemies join, so the fight is still going when the yield runs out (and the director has to cope with new arrivals).
        self.spawn(mobs[:3])
        time.sleep(YIELD_SEC - 3)
        again = [e for e in self.skull_events(cleared_at) if e.data["target"] and e.mono - cleared_at < YIELD_SEC - 1]
        after = self.status()
        gained = (after["yield_ticks"] or 0) - (before["yield_ticks"] or 0)
        overridden = (after["overrides"] or 0) - (before["overrides"] or 0)
        if overridden <= 0:
            # The director never saw the player take its mark off: the marked enemy had died first (it lets go of its own dead marks),
            # so a new skull a moment later is the next consult doing its job, not a broken yield.
            self.record("D4", "SKIP", why="the marked enemy was already dead when the player cleared the skull", retry=True)
        elif again:
            self.record("D4", "FAIL", why="the director put the skull back inside the yield",
                        seconds_after_clear=round(again[0].mono - cleared_at, 1), overrides=overridden)
        elif gained <= 0:
            self.record("D4", "SKIP", why="the fight ended before the director had a tick in which to yield")
        else:
            self.record("D4", "PASS", yield_ticks=gained, seconds_checked=YIELD_SEC - 3)

    def d6_conserve(self, mobs):
        """The mana posture mechanism: with a healer in the party the director puts `save mana` on it, checks the engine took it, and
        takes it off again when the fight is over. (Jev's own choice of `conserve` is benched offline: forcing it with a GM command
        makes this independent of a model and of a healer happening to be low on mana.)"""
        c = self.client
        # Healers may already carry `save mana` (Conquest of Azeroth's do): the director then has nothing to add. Take it off first
        # (the master's `co -save mana` in party chat), so the add and the removal after the fight are what is under test.
        c.party("co -save mana")
        time.sleep(3)
        before = self.status()
        self.spawn(mobs)
        # The director says which party it is directing, with roles: without a healer there is nothing to put `save mana` on.
        began = time.monotonic() + 20
        while time.monotonic() < began and self.status()["party"] == before["party"]:
            time.sleep(1.5)
        party = self.status()["party"]
        if party == before["party"] or "/healer/" not in party:
            self.record("D6", "SKIP", why="no healer in this party" if party != before["party"] else "the director saw no fight", party=party,
                        retry=True)
            return
        c.say(".ollama director force conserve")
        carrying = 0
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not carrying:
            time.sleep(1.0)
            carrying = self.status()["conserving"] or 0
        if not carrying:
            # The fight may simply be over already (the forced posture is applied at the director's next look).
            self.record("D6", "SKIP", why="no healer carried `save mana` (the fight ended first?)", party=party, retry=True)
            return
        end = time.monotonic() + self.args.fight_end_wait
        while time.monotonic() < end and self.status()["fights_open"] != 0:
            time.sleep(2)
        time.sleep(2)
        after = self.status()
        leaked = after["conserving"] or 0
        mismatches = (after["posture_mismatches"] or 0) - (before["posture_mismatches"] or 0)
        ok = leaked == 0 and mismatches == 0 and after["fights_open"] == 0
        self.record("D6", "PASS" if ok else "FAIL", healers_carrying_save_mana=carrying, left_on_after_the_fight=leaked,
                    engine_disagreed=mismatches, fight_over=after["fights_open"] == 0)

    def d5_switch_off(self, mobs):
        """A player who ran `.ollama director off` is left alone: no marks, and no consults at all."""
        self.client.say(".ollama director off")
        time.sleep(1)
        try:
            before = self.status()
            t0 = self.spawn(mobs)
            time.sleep(self.args.off_watch)
            marks = [e for e in self.client.events_since_mono(t0, "raid_target") if e.data["target"]]
            after = self.status()
            consults = (after["consults"] or 0) - (before["consults"] or 0)
            self.record("D5", "PASS" if not marks and consults == 0 else "FAIL", marks=len(marks), consults=consults,
                        seconds_watched=self.args.off_watch)
        finally:
            self.client.say(".ollama director on")
            time.sleep(1)

    # ---- driver --------------------------------------------------------------------------------------
    def run(self) -> int:
        a = self.args
        try:
            state = self.status()
        except (OSError, RuntimeError) as error:
            print(json.dumps({"summary": "ERROR", "why": "the worldserver console is not reachable (Ra.Enable = 1?): %s" % error}), flush=True)
            return 1
        if not state["on"]:
            print(json.dumps({"summary": "SKIP", "why": "the director is off on this realm (OllamaChat.Director.Enable, "
                              "OllamaChat.Jev.Director.Enable, a jev key)", "status": state["text"]}), flush=True)
            return 0
        self.login()
        self.pick_bots()
        mobs = self.pick_mobs()
        self.log("bots:", ", ".join("%s (class %d, lvl %d)" % (b[1], b[2], b[3]) for b in self.bots), "| mobs:", ",".join(mobs))
        wanted = set(a.only.split(",")) if a.only else None
        fights = (("D1", {"D1", "D2", "D3"}, lambda: self.d1_to_d3(mobs)), ("D4", {"D4"}, lambda: self.d4_yield(mobs)),
                  ("D5", {"D5"}, lambda: self.d5_switch_off(mobs)), ("D6", {"D6"}, lambda: self.d6_conserve(mobs)))
        for label, covers, step in fights:
            if wanted and not wanted & covers:
                continue
            # A fight where jev was never sure is no verdict on the director: it gets another fight (new mobs) before it is a SKIP.
            for attempt in range(1, a.attempts + 1):
                mark = len(self.results)
                if label == "D6" and attempt == 1:
                    self.pick_bots(want_healer=True)     # a healer seen in an earlier fight of this run, if one is about
                    mobs = self.pick_mobs()
                try:
                    self.party_up()
                    step()
                except Exception as error:  # noqa: BLE001 - a scenario that blows up is a failed scenario
                    self.record(label, "ERROR", error=f"{type(error).__name__}: {error}")
                finally:
                    try:
                        self.end_fight()
                    except Exception:  # noqa: BLE001
                        pass
                if not any(r.get("retry") for r in self.results[mark:]) or attempt == a.attempts:
                    break
                self.log("no verdict; another fight with other bots (attempt %d of %d)" % (attempt + 1, a.attempts))
                self.pick_bots(shuffle=True, want_healer=label == "D6")
                mobs = self.pick_mobs()
        latest = {}                         # the last word on each scenario: a retried SKIP is superseded by what followed
        for r in self.results:
            latest[r["scenario"]] = r
        failed = [name for name, r in latest.items() if r["status"] in ("FAIL", "ERROR")]
        print(json.dumps({"summary": "FAIL" if failed else "PASS", "failed": failed,
                          "skipped": [name for name, r in latest.items() if r["status"] == "SKIP"]}), flush=True)
        try:
            self.client.close()
        except Exception:  # noqa: BLE001
            pass
        return 1 if failed else 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    minds_run.add_realm_args(p)
    p.add_argument("--bots", type=int, default=4, help="party size, the player not counted")
    p.add_argument("--mobs", default="", help="creature_template entries to spawn, comma separated (default: picked from the world database)")
    p.add_argument("--fight-wait", type=float, default=60, help="seconds to wait for the director's first mark in a fight")
    p.add_argument("--fight-end-wait", type=float, default=150, help="seconds to wait for the bots to win before D3 gives up")
    p.add_argument("--attempts", type=int, default=2, help="fights tried for a scenario whose first fight gave jev nothing to be sure of")
    p.add_argument("--off-watch", type=float, default=25, help="seconds D5 watches a fight for marks that must not come")
    p.add_argument("--only", default="", help="D1, D2, D3, D4, D5 or D6 (D1-D3 share a fight)")
    return DirectorRun(p.parse_args()).run()


if __name__ == "__main__":
    sys.exit(main())
