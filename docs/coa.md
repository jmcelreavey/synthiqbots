# Conquest of Azeroth (SquidBots branch `coa`)

This branch runs the module on a Conquest of Azeroth realm built from
[jealous-sound/azerothcore-wotlk-coa](https://github.com/jealous-sound/azerothcore-wotlk-coa) with
[Zyth45's mod-playerbots](https://github.com/Zyth45/mod-playerbots/tree/coa), and hands the thinking to the
SquidBots **mind service** in [squidbots-dashboard](https://github.com/jmcelreavey/squidbots-dashboard) (personalities,
memory, which model answers, spend, a page to watch and steer it). Without the service it still works with any
OpenAI-compatible endpoint set as `OllamaChat.Gateway.Url`.

## Install

```bash
cd azerothcore-wotlk-coa/modules
git clone --branch coa https://github.com/Zyth45/mod-playerbots.git mod-playerbots       # if you do not have it
git clone --branch coa https://github.com/jmcelreavey/synthiqbots.git mod-ollama-chat    # the folder name matters
```

Rebuild (re-run CMake so it finds the module), copy `conf/mod_ollama_chat.conf.dist` to
`etc/modules/mod_ollama_chat.conf`, then follow [docs/minds.md](https://github.com/jmcelreavey/squidbots-dashboard/blob/minds/docs/minds.md)
in the dashboard repo: start the service, add a model, apply the **Minds on** recipe, whitelist your account.

Checked against: CoA core `main` (442cf4c9a), mod-playerbots `coa` (c7332f7), clang, Release, static modules, with the
mind service and OpenAI models. `python3 tools/e2e/minds_run.py` (a clientless player, see the file's header) passes all
of: whisper answered in character, knows its real class and level, remembers across conversations, the service saw the right
player and bot, and "invite me" produces a group invite; with the gateway off it passes the baseline (the module does nothing
and the service is never called).

## What is different from upstream synthiqbots

| Change | Why |
|---|---|
| Class names for classes 12+ come from playerbots' `ChatHelper::FormatClass` | The model heard "Unknown" for every CoA class; the client's `ChrClasses` table has the real names. |
| `OllamaChat.LocalChannelNames` / `GlobalChannelNames` | CoA's per-zone channel is `Zone - <place>` and its realm channel is `Ascension`; the fixed `General -`/`World` matching missed both. |
| `OllamaChat.Gateway.InjectIdentity` | The bot/player identity block is what a memory-keeping gateway needs; it was sent only while the MCP server (bound to `0.0.0.0`) was on. |
| Promotion falls back to `Gateway.Url` | Whispering a bot woke it only if a "template" bot had a per-bot override (a fleet setup). A single global gateway now works. |
| `table_schema = DATABASE()` | Six queries named `acore_characters`; realms with other schema names (`coa_dev_*`, a second realm) silently lost personality and audit tables. |
| `#include <cstdint>` in `mod-ollama-chat_httpclient.h` | Did not compile with clang. |
| `OllamaChat.AnswerAddressedInCombat` | A whispered bot in combat was silently ignored; a bot that hunts is in combat almost always. |
| `OllamaChat.PromptDir`, prompt lookup under `SourceDirectory` | Off Docker the three prompt files were never found. |
| Gateway needs only a URL | An empty token disabled the whole gateway at startup; a gateway on the same machine has none. |
| `bot_guid=` first in the tactical snapshot | The quick-decision requests named the bot but gave no guid, so a gateway could not tell which bot a call was for (and CoA names have two words). |
| Classifier: "say ... in world/zone/trade/lfg chat" maps to `bot_channel_say` | The fast classifier turned every "say" into a local `bot_say`, so the channel tool was never reached. |
| New tool `bot_channel_say` | Bots could only say or yell locally. Now an awake bot can speak in the zone, Trade, LFG or realm channel when asked or when the line is for the channel, through playerbots' own channel code. |
| `OllamaChat.Tactical.Enable = 0` by default | So a realm without AI behaves like one without the module. The Minds on recipe turns it on. |

## Bots that are not awake behave as before

The module acts only for accounts on `Gateway.WhitelistAccountIds` and the bots they wake; with the whitelist empty or
`Gateway.Enable = 0` it does nothing. Stock playerbots chatter (`AiPlayerbot.RandomBotTalk` and friends) is untouched
until you choose to turn it off so the two do not talk over each other.

## Ambient chat (`OllamaChat.Ambient.*`)

`src/mod-ollama-chat_ambient.cpp`. Two hooks into the public-chat handler:

1. **An unnamed line from a whitelisted player** in /say, /yell or a channel (the bots the handler already picks: `SayDistance`,
   `YellDistance`, `PlayerReplyChance.*`, up to `MaxBotsToPick`) is answered through the mind service's `POST /ambient`, one
   detached thread per bot, staggered by `Ambient.StaggerMs`. The answer is spoken on the world thread in the channel the line
   came from (`SayToChannel` for zone, Trade, LFG; `SayToWorld` for the realm channel). A bot's answer can be answered by other
   bots (`ChainChance`, `ChainMaxDepth`), inside a "scene" opened by the player's line and capped by `MaxLinesPerScene`.
   Party members, named bots and whispers keep their old paths.
2. **Stock lines**: `OnPlayerCanUseChat` for /say, /yell and channels calls `MaybeRewrite`. A bot's line that fits one of the
   `ai_playerbot_texts` broadcast/suggest templates, with a whitelisted player within `RewriteRangeYards`, is vetoed (the hook
   returns false) and re-spoken in the bot's voice; the mind service keeps item and quest links intact or says nothing, in which
   case the stock line is sent late. Lines the module spoke itself are remembered for 20 seconds so they are not rewritten again.

**Who is being talked to.** A bot is addressed, and answered by the ordinary path that can act (tools, memory), when a
whitelisted player:

- names it, in full (`Flutki Bot`) or without the ` Bot` suffix (`Flutki`);
- uses a short form where one talks *to* somebody: `Hey Ed`, `Ed, how are you?`, `thanks ed`, `how are you, ed?`, `@ed`. Words
  such as `all`, `guys` or `there` are never read as names. A short form counts when exactly one bot answers to it, or when one
  of them is the player's conversation partner or spoke lately; two strangers called Ed* are not guessed between;
- keeps talking to a bot that just answered them. That bot is the player's **partner** in that channel for
  `Ambient.PartnerWindowSec` (120) after each exchange, so "how are you?" after its answer needs no name. A line to the room
  ("anyone..", "everyone..") still lets other bots join in;
- asks for a party invite with nobody named ("someone invite me"). One idle bot of the player's faction is chosen, and it
  invites them with `bot_invite_to_group`.

An addressed bot answers in the channel the line was said in (General, Trade, LFG, the realm channel, /say), not by whisper.

**Trade adverts are real.** When the Trade channel gets a conversation starter, the chosen bot looks in its bags for something
tradeable a vendor would buy (no soulbound or quest items, whole stack), prices it at a few times the vendor price, and says a
banked advert naming the item and the price. The offer stays open `Ambient.ListingMinutes` (30). A player who whispers or
answers the bot reaches the model with "you are selling X for Y" in its context, and the `bot_sell_by_mail` tool mails the item
cash on delivery (`MailDraft::AddCOD`); the buyer pays when opening the mail. A bot with nothing to sell stays quiet.

Delivery threads never hold a lock while speaking, because speaking re-enters the chat hook.

## The game client has to match the core

The core changed how it talks to the Ascension client on 2026-09-28 (upstream commit `2d13ffd73`, "native Extensions.dll
packets only"): it dropped `CoA.PlaintextWorldHeaders` and now encrypts world packet headers with the stock cipher. A client
built before that (Ascension.exe and Extensions.dll dated 2026-09-25 or earlier) still sends plaintext headers, so on a
current `main` it logs in, shows the realm list, then fails at the world handshake: the server logs
`ReadHeaderHandler(): client 127.0.0.1 sent malformed packet (raw: 000CDC010000, cryptInitialized: true ...)` (that raw
value is a plaintext CMSG_PING) and the client disconnects or crashes in `Extensions.dll`.

Two ways out: use a client release built for the current core, or restore the switch. The private fork's version is four
small hunks in `WorldSocket.cpp` (skip `_authCrypt.Init` when `CoA.PlaintextWorldHeaders` is on and the connection is
allowed to be an Ascension one), plus `CoA.PlaintextWorldHeaders = 1` in `coa.conf`. The headless client in
`tools/e2e` speaks plaintext headers with `WOWCLIENT_PLAINTEXT_HEADERS=1`; the realm can only be in one mode at a time.

## Jev on Conquest of Azeroth (measured 2026-09-30)

The tactical funnel (`Jev.Enable = 1`, `Jev.Tactical.Enable = 1`, a TypeSafe key in `Jev.ApiKey` or the
`AC_OLLAMA_CHAT_JEV_API_KEY` environment variable, and an absolute `Jev.QuestionsFile` path) works on this core. Against
`gpt-6-luna` as the fallback, over 82 ticks: jev decided 60 (all `tactical_idle`) in about 0.4 s; the other 22 ran on the LLM
tier at about 2.1 s (free-text or argument actions such as `bot_say`, plus 18 idle ticks jev was not confident about). A jev
tick sends about 2,100 input tokens, which is about $0.00009; a luna tick costs about $0.00013, so the money saved is
roughly 10 to 30 percent of the tactical spend (about half a cent an hour per companion). The real gain is latency. Note
that the jev client turns TLS certificate verification off (`mod-ollama-chat_jev.cpp`, `MakeClient`), so the bearer key
travels without a certificate check.

## Client polls and addon commands

- **`.localspecstate`:** jealous-sound removed the chat command in #5507 (talent state moved to native Extensions.dll packets), but
  the Ascension client build still polls it, and an unknown command answers every poll with a red "Command does not exist" line.
  The module registers it as a silent no-op. Drop that entry if you run a core that still implements it (a duplicate name).
- **SquidBots Lite** (Zyth45's client addon) orders bots with plain playerbots text: `follow`, `co +passive`, `@tank nc -coa auto pull`,
  `formation circle`, `focus heal +Name`, `co ?`. Playerbots answers those itself, so the chat handler now ignores such lines in every
  channel (not only whispers) instead of asking the model to reply on top.

## Keeping up with upstream

`main` here mirrors `synthiq-ai/synthiqbots`; `coa` is the working branch. `git fetch upstream && git merge upstream/main`
into `coa`. The CoA changes are small and spread thin on purpose. Note that `config.cpp`, `config.h`,
`handler.cpp` and a few more use CRLF line endings: keep them, or merges get painful.
