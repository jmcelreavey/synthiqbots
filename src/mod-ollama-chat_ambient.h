#ifndef MOD_OLLAMA_CHAT_AMBIENT_H
#define MOD_OLLAMA_CHAT_AMBIENT_H

#include "mod-ollama-chat_handler.h"

#include "ScriptMgr.h"

#include <cstdint>
#include <ctime>
#include <string>

class Channel;
class Player;

// Ambient chat: a whitelisted player says "hi" in /say, /yell or a public channel (General, Trade, LFG, the realm
// channel) without naming anyone, and the bots in earshot answer in their own voices, staggered like people typing.
// A bot's answer can be answered in turn by another bot, a few lines deep, so a street of bots talks back and
// among themselves for a minute and then goes quiet. The words come from the mind service (POST /ambient), which
// knows each bot's personality; a bot the player names, whispers or groups with is handled by the ordinary paths.
namespace OllamaChat
{
    namespace Ambient
    {
        // Percent chance that a bot in earshot answers a line that another bot said as part of an ambient conversation.
        uint32_t ChainChancePercent();

        // True when `msg`, said by the bot `speakerGuid`, is a line an ambient reply put in that bot's mouth a moment ago.
        bool IsSceneLine(uint64_t speakerGuid, const std::string& msg);

        // Called for every line a player-controlled character is about to send to /say, /yell or a channel. When it is one of
        // playerbots' stock lines (its ai_playerbot_texts templates: "Took [quest]. Time to dive in.") from a bot with a
        // whitelisted player within earshot, the stock line is held back and the bot says the same thing in its own voice a
        // moment later; returns true then. Anything else, and every line while nobody is near, goes out untouched.
        bool MaybeRewrite(Player* speaker, uint32_t chatType, const std::string& msg, Channel* channel);

        // The same for a line a bot sends to a public channel through playerbots' SayToChannel / SayToWorld (chan is a
        // ChatChannelId, or -1 for the realm channel). Reached through the filter in
        // patches/mod-playerbots-outgoing-channel-filter.patch; without that patch only /say and /yell lines are rewritten.
        bool MaybeRewriteChannel(Player* speaker, int chan, const std::string& msg);

        // Blocking HTTP: call from a worker thread, never the world thread. A line for the bot `botGuid` to say in party chat about
        // the enemy `mob`, in its own voice, from the mind service's line bank. `kind` is "focus" (kill it first) or "cc" (it is
        // crowd-controlled). Empty when the service is off or down, has nothing for this kind of bot, or the bot is muted.
        std::string PartyCallout(uint64_t botGuid, const std::string& botName, const std::string& kind, const std::string& mob,
                                 const std::string& contextJson = std::string{});

        // World thread. What a roleplaying character needs to know about `bot` (race, class, gender, level, zone, area, what it is doing and
        // its open errands) as one line of JSON: it goes into the BOT STATE SNAPSHOT of a conversation and into the director's callouts.
        std::string RoleplayContextJson(Player* bot);

        // World thread, every world update. Now and then, for a whitelisted player who has been standing in a quiet zone, one
        // bot there says something unprompted (a line from the mind service's line bank), which the bots around it can answer
        // like any other ambient line. Without this a realm is silent until somebody types. OllamaChat.Ambient.StarterIntervalSec
        // (0 turns it off) is how often a zone is considered.
        void Tick(uint32_t diff);

        // What a bot advertised in the Trade channel, for as long as the offer stands (OllamaChat.Ambient.ListingMinutes). It is
        // something the bot really carries, so a player who answers the advert can really buy it: the bot's mail tool
        // (bot_sell_by_mail) sends it cash on delivery at this price.
        struct Listing
        {
            uint32_t    itemId      = 0;
            uint32_t    count       = 0;
            uint32_t    priceCopper = 0;
            time_t      at          = 0;
            std::string link;        // "[Linen Cloth] x20", as said in chat
            std::string priceText;   // "2g 50s"
        };

        // When `botGuid` last said an ambient line (0: not lately). "Hey Ed" goes to whichever Ed was just talking.
        time_t LastSpokeAt(uint64_t botGuid);

        // "say", "yell", "zone", "trade", "lfg", "world" for a public line, "" when it arrived anywhere else (party, whisper, guild).
        std::string ChannelKind(ChatChannelSourceLocal source, Channel* channel);

        // World thread. The bot says `text` in that kind of channel, so a player who asked in General is answered in General and
        // not by a whisper nobody else sees.
        bool SayInChannel(uint64_t botGuid, const std::string& kind, const std::string& text);

        // True (and `out` filled) while `botGuid` has an open offer.
        bool GetListing(uint64_t botGuid, Listing& out);
        void ClearListing(uint64_t botGuid);

        // A whitelisted player has just logged in: a few of the realm's regulars will greet them by name a moment later, and, when
        // OllamaChat.Community.JoinHomeGuild is on, a player with no guild is put into one of the regulars' guilds the first time.
        void NoteLogin(Player* player);

        // The bot that last answered `speaker` in this channel while the exchange is still warm
        // (OllamaChat.Ambient.PartnerWindowSec, 0 turns it off); 0 when there is none. A player who keeps talking after a bot
        // answered expects that bot to answer again, whether or not they type its name.
        uint64_t PartnerOf(Player* speaker, ChatChannelSourceLocal source, Channel* channel);

        // `force`: the player named the bot (or it was asked for something), which takes the conversation over from whoever
        // answered before. Without it the first bot to answer keeps the conversation.
        void RememberPartner(Player* speaker, ChatChannelSourceLocal source, Channel* channel, uint64_t botGuid, bool force);

        // "someone invite me", "send me a group invite", "inv me pls": a request for a party invite, to nobody in particular.
        bool IsInviteRequest(const std::string& msg);

        // Called for each bot the chat handler picked to react to a public line. Returns true when the line was taken
        // (an answer is on its way, or the bot chose to stay quiet) and the caller must not route it any further.
        // `addressed`: the line is aimed at this bot (named, a conversation it is in, an invite asked of it), so it is answered
        // by the ordinary path that can act, never as ambient chatter.
        bool Handle(Player* bot, Player* speaker, const std::string& msg, ChatChannelSourceLocal source,
                    Channel* channel, bool speakerIsBot, bool addressed = false);
    }
}

// Tells the ambient chat that a player has come online (see NoteLogin).
class CommunityScript : public PlayerScript
{
public:
    CommunityScript();
    void OnPlayerLogin(Player* player) override;
};

#endif // MOD_OLLAMA_CHAT_AMBIENT_H
