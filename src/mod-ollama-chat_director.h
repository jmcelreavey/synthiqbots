#ifndef MOD_OLLAMA_CHAT_DIRECTOR_H
#define MOD_OLLAMA_CHAT_DIRECTOR_H

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>

class Player;

// Combat director: while a whitelisted player and their bots are in a fight, it asks jev now and then which
// enemy to kill first, which to crowd-control and whether the healers should ration mana, and puts the answers
// on the board the playerbots engine already reads (raid marks, the `save mana` strategy). The engine still casts
// every spell; the director only decides who the party is fighting. See docs/director.md and
// mod-ollama-chat_director_core.h for the rules that bound what the model may change.
namespace OllamaChat
{
    namespace CombatDirector
    {
        // World thread, every world update. No-op unless OllamaChat.Director.Enable=1 and the jev `director` site is on.
        void Tick(uint32_t diff);

        // A player can switch the director off for their own character (`.ollama director off`), without silencing the bots the way
        // `.ollama optout` does. Remembered in the character's settings, so it survives a logout. World thread.
        bool IsOffFor(Player* player);
        void SetOffFor(Player* player, bool off);

        // Test hook (`.ollama director force conserve`, administrators): on its next tick the director puts `save mana` on the healers of
        // this player's party as if jev had chosen `conserve`, so the apply / verify / clean-up path can be tested without a model
        // choosing it and without a healer being low on mana. World thread.
        void ForceConserveFor(Player* player);

        // Test hook (`.ollama director force cc`): at its next look the director puts the moon on its best crowd-control candidate as if jev had
        // chosen it, so the engine's response (does a mage polymorph it?) and the release of a mark that is not holding can be tested without a
        // model. Needs three or more enemies and a living bot of a class that can crowd-control. World thread.
        void ForceCcFor(Player* player);

        // The last `max` decisions, oldest first: what the model was asked, what it said, what was applied. Thread-safe.
        nlohmann::json RecentDecisions(std::size_t max = 20);

        // Counters since the worldserver started, for a test that wants proof of what the director did in a window (read them
        // before and after). Thread-safe.
        //   consults     answers jev has given
        //   yield_ticks  one-second ticks in a fight where the focus question was withheld because the player owns the skull
        //   fights_open  fights the director is following right now (it ends one a second or two after the last enemy dies)
        nlohmann::json Counters();
    }
}

#endif // MOD_OLLAMA_CHAT_DIRECTOR_H
