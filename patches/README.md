# Patches

Changes to code that is not ours. Apply them in the checkout they name, in this order, with `git apply` (or `patch -p1`); `git apply --check` first.

| patch | applies to | what it does |
|---|---|---|
| `mod-playerbots-outgoing-channel-filter.patch` | `modules/mod-playerbots` | A weak `g_ollamaOutgoingChannelFilter` in `PlayerbotAI.cpp`, so the module can hold playerbots' stock channel chatter back (roleplay mode). |
| `mod-playerbots-quiet-reports.patch` | `modules/mod-playerbots` | A weak `g_ollamaMuteReports` gate in `PlayerbotAI::MasterWantsThis`, so the module can keep a bot from telling its master "Following", "Selling [item]" or "I'm maintaining" about a command the module gave it on its own (see `mod-ollama-chat_quiet.h`). Needs the channel-filter patch above applied first. |
| `core-worldsocket-plaintext-headers.patch` | the CoA core (`azerothcore-wotlk-coa`) | The CoA client build from before upstream commit `2d13ffd73` keeps world packet headers plaintext after a valid `CMSG_AUTH_SESSION`; this puts that mode back behind `CoA.PlaintextWorldHeaders = 1`. Needed for the Ascension client on the test realm. |
| `core-coa-companion-enabling.patch` | the CoA core | What `Corfirean/mod-coa-playerbots` needs from the core that a module cannot reach: `AscensionClassServiceBridge` (spec and talent switching through the real, budget-checked service), `friend class BotMgr` on `Guild`, `LFGMgr::GetProposalIdForPlayer`, and a `PLAYERHOOK_ON_PETITION_OFFERED` hook (`ScriptMgr::OnPetitionOffered`, fired when a player is asked to sign a petition). All additive: with no module using them nothing changes. Their `docs/core-patches.md` lists three more changes in their working tree that are left out on purpose, because they change behaviour for everyone: `CombatManager::PutReference` no longer asserts on a duplicate combat reference, `Player::ApplySpellMod` lets a modifier raise a free or instant cost, and extra error logging in the petition and name-query handlers. |

The core already has `WorldSession`'s public `LoginQueryHolder`, `World::AddQueryHolderCallback` and `Group::GetRolls`, which are the other
two patches that module's docs list.

## What still stops `Corfirean/mod-coa-playerbots` building here

A syntax-only compile of its 97 source files against this core with the patches above (flags from a real module file; nothing linked): **62 compile**.
The other 35 (`engine/*`, `profiles/*`, `BotAI.cpp`, `BotFlee.cpp`, `BotMgr.cpp`, `BotWorldScript.cpp`) stop on one thing: `#include "AscensionResourceQuery.h"`
in `engine/CombatResource.h`, a header of query functions over the CoA classes' resources that the combat engine calls into. It is not in the CoA
core's `main` (or any other branch of `jealous-sound/azerothcore-wotlk-coa`) and not in their `docs/core-patches.md`; it lives only in their own core
checkout. Ask for that commit. One more, unrelated to the core: `BotTalentBuilds.cpp` needs `#include <boost/bind/bind.hpp>` with a current Boost
(`boost::placeholders` moved).
