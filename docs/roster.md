# The roster: the same bots every time

With `AiPlayerbot.DisabledWithoutRealPlayer = 1` the realm's random bots log out five minutes after the last real player leaves and log back
in a few seconds after the next one arrives. You want the same bots back: the ones with stories, friends and a guild.

## What playerbots does

Which random bots are in the world is stored in playerbots' own database, one `add` row each (`playerbots_random_bots`, owner 0,
valid for `AiPlayerbot.PermanentlyInWorldTime`, a year).

- **Resting and waking** leaves those rows alone. The same bots come back (checked on the test realm: 195 bots in, all gone 340 s after the
  player left, the same ones back).
- **A worldserver restart** does not. `RandomPlayerbotMgr::Init()` runs `DELETE FROM playerbots_random_bots WHERE event = 'add'` at every start,
  and the first player to log in finds an empty roster and playerbots draws a new one at random. On the test realm a restart brought back 99 of the
  200 bots that had been online before it.

## What this module does

`OllamaChat.Roster.KeepAcrossRestarts` (on by default) keeps a copy of the roster under its own event name, `ollama_roster`, which `Init()` leaves
alone, every `OllamaChat.Roster.SaveIntervalSec` seconds, and puts the `add` rows back from it during startup, before the first update tick
reads them (`mod-ollama-chat_roster.cpp`). Nothing is saved while the roster is empty, so a realm that was up with nobody on does not
overwrite the last roster with nothing. If `MaxRandomBots` is raised, playerbots fills the extra places at random as before; if it is lowered,
it uses the first ones.

It does nothing with `AiPlayerbot.EnablePeriodicOnlineOffline = 1`, which rotates the bots on purpose.

## Keeping chosen bots offline

`tools/roster.py` in the dashboard repo (stop the worldserver first):

    python3 tools/roster.py --defaults-file ~/.config/coa-dev/client.cnf --characters-db coa_test_minds_characters \
        --playerbots-db coa_test_minds_playerbots list --names
    python3 tools/roster.py ... bench "Aelindor Wyndsong"      # out of the roster, and never picked to fill a place
    python3 tools/roster.py ... unbench "Aelindor Wyndsong"    # may be picked again (the next free place can pick them)

Benching writes a `logout` row valid for a year. Playerbots already skips a bot with one when it picks bots to log in, and the module's restore
skips them too.

Only the random-bot pool is covered. A bot a player adds to a party by hand, or one called into a battleground queue, is a different login.
