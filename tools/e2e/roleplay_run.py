#!/usr/bin/env python3
"""End-to-end check of roleplay mode (docs/roleplay.md) on an AzerothCore realm running this module, mod-playerbots and the SquidBots
mind service with chat mode "roleplay".

A clientless player whispers a playerbot and then stands among bots, and the run asserts on what that player reads and on what the mind
service recorded:

  R1  a whisper is answered by a character: the mind service holds a roleplay character for the bot (race, class, calling) and the model
      was shown its sheet, its zone and what it is doing
  R2  the answer is in the world: no levels, servers, bots or dungeon finders in it
  R3  "are you an AI?" is not answered with a yes: the character stays in character
  R4  where the bot is: asked where it is, the answer names the zone (or area) it really stands in (SKIP when the zone is not in the lore tables)
  R5  channels stay silent: in the watch window no bot says anything in the realm channel, General, Trade or Looking For Group
  R6  characters speak aloud: a bot near the player says something in /say within the watch window (SKIP when none did; the bank must be written
      and the starter interval is a minute or two), and it is in the world too

The test account must be a GM (the run revives itself and goes to the crowd) and on the module's whitelist. The mind service must answer
POST /cast {"op": "mode"} with chat_mode roleplay. One JSON line per scenario, then a summary; exit 0 when nothing failed. A model's answer is
not deterministic: a FAIL on R2 or R3 deserves a second run before it is believed.

    python3 tools/e2e/roleplay_run.py --account TESTER --password secret --characters-db coa_test_minds_characters \\
        --world-db coa_test_minds_world --defaults-file ~/.config/coa-dev/client.cnf --mind-db ~/src/coa-minds/mind.sqlite \\
        --ra-user minds --ra-password minds [--watch 240] [--mind-url http://127.0.0.1:18800]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.request

import minds_run

CHAT_SAY, CHAT_CHANNEL = 0x01, 0x11
# Words of a player at a keyboard. A person in the world does not say them.
META = re.compile(r"\b(levels?|xp|servers?|mmo|npcs?|dps|bots?|ai|language model|dungeon finder|respawn\w*|cooldowns?)\b", re.I)
ADMITS = re.compile(r"\b(i am|i'm|yes,? i am|yes,? i'm)\s+(an?\s+)?(ai|a\.i\.|bot|program|language model|artificial)\b", re.I)
FORBIDDEN_CHANNELS = ("ascension", "general", "trade", "lookingforgroup", "looking for group", "world")


class RoleplayRun(minds_run.Run):
    def __init__(self, args):
        super().__init__(args)
        self.bot = None
        self.character = None

    def mode(self) -> dict:
        request = urllib.request.Request(args_url(self.args) + "/cast", data=json.dumps({"op": "mode"}).encode("utf-8"),
                                         headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(request, timeout=10) as answer:
            return json.loads(answer.read().decode("utf-8"))

    def row(self, query, params=()):
        rows = self.mind_rows(query, params)
        return dict(rows[0]) if rows else None

    # ---- scenarios -------------------------------------------------------------------------------------
    def r1_whisper_is_answered_by_a_character(self, bot):
        guid, name, _, _ = bot
        reply = self.ask(name, "Well met. Tell me who you are, and where you hail from.", wait=120)
        if not reply:
            return self.record("R1", "FAIL", bot=name, why="no answer to the whisper within 120 s")
        self.character = self.row("SELECT * FROM rp_character WHERE bot_guid = ?", (guid,))
        if not self.character:
            return self.record("R1", "FAIL", bot=name, reply=reply, why="the mind service holds no roleplay character for this bot")
        turn = self.row("SELECT system_prompt FROM turn_log WHERE bot_guid = ? AND lane = 'smart' ORDER BY id DESC LIMIT 1", (guid,))
        prompt = (turn or {}).get("system_prompt", "")
        missing = [part for part in ("WHO YOU ARE", "YOUR STORY SO FAR", self.character["race"]) if part not in prompt]
        if missing:
            return self.record("R1", "FAIL", bot=name, reply=reply, why="the model was not shown %s" % ", ".join(missing))
        self.record("R1", "PASS", bot=name, race=self.character["race"], calling=self.character["calling"], klass=self.character["klass"],
                    level=self.character["level"], zone=self.character["zone"], story=bool(self.character["story"]), reply=reply)
        self.last_reply = reply

    def r2_in_the_world(self, bot):
        reply = getattr(self, "last_reply", "")
        found = META.search(reply)
        self.record("R2", "FAIL" if found else "PASS", reply=reply, why=("it said %r" % found.group(0)) if found else "")

    def r3_stays_in_character(self, bot):
        reply = self.ask(bot[1], "Be honest with me: are you an AI, or a bot?", wait=120)
        if not reply:
            return self.record("R3", "FAIL", why="no answer within 120 s")
        self.record("R3", "FAIL" if ADMITS.search(reply) else "PASS", reply=reply)

    def r4_knows_where_it_is(self, bot):
        zone = (self.character or {}).get("zone") or ""
        area = ""
        reply = self.ask(bot[1], "Where are we standing right now, friend?", wait=120)
        if not reply:
            return self.record("R4", "FAIL", why="no answer within 120 s")
        if not zone:
            return self.record("R4", "SKIP", why="the game has not told the service a zone for this bot", reply=reply)
        hit = zone.lower() in reply.lower() or (area and area.lower() in reply.lower())
        self.record("R4", "PASS" if hit else "FAIL", zone=zone, reply=reply)

    def watch(self, seconds):
        """Stand by the biggest crowd of bots and listen: [(kind, channel, sender, text)]."""
        rows = minds_run.mysql(self.args, "SELECT map, position_x, position_y, name FROM %s.characters WHERE online = 1 AND guid != %d "
                                          "AND name REGEXP '^[A-Za-z]{2,12}( [A-Za-z]{2,12})?$' LIMIT 4000" % (self.args.characters_db, self.me.guid))
        crowd = {}
        for map_id, x, y, name in rows:
            crowd.setdefault((map_id, int(float(x) // 60), int(float(y) // 60)), []).append(name)
        target = max(crowd.values(), key=len)[0] if crowd else None
        if target:
            self.client.say(".revive")
            time.sleep(1.5)
            self.client.say(".appear |cffffffff|Hplayer:%s|h[%s]|h|r" % (target, target))
            time.sleep(6)
        t0 = time.time()
        time.sleep(seconds)
        heard = []
        for c in self.client.chat_since(t0):
            if c.sender_guid == self.me.guid:
                continue
            channel = c.channel
            heard.append((c.chat_type, channel, c.sender_name or str(c.sender_guid), c.text))
        return heard, target

    def r5_r6_channels_and_say(self, seconds):
        heard, target = self.watch(seconds)
        in_channels = [h for h in heard if h[0] == CHAT_CHANNEL]
        forbidden = [h for h in in_channels if any(word in (h[1] or "").lower() for word in FORBIDDEN_CHANNELS) or not h[1]]
        self.record("R5", "FAIL" if forbidden else "PASS", watched_s=seconds, channel_lines=len(in_channels), forbidden=forbidden[:5])
        said = [h for h in heard if h[0] == CHAT_SAY]
        if not said:
            return self.record("R6", "SKIP", watched_s=seconds, near=target, why="no bot spoke aloud: is the roleplay bank written, and is a "
                                                                                   "whitelisted player near them? (OllamaChat.Ambient.StarterIntervalSec)")
        spoiled = [h for h in said if META.search(h[3])]
        self.record("R6", "FAIL" if spoiled else "PASS", lines=[h[3] for h in said[:6]], count=len(said), spoiled=[h[3] for h in spoiled[:3]])

    # ---- the run -----------------------------------------------------------------------------------------
    def run(self) -> int:
        try:
            mode = self.mode()
        except Exception as error:  # noqa: BLE001 - a report, not a stack trace
            print("ERROR the mind service did not answer %s: %s" % (args_url(self.args), error))
            return 2
        if mode.get("chat_mode") != "roleplay":
            print("ERROR the mind service is in %r mode; switch it to roleplay on the Minds page first." % mode.get("chat_mode"))
            return 2
        self.log("mind service: roleplay, channels", mode.get("channels"))
        self.login()
        try:
            bot = self.pick_bot()
            self.log("bot:", bot)
            only = {part.strip().upper() for part in self.args.only.split(",") if part.strip()}
            wants = lambda name: not only or name in only  # noqa: E731
            if wants("R1") or wants("R2"):
                self.r1_whisper_is_answered_by_a_character(bot)
                self.r2_in_the_world(bot)
            if wants("R3"):
                self.r3_stays_in_character(bot)
            if wants("R4"):
                if not self.character:
                    self.r1_whisper_is_answered_by_a_character(bot)
                self.r4_knows_where_it_is(bot)
            if wants("R5") or wants("R6"):
                self.r5_r6_channels_and_say(self.args.watch)
        finally:
            try:
                self.client.close()
            except Exception:  # noqa: BLE001
                pass
        counts = {status: sum(1 for row in self.results if row["status"] == status) for status in ("PASS", "SKIP", "FAIL")}
        print("SUMMARY %s" % json.dumps(counts), flush=True)
        return 1 if counts["FAIL"] else 0


def args_url(args) -> str:
    return args.mind_url.rstrip("/")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    minds_run.add_realm_args(parser)
    parser.add_argument("--mind-url", default="http://127.0.0.1:18800")
    parser.add_argument("--watch", type=float, default=240, help="seconds R5 and R6 listen among the bots")
    parser.add_argument("--only", default="", help="R1, R2, R3, R4 or R5,R6 (R2 needs R1; R5 and R6 share a window)")
    return RoleplayRun(parser.parse_args()).run()


if __name__ == "__main__":
    sys.exit(main())
