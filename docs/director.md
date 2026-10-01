# Combat director

While a whitelisted player's party is fighting, the director asks [jev](jev.md) three questions and puts the
answers on the board the playerbots engine already reads. It never casts a spell, moves a bot or picks a bot's
target itself. Everything the engine does in a fight, it still does.

| Decision | What the director does | What the engine does with it |
|---|---|---|
| **Focus** — which enemy dies first | Raid mark **skull** on that enemy | `DpsTargetValue` returns the skull-marked unit before anything else, so DPS bots switch to it. Healers ignore it. |
| **Crowd control** — which enemy to lock down | Raid mark **moon** on that enemy | Bots leave a moon-marked unit alone (`FindNonCcTargetStrategy`); mages (polymorph), rogues (sap), hunters (freezing trap), warlocks (banish, fear) and druids (roots, hibernate, cyclone) cast their CC spell on it (`rti cc`). Placing the moon is verified live, the polymorph itself is not: see Limits. |
| **Mana posture** — should healers ration mana | `save mana` strategy on the healer bots | The strategy vetoes heals that would mostly overheal once mana is under `AiPlayerbot.SaveManaThreshold`. Only asked when it would change something: a healer that already carries `save mana` (Conquest of Azeroth's healers do) is left as it is and does not make the director ask. |

A bot says the plan in party chat, at most once every 8 s, in its own voice: the mind service has a line bank per personality for
the two calls (`combat_focus`, `combat_cc`, `{mob}` is the enemy's name: "focus Defias Conjurer, this fight used to be easier").
Without the service, or with nothing banked for that bot, the director says a plain line ("Kill the Defias Conjurer first.").

## When it runs

- `OllamaChat.Director.Enable = 1` **and** `OllamaChat.Jev.Director.Enable = 1` (plus the `Jev.*` connection settings).
- A whitelisted human (`IsWhitelistedHumanPlayer`, the same test the tactical layer uses) is in a party with at least one
  playerbot, and somebody in it is in combat.
- Parties only. Raids, battlegrounds and any fight with a player on the other side are left alone.
- Not for an account that ran `.ollama optout`, nor for a character that ran `.ollama director off` (`.ollama director on` undoes it;
  the bots keep talking, they just stop marking and calling targets). Remembered across logouts in the character's settings.
- Not every pull: a fight with fewer than `MinEnemies` (2) enemies is the engine's, unless an enemy is an elite, a rare
  or a boss, or somebody in the party is below half health.
- At most one consult every `IntervalMs` (2.5 s) per fight, `MaxCallsPerFight` (30) per fight and `MaxCallsPerHour`
  (900) across the realm. A request that has not come back in `TimeoutMs` (1.5 s) is dropped and the engine carries on.

## How it stays polite

The model picks among options; code decides what may be offered and what may be applied
(`src/mod-ollama-chat_director_core.h`, pinned by `tests/director/harness_director.cpp`).

- **The player's marks are theirs.** A skull or moon the director did not place (or one it placed that somebody
  moved or cleared) stays the player's for `YieldSec` (20 s), and the director does not ask about that mark meanwhile.
  A mark on a corpse is nobody's. The yield outlives the fight record: a fight is only over once no enemy has attacked the party for 4 s (a tick of
  that, an enemy that evades, a gap between pulls, is not the end), and a yield still running when a record does end is carried into the next one, so
  a quick restart of the same engagement cannot launder it. (Found by the e2e: the skull came back 8 s after the player took it off.)
- **What the player is hitting stays.** If the player is attacking an enemy, the focus does not move off it to another enemy
  unless a member under 40% health is being attacked. (The wording was tried too, "respect the player's target": it made the
  model passive everywhere, so it is a rule in code.)
- **No flip-flopping.** A new focus is kept for `FocusHoldSec` (6 s), and a focus under 30 % health is finished,
  not abandoned.
- **Crowd control is rare on purpose.** Offered only with three or more enemies and a living bot that can do it;
  never the focus, never a boss, never an enemy whose crowd control already failed this fight. "Can do it" means a stock class with a
  polymorph / sap / trap / banish / roots / hibernate spell (mage, rogue, hunter, warlock, druid): playerbots aims crowd control at the moon mark for
  those. Priests are left out: their only trigger is Shackle Undead, wasted on an ordinary mob. The Conquest of Azeroth class AI is spell-driven and does
  not read the mark, so on a CoA realm the CC question is never asked and only focus and mana posture apply.
- **The model cannot invent.** Options are labels (`E1`..`E6`, most worth killing first) built from the live snapshot.
  An unknown label, a pick under `Jev.Director.MinConfidence`, or an answer to a question that was not asked is ignored.
- **It cleans up.** When the fight ends the director removes the marks it placed and switches `save mana` back off
  on the healers it switched it on for (a strategy a bot already had is never removed). After every change it reads the bot's strategy list
  back and counts a disagreement (`posture mismatches`); `healers conserving` is how many it is still carrying, which must be 0 after a fight.
- **Answers are re-checked.** The model answers about a moment ago; before anything changes the board is read again
  and the picked enemy must still be alive and fighting.

## What the model sees

```json
{"party":   [{"name":"Tallestis","class":"warrior","role":"tank","alive":true,"hp":82,"power":40}, ...],
 "enemies": [{"id":"E1","name":"Defias Conjurer","level":18,"rank":"normal","hp":100,
              "priority":71,"casting":"Frostbolt","attacking":"Mira"}, ...],
 "enemy_count": 3}
```

The questions and their wording are in `prompts/jev_questions.json` under `director`. Enemies are ordered before
labelling (casters, enemies hitting a healer or a hurt member, nearly dead ones first) so `E1` is usually sensible and the
cap of six enemies drops the least interesting.

## How the wording was chosen

`prompts/jev_questions.json` (`director`) was tuned against `tests/director/bench_director.py`: 15 hand-labelled fights (a caster
on the healer, a nearly dead enemy, the mage being chewed, adds before a boss, three identical mobs, ...) are turned into the exact
request the game sends (`tests/director/dump_request.cpp` links the real `mod-ollama-chat_director_core.h`) and posted to jev. Each enemy
also carries `priority`, the rules' own score (casters, enemies hitting a healer or a hurt member, nearly dead ones), which the wording tells jev to
use as a tiebreaker: it lifted the share of fights jev is sure enough about from 33 to 42 of 45 at the 0.6 bar with the same accuracy. In other
words jev mostly confirms the rules' ranking and overrides it when something in the snapshot says otherwise; it never gets to act on a coin flip.
The first, descriptive wording picked a sensible enemy in 24 of 26 runs but was rarely *sure* (confidence is margin-like: probability
mass on `keep` drags it down), so at the 0.6 bar it acted on 9. An explicit priority order ("1) an enemy attacking a healer, a caster or
anyone below 40% hp; 2) a caster ...") gets the same accuracy and acts on 20-22 of 26 with no action at all on the coin-flip fights
(`keep` at 0.9). Re-run it after changing the wording or `MinConfidence`:

```bash
g++ -std=c++17 -I src -I deps tests/director/dump_request.cpp -o /tmp/dump_request
python3 tests/director/bench_director.py --key-file ~/src/coa-minds/typesafe.key --repeat 2 --others
```

~$0.001 a run. The labels are one person's judgement of "sensible", so read the table, not just the totals.

## Configuration

All `OllamaChat.Director.*` keys are read live (`config_reload` applies them). See `conf/mod_ollama_chat.conf.dist`.

| Key | Default | |
|---|---|---|
| `Director.Enable` | `0` | Master switch |
| `Jev.Director.Enable` / `.MinConfidence` | `0` / `0.6` | The jev site |
| `Director.IntervalMs` | `2500` | Min time between consults of one fight |
| `Director.MaxCallsPerFight` / `MaxCallsPerHour` | `30` / `900` | Spend caps |
| `Director.TimeoutMs` | `1500` | One jev request |
| `Director.MinEnemies` | `2` | Smaller pulls are the engine's |
| `Director.FocusHoldSec` / `YieldSec` | `6` / `20` | Flip hold, player-mark yield |
| `Director.CrowdControl` | `1` | `0` = never CC |
| `Director.Announce` | `1` | Party-chat line for a new focus / CC |
| `Director.CalloutTimeoutSeconds` | `3` | How long to wait for the bot's own-voice line before the plain one |

## Looking at it

- `.ollama director status` (GM or the worldserver console): the switches and the last eight consults — what jev said with its
  confidence, what was applied, how many of the party were already on the focus, how long it took and what it cost.
- `admin_director_state` (MCP, when the MCP server is on): the same ring, as JSON, up to 50 entries, and the counters.
- The status also carries counters since startup — `consults answered`, `yield ticks` (one-second ticks in which the focus question
  was withheld because the player owns the skull), `overrides` (times the director saw a player take one of its marks off or move it) and
  `fights open` (fights being followed now), `healers conserving` (healers carrying a `save mana` the director put there), `posture mismatches`
  (the engine disagreed with a strategy change) and `crowd control released` — which `tools/e2e/director_run.py` reads to tell "stayed out of the way"
  from "was not looking".
- `.ollama director force conserve` (administrators) is a test hook: at its next look the director puts `save mana` on the healers of your
  party as if jev had chosen `conserve`. `tools/e2e/director_run.py` D6 uses it (after taking `save mana` off with `co -save mana`) to prove
  the add, the engine's agreement and the removal after the fight without depending on a model or a healer being low on mana.
- `.ollama director force cc` (administrators) is the same kind of hook for crowd control: the director puts the moon on its best candidate
  (waiting up to 30 s for a fight with three enemies and a living bot that can crowd-control).
  To try crowd control on a Conquest of Azeroth realm: create a stock-class alt on the test account (the e2e client's `create_character`, a mage:
  Undead, class 8), log the GM in, `.playerbots bot add <alt>`, `.character level <alt> 20` and `.learn <spell id>` for its spells (116 Frostbolt,
  133 Fireball, 118 Polymorph) with the alt selected, then `invite` it and spawn mobs. `.list auras` on the moon-marked unit shows whether it was polymorphed.
- At the start of a fight the director logs the party it sees with each member's role, hp and power (`last fight's party` in the status): a role is
  the engine's call, and a surprise there (no healer where you expected one) explains a lot.
- A pick that was confident enough but not applied is logged with the reason (`jev picked E2 (0.74) but it was not applied: the player is
  attacking Thug A`) and shown under the consult in the status.
- The log: `[Ollama Chat Director] group <id>: t+6.1s focus=… cc=… posture=… (<ms> ms; 0 of 4 attackers were on the old focus)` for a change,
  `… keeping the focus, 3 of 4 attackers are on it` for a consult that changed nothing while a focus is set (this is how to see whether
  the bots really follow the mark), `… fight over after 32.7 s, 7 consults` when it ends. Each Jev call is also logged by the jev tier
  (`site=director focus=E1(0.80) …`) with latency, input tokens and an estimated cost (`Jev.InputPricePerMillionUsd`).

## Tests

`tools/e2e/director_run.py` is the live check (a party of bots, mobs spawned on it, the marks seen from the player's side; see
[e2e.md](e2e.md)). The rest needs no server:

```bash
tests/run_harnesses.sh                      # every standalone harness, jev's included
g++ -std=c++17 -I src -I deps tests/director/harness_director.cpp -o /tmp/hd && /tmp/hd   # just this one
```

The harness covers the consult gate, enemy ranking, who may be crowd-controlled and when a crowd-control mark is released, the player-mark yield, the
player's own target, the hold and finish rules, and that an unasked or low-confidence answer never reaches the game. Each rule was also checked by
breaking it and watching the harness fail.

## Limits

- It cannot make a bot do something the engine cannot (no CC spell in the party, no interrupt available).
- **Crowd control is the least exercised part.** What is known: the playerbots source makes the moon the default `rti cc` mark, and its crowd-control target
  chooser (`FindTargetForCcStrategy`) takes the moon-marked unit first whenever the bot can cast the spell on it; mage, warlock, rogue, hunter and druid
  have triggers that use it. On the CoA test realm, which has no stock-class bots, a stock mage alt was brought in as a playerbot (see below): with it in
  the party the director offered CC, jev chose a target, the moon went up, the bot said so, and a mark whose enemy was attacking anyway was released. What
  was seen on a later run, with hostile level-20 spiders and a slime instead of four mobs on top of the mage: the moon went up on a spider within two seconds, Frost Nova, then **Polymorph** on the moon-marked enemy (`.list auras`), the mark came down when the cast was done and the skull went on another enemy. (The first attempt, with beasts that never aggroed, saw no fight at all; `tools`-wise use hostile mobs of the party's level, as `director_run.py` picks.) Not every enemy can be crowd-controlled
  (polymorph skips undead and elementals, say) and the director does not check creature types. The engine's DPS and tank target choosers skip a
  moon-marked unit outright, so a mark that is not holding would leave the party unwilling to fight back at it: an enemy that is *attacking* while
  it carries the director's moon, once the cast has had 4 s, has its mark released at once and is never offered for CC again that fight
  (`crowd control released` in the status). A held enemy attacks nobody, so it is not in the snapshot; when the others are dead the fight is over from
  the director's side and its marks come off, so the party then goes for it.
- Focus works through the skull, which healers and tanks treat differently: a tank still holds what is on it and
  picks up the skull target only when it is hitting a non-tank (`TankTargetValue`).
- Raids (more than five members) are out of scope for now: several tanks, assignments and mechanics are a different problem.
- `MinConfidence` compares with jev's `confidence`, which is margin-like (the probability mass on `keep` lowers it): 0.6 means
  "a clear win", and an ambiguous fight is left to the engine on purpose.

## Checked live

On the Conquest of Azeroth test realm, a clientless player (`tools/e2e/`, GM, invulnerable) in a party of 1-4 bots with 3-5 hostile mobs spawned on it,
repeatedly (`director_run.py`, D1-D6): the first consult comes 1-4 s after the pull; a confident focus puts the skull on the enemy and a bot says so in its
own voice; at the next consult all of the attackers were on the marked enemy, then two of four (the tank holds what it holds); the focus does not flip;
with the player taking the skull off, the director does not place it again for the 20 s yield; the marks are gone after every fight; the player's off switch
is honoured and survives a logout; and a forced `conserve` puts `save mana` on a healer, the engine agrees, and it is off again afterwards. Counting
only fights where jev was sure, no run has failed since the yield carry-over was added; a fight where jev is not sure is a SKIP, by design. The
crowd-control path is checked live once, with a stock mage alt (see Limits): moon up, Polymorph on the marked enemy, moon released; one run is not a soak, and the other CC classes are checked only by the harness.
