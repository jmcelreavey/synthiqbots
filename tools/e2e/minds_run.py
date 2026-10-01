#!/usr/bin/env python3
"""End-to-end check of the SquidBots mind setup on a Conquest of Azeroth (or any AzerothCore) realm.

A clientless player logs in, whispers a real playerbot, and the run asserts on both sides: what the "player" received,
and what the server and the mind service recorded. It needs the worldserver (with mod-playerbots and this module), the
authserver, MySQL, and the mind service (`python run-mind.py` in squidbots-dashboard) with a model assigned.

    python3 tools/e2e/minds_run.py --account TESTER --password secret \
        --auth-port 13724 --characters-db coa_test_minds_characters --world-db coa_test_minds_world \
        --defaults-file ~/.config/coa-dev/client.cnf --mind-db ~/dev/squidbots-dashboard/mind.sqlite

Scenarios (a name after --only runs just those):
  S0  the baseline: run it with OllamaChat.Gateway.Enable = 0 (then `.ollama reload`). The bot is an ordinary
      playerbot and the mind service is never called
  S1  whisper a bot: it answers, in one plain line
  S2  it knows what it is: the answer matches its real class or level in the database (a tool call, not a guess)
  S3  memory: told something, it repeats it in a later conversation
  S4  identity: the mind service logged those turns against this player and this bot
  S5  "invite me": the bot puts the player in its group
One JSON line per scenario, then a summary; exit 0 when nothing failed.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sqlite3
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wowclient.auth import logon  # noqa: E402
from wowclient.world import CHAT_WHISPER, WorldClient, WorldError  # noqa: E402


def mysql(args, query: str) -> list[list[str]]:
    cmd = ["mysql", f"--defaults-extra-file={os.path.expanduser(args.defaults_file)}", "-N", "-B", "-e", query]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if out.returncode:
        raise RuntimeError(out.stderr.strip()[:300])
    return [line.split("\t") for line in out.stdout.splitlines() if line]


def console(args, command: str) -> str:
    """A GM command through the worldserver's remote console (Ra.Enable = 1)."""
    import socket
    with socket.create_connection((args.host, args.ra_port), timeout=10) as sock:
        sock.settimeout(4)
        def read(pattern, ms=8000):
            out, end = "", time.time() + ms / 1000
            while time.time() < end and not re.search(pattern, out):
                try:
                    out += sock.recv(65536).decode("utf-8", "replace")
                except OSError:
                    break
            return out
        read("Username:")
        sock.sendall((args.ra_user + "\r\n").encode())
        read("Password:")
        sock.sendall((args.ra_password + "\r\n").encode())
        if "AC>" not in read("AC>|failed|incorrect"):
            raise RuntimeError("console login failed")
        sock.sendall((command + "\r\n").encode())
        return re.sub(r"AC>\s*$", "", read(r"AC>\s*$", 15000)).strip()


class Run:
    def __init__(self, args):
        self.args = args
        self.results: list[dict] = []
        self.client: WorldClient | None = None

    def log(self, *parts):
        print("  ", *parts, file=sys.stderr, flush=True)

    # ---- setup ---------------------------------------------------------------------------------------
    def login(self):
        a = self.args
        key, realms = logon(a.host, a.auth_port, a.account, a.password)
        realm = realms[0]
        self.client = WorldClient(a.host, int(realm.address.split(":")[1]), a.account, key, realm_id=realm.id, log=self.log)
        chars = self.client.characters()
        if not chars:
            for race, cls in ((2, 1), (1, 1), (5, 4), (2, 3)):
                try:
                    self.client.create_character(a.character, race=race, cls=cls)
                    break
                except WorldError as error:
                    self.log("create", race, cls, "->", error)
            chars = self.client.characters()
        if not chars:
            raise RuntimeError("no character could be created for the test account")
        self.me = chars[0]
        self.client.enter_world(self.me)
        self.log("in world as", self.me.name, "guid", self.me.guid)
        # A group survives logout, and a player who is already grouped cannot be invited: start from none.
        self.client.leave_group()
        time.sleep(2.5)

    def pick_bot(self) -> tuple[int, str, int, int]:
        """(guid, name, class, level) of an online playerbot near nobody in particular."""
        rows = []
        deadline = time.time() + self.args.bot_wait
        while time.time() < deadline and not rows:
            # A name a player can whisper: letters, and Conquest of Azeroth's optional surname ("Kemornar Bot").
            rows = mysql(self.args, f"SELECT guid, name, class, level FROM {self.args.characters_db}.characters "
                                    f"WHERE online = 1 AND guid != {self.me.guid} AND name REGEXP '^[A-Za-z]{{2,12}}( [A-Za-z]{{2,12}})?$' "
                                    f"ORDER BY RAND() LIMIT 1")
            if not rows:
                time.sleep(10)
        if not rows:
            raise RuntimeError("no playerbot came online")
        guid, name, cls, level = rows[0]
        return int(guid), name, int(cls), int(level)

    # ---- helpers -------------------------------------------------------------------------------------
    def ask(self, bot: str, text: str, wait: float = 90.0) -> str | None:
        """Whisper the bot and return its first whispered reply (or None)."""
        # The module drops a message that comes sooner than Gateway.MinSecondsBetweenRequests after the last one
        # to the same bot (silently, by design), so leave a gap however that is set.
        time.sleep(max(0.0, self.args.gap - (time.time() - getattr(self, "last_ask", 0.0))))
        self.last_ask = time.time()
        t0 = time.time()
        self.client.whisper(bot, text)
        found = self.client.wait_until(
            lambda: next((c for c in self.client.chat_since(t0) if c.chat_type == CHAT_WHISPER and c.sender_name.lower() == bot.lower()), None),
            wait)
        return found.text if found else None

    def mind_rows(self, query: str, params=()) -> list[sqlite3.Row]:
        db = sqlite3.connect(os.path.expanduser(self.args.mind_db))
        db.row_factory = sqlite3.Row
        try:
            return db.execute(query, params).fetchall()
        finally:
            db.close()

    def record(self, name: str, status: str, **details):
        row = {"scenario": name, "status": status, **details}
        self.results.append(row)
        print(json.dumps(row), flush=True)

    # ---- scenarios -----------------------------------------------------------------------------------
    def s0_baseline_without_ai(self, bot):
        """Run with the gateway OFF: a whisper is left to playerbots, and the mind service is never called."""
        a = self.args
        before = self.mind_rows("SELECT COUNT(*) AS n FROM call_log")[0]["n"]
        console(a, "ollama reload")   # make sure the running config is what is on disk
        reply = self.ask(bot, "hello there", wait=12)
        after = self.mind_rows("SELECT COUNT(*) AS n FROM call_log")[0]["n"]
        self.record("S0", "PASS" if after == before else "FAIL", mind_calls=after - before, playerbots_reply=reply)

    def s1_answers(self, bot):
        reply = self.ask(bot, "hi! who are you and what are you up to?")
        ok = bool(reply) and len(reply) <= 255 and "\n" not in reply and "**" not in reply
        self.record("S1", "PASS" if ok else "FAIL", reply=reply)
        return reply

    def s2_knows_itself(self, bot, cls, level):
        names = {r[0]: r[1] for r in mysql(self.args, f"SELECT class, client_name FROM {self.args.world_db}.ascension_custom_class")} \
            if self.args.world_db else {}
        stock = {1: "warrior", 2: "paladin", 3: "hunter", 4: "rogue", 5: "priest", 6: "death knight", 7: "shaman", 8: "mage", 9: "warlock", 11: "druid"}
        cls_name = (names.get(str(cls)) or stock.get(cls) or "").lower()
        reply = self.ask(bot, "quick check: what class and level are you exactly?")
        low = (reply or "").lower()
        ok = bool(reply) and (str(level) in low or (cls_name and cls_name in low))
        self.record("S2", "PASS" if ok else "FAIL", expect={"class": cls_name, "level": level}, reply=reply)

    def s3_remembers(self, bot):
        self.ask(bot, "just so you remember: I main a paladin and I am farming Ulduar this week.")
        time.sleep(1.5)
        reply = self.ask(bot, "what class do I main, again?")
        ok = "paladin" in (reply or "").lower()
        self.record("S3", "PASS" if ok else "FAIL", reply=reply)

    def s4_identity(self, bot_guid):
        rows = self.mind_rows("SELECT player_guid, player_name, said, reply FROM turn_log WHERE bot_guid = ? AND lane = 'smart' ORDER BY id DESC LIMIT 10",
                              (bot_guid,))
        mine = [r for r in rows if r["player_guid"] == self.me.guid and r["player_name"].lower() == self.me.name.lower()]
        persona = self.mind_rows("SELECT archetype, source FROM persona WHERE bot_guid = ?", (bot_guid,))
        self.record("S4", "PASS" if mine and persona else "FAIL", turns_for_this_player=len(mine), of=len(rows),
                    persona=dict(persona[0]) if persona else None)

    def s5_invites_me(self, bot):
        t0 = time.time()
        reply = self.ask(bot, "could you invite me to your group, please?", wait=60)
        got = self.client.wait_until(lambda: self.client.events_since(t0, "group_invite") or self.client.events_since(t0, "group_list"), 30)
        if got and self.client.events_since(t0, "group_invite"):
            self.client.accept_invite()
            time.sleep(1.5)
            self.client.leave_group()          # leave the realm as it was found
        self.record("S5", "PASS" if got else "FAIL", reply=reply, saw="invite" if got else None)

    # ---- driver --------------------------------------------------------------------------------------
    def run(self) -> int:
        a = self.args
        self.login()
        guid, name, cls, level = self.pick_bot()
        self.log("bot:", name, "guid", guid, "class", cls, "level", level)
        wanted = set(a.only.split(",")) if a.only else None
        steps = [("S0", lambda: self.s0_baseline_without_ai(name)), ("S1", lambda: self.s1_answers(name)),
                 ("S2", lambda: self.s2_knows_itself(name, cls, level)), ("S3", lambda: self.s3_remembers(name)),
                 ("S4", lambda: self.s4_identity(guid)), ("S5", lambda: self.s5_invites_me(name))]
        for label, step in steps:
            if wanted and label not in wanted:
                continue
            try:
                step()
            except Exception as error:  # noqa: BLE001 - a scenario that blows up is a failed scenario
                self.record(label, "ERROR", error=f"{type(error).__name__}: {error}")
        failed = [r["scenario"] for r in self.results if r["status"] in ("FAIL", "ERROR")]
        print(json.dumps({"summary": "FAIL" if failed else "PASS", "failed": failed, "bot": name}), flush=True)
        try:
            self.client.close()
        except Exception:  # noqa: BLE001
            pass
        return 1 if failed else 0


def add_realm_args(p):
    """The options every run against a realm takes: where it is, which account logs in, and where its databases are."""
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--auth-port", type=int, default=3724)
    p.add_argument("--account", required=True)
    p.add_argument("--password", required=True)
    p.add_argument("--character", default="Mindprobe", help="created on first run if the account has none")
    p.add_argument("--characters-db", default="acore_characters")
    p.add_argument("--world-db", default="acore_world")
    p.add_argument("--defaults-file", default="~/.my.cnf", help="MySQL client option file")
    p.add_argument("--mind-db", default="mind.sqlite")
    p.add_argument("--ra-port", type=int, default=3443)
    p.add_argument("--ra-user", default="local")
    p.add_argument("--ra-password", default="local")
    p.add_argument("--bot-wait", type=int, default=480, help="seconds to wait for a playerbot to be online")
    p.add_argument("--gap", type=float, default=6.5, help="seconds between whispers (above MinSecondsBetweenRequests)")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_realm_args(p)
    p.add_argument("--only", default="")
    return Run(p.parse_args()).run()


if __name__ == "__main__":
    sys.exit(main())
