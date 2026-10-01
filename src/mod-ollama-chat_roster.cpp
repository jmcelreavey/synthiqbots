#include "mod-ollama-chat_roster.h"

#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "PlayerbotMgr.h"
#include "QueryResult.h"

#include <algorithm>

namespace OllamaChat::Roster
{
    namespace
    {
        uint32_t s_accumMs = 0;

        // Off with periodic online/offline: that setting rotates the roster on purpose, and putting it back would fight it.
        bool Enabled()
        {
            return sConfigMgr->GetOption<bool>("OllamaChat.Roster.KeepAcrossRestarts", true) &&
                   !sConfigMgr->GetOption<bool>("AiPlayerbot.EnablePeriodicOnlineOffline", false);
        }

        uint64_t CountRows(char const* event)
        {
            QueryResult result = PlayerbotsDatabase.Query(
                "SELECT COUNT(*) FROM playerbots_random_bots WHERE owner = 0 AND event = '{}'", event);
            return result ? (*result)[0].Get<uint64>() : 0;
        }
    }

    void Restore()
    {
        if (!Enabled())
            return;

        uint32_t const validIn = sConfigMgr->GetOption<uint32>("AiPlayerbot.PermanentlyInWorldTime", 31104000);
        // Direct, not queued: the first update tick reads these rows and must find them. A bot that is already in the roster, or
        // that has a live `logout` row (benched), is left out.
        PlayerbotsDatabase.DirectExecute(
            "INSERT INTO playerbots_random_bots (owner, bot, `time`, validIn, event, `value`) "
            "SELECT 0, s.bot, UNIX_TIMESTAMP(), {}, 'add', 1 FROM playerbots_random_bots s "
            "WHERE s.owner = 0 AND s.event = 'ollama_roster' "
            "AND NOT EXISTS (SELECT 1 FROM playerbots_random_bots a WHERE a.owner = 0 AND a.bot = s.bot AND a.event = 'add') "
            "AND NOT EXISTS (SELECT 1 FROM playerbots_random_bots l WHERE l.owner = 0 AND l.bot = s.bot AND l.event = 'logout' "
            "AND (l.validIn = 0 OR l.`time` + l.validIn > UNIX_TIMESTAMP()))",
            validIn);

        LOG_INFO("server.loading", "[Ollama Chat Roster] {} random bots remembered, {} in the roster after putting them back",
                 CountRows("ollama_roster"), CountRows("add"));
    }

    void Tick(uint32_t diffMs)
    {
        s_accumMs += diffMs;
        uint32_t const intervalMs = std::max<uint32_t>(10, sConfigMgr->GetOption<uint32>("OllamaChat.Roster.SaveIntervalSec", 60)) * 1000;
        if (s_accumMs < intervalMs)
            return;
        s_accumMs = 0;

        // A realm with nobody on has no roster yet: keep the last one rather than save nothing over it.
        if (!Enabled() || CountRows("add") == 0)
            return;

        auto transaction = PlayerbotsDatabase.BeginTransaction();
        transaction->Append("DELETE FROM playerbots_random_bots WHERE owner = 0 AND event = 'ollama_roster'");
        transaction->Append(
            "INSERT INTO playerbots_random_bots (owner, bot, `time`, validIn, event, `value`) "
            "SELECT 0, bot, UNIX_TIMESTAMP(), validIn, 'ollama_roster', 1 FROM playerbots_random_bots WHERE owner = 0 AND event = 'add'");
        PlayerbotsDatabase.CommitTransaction(transaction);
    }
}
