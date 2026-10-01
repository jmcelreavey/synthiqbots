#ifndef MOD_OLLAMA_CHAT_ROSTER_H
#define MOD_OLLAMA_CHAT_ROSTER_H

#include <cstdint>

// The roster: which random bots are in the world.
//
// mod-playerbots keeps one `add` row per random bot that belongs in the world (playerbots_random_bots, owner 0), and
// RandomPlayerbotMgr::Init() deletes every one of them each time the worldserver starts. The first player to log in then
// finds an empty roster and playerbots draws a new one at random, so a restart means 200 different bots out of the 1,400 on
// the realm: the ones with stories, friends and a guild are gone and strangers stand in their place.
//
// This keeps a copy of the roster under its own event name (`ollama_roster`, which Init() leaves alone) and puts the `add` rows
// back at startup, before playerbots reads them. It also keeps a bot out of the roster that was benched on purpose
// (a live `logout` row), which playerbots already refuses to pick.
namespace OllamaChat::Roster
{
    // World thread, from OnStartup (after playerbots' Init() has run). Puts the remembered roster back as `add` rows.
    void Restore();

    // World thread, from OnUpdate. Interval-gated: copies the live roster to the remembered one, unless there is none.
    void Tick(uint32_t diffMs);
}

#endif
