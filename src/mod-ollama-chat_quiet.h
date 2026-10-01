#ifndef MOD_OLLAMA_CHAT_QUIET_H
#define MOD_OLLAMA_CHAT_QUIET_H

#include <cstdint>

// Keeping a bot quiet about what nobody asked it to do.
//
// mod-playerbots tells a master what a bot is doing in answer to a command ("Following", "Selling [Linen Cloth]", "I'm maintaining"), because a
// player typed the command. The module also gives bots commands of its own: the tactical tier sends a bot to follow, stay, sell its grey items,
// repair, train or regear when it judges that worthwhile, and it runs them as the bot's master, so playerbots takes them for orders from the
// player and the player is told about each one, out of nowhere. Mute() makes a bot report nothing for a few seconds around such a command.
//
// It works through a gate in mod-playerbots (patches/mod-playerbots-quiet-reports.patch). Without that patch the gate is never asked and
// nothing changes.
namespace OllamaChat::Quiet
{
    // Any thread. The bot reports nothing to its master for `seconds`.
    void Mute(uint64_t botGuid, uint32_t seconds);

    // Any thread.
    bool IsMuted(uint64_t botGuid);
}

#endif
