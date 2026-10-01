# Roleplay mode (module side)

With the mind service from [squidbots-dashboard](https://github.com/jmcelreavey/squidbots-dashboard) (`docs/roleplay.md` there) the bots can
be **characters in the lore** instead of fake players. Roleplay is the default. This page is what the module does for it; the characters,
the lore and the line bank live in the mind service.

## What the module does

1. **Follows a switch.** Every `OllamaChat.Roleplay.PollSeconds` (10) a worker thread asks the mind service `POST /cast {"op":"mode"}` and
   gets `{"chat_mode": "roleplay"|"players", "channels": [...]}`. The Minds page toggles it; no restart. Until the service has answered,
   and whenever it cannot be reached, roleplay is assumed (the safe side is a bot that says nothing in the realm channel). The conf can
   force a mode without asking: `OllamaChat.Roleplay.Mode = auto | roleplay | players`.
2. **Tells the service who each bot is.** Race, class, gender, level, zone, area, what the bot is doing and the titles of up to four open
   errands (`DescribeBotBrief`) go with every ambient request (answers, starters, welcomes, stock-line rewrites), with the director's
   party callouts, with the regulars sync, and, as one `roleplay_context={...}` JSON line in the `[BOT STATE SNAPSHOT]`, with every
   conversation. With these the service makes the character the first time it sees a bot and puts the zone and the errands in front of
   the model.
3. **Keeps the bots out of channels a character would not use.** In roleplay a bot speaks up by itself only in the channels the service
   lists (default `say,yell,guild`), intersected with `OllamaChat.Ambient.Channels`. The realm channel (Ascension), General, Trade and
   Looking For Group are silent, and so are the starters, the welcome and the answers to lines said there. Whispers, party chat and a
   player talking to a bot by name are answered in every channel, in character.
4. **Drops stock chatter in those channels.** Playerbots' own lines (`ai_playerbot_texts`: loot brags, LFG and Trade adverts, "Took
   [quest]") that a bot is about to send to a channel a character would not use are not said at all
   (`OllamaChat.Roleplay.DropStockLines`, on). In `say`, `yell` and guild they are rewritten in the character's own words as before. Only
   recognised stock lines are dropped: a line a player asked a bot to say, or one this module produced, always goes out. With the
   channel-filter patch (`patches/mod-playerbots-outgoing-channel-filter.patch`) this covers channel lines too; without it only `/say` and
   `/yell` stock lines pass through the module's hooks, so General and realm-channel chatter would still come from playerbots.
5. **Talks in `/say`.** A character near a whitelisted player says something aloud now and then (`OllamaChat.Roleplay.SayWeight`,
   `SayRangeYards` 30, `YellRangeYards` 90), picked from the bots within earshot; the bots around may answer, a few lines deep, then it
   goes quiet.

6. **Keeps a journal.** `OllamaChat::Roleplay::Note` (called from a `PlayerScript`) remembers each bot's last ten level-ups, new zones, finished errands, deaths,
   dungeon bosses, notorious kills and rare finds, and `DescribeBotBrief` sends them (with the time of day, the holidays being kept, and the
   titles and aims of its open errands) in the bot's context. The mind service stores them and writes the bot's chapters around them.
7. **Presence** (`src/mod-ollama-chat_roleplay.cpp`):
   - *Companions*: a bot in a party with a whitelisted player says a line in party chat about a new zone, a great foe, a fall, a find, a quiet
     road (`CompanionRemarks`, `CompanionIdleMinutes`, one per party per `CompanionGapSec`).
   - *Greetings*: a bot that knows a player says hello when they come within `ProximityGreetYards` (the mind decides who is a stranger).
   - *Emotes*: `/bow`, `/wave`, `/thank`, `/laugh` and the like, aimed at a bot or at nobody near one, are answered with an emote and, `EmoteSpeakPercent` of
     the time, a bank line.
   - *The people of the world*: an innkeeper, guard, trainer, vendor, banker, auctioneer, stable master, flight master or quest giver within
     `NpcRangeYards` answers a whitelisted player's `/say` when they are selected or named by name, title or role (`NpcReplies`); the game's
     own friendliness decides whether they are warm or curt.
   - *Sides*: a character does not answer, and does not speak up in `/say` for, a player of the other faction unless `AllowTwoSide.Interaction.Chat` is on.
   - *Speech, not typing*: `OllamaChat.Roleplay.Typing*` and `Jitter*` replace the ambient figures, and a bot spoken to by name makes a thinking gesture
     (`AckEmote`) while its answer is fetched.
8. **Calls a bot by its first name.** `Promotion::ShortName` is the first word of a two-word name, so "Elorin" reaches "Elorin Moonwhisper".

Players mode is untouched: with `chat_mode = players` or `OllamaChat.Roleplay.Mode = players` nothing above applies and the module behaves as
before.

## Keys

All in the *ROLEPLAY* section of `conf/mod_ollama_chat.conf.dist`: `OllamaChat.Roleplay.Mode`, `.Channels`, `.DropStockLines`, `.SayWeight`,
`.SayRangeYards`, `.YellRangeYards`, `.PollSeconds`, `.Typing*`, `.Jitter*`, `.AckEmote`, `.EmoteReplies`, `.EmoteNearbyPercent`, `.EmoteSpeakPercent`,
`.ProximityGreet*`, `.CompanionRemarks`, `.CompanionIdleMinutes`, `.CompanionGapSec`, `.CompanionBotGapSec`, `.NpcReplies`, `.NpcRangeYards`, `.NpcGapSec`.
`OllamaChat.Ambient.Channels` now lists `guild` by default.

## Things to know

- Roleplay needs the mind service. With it stopped (or `OllamaChat.Gateway.Enable = 0`) the module does nothing, as before.
- A mind service from before roleplay answers `unknown cast op "mode"`: the module then keeps assuming roleplay, which silences the
  realm channel but gets no characters. Update the service.
- With `AiPlayerbot.CoaBotSurname = 1` (the default) playerbots gives every bot a placeholder surname, " Bot". Two-word names are
  left alone, so renaming the bots to names that suit their race (`tools/rename_bots.py` in the dashboard repo, a dry run unless
  `--apply`) needs no change to playerbots' own settings.
- The zone's weather is not sent: the core keeps a zone's weather state private.
- `tools/e2e/roleplay_run.py` R7-R11 check the journal, an emote, an innkeeper, a greeting and a companion remark on a live realm.
- The `[BOT STATE SNAPSHOT]` is read on the thread that already read the bot's state for it; the errand titles are the quests' English
  names.
