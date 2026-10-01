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
        uint32_t s_intervalMs = 60000;
        bool s_enabled = true;

        // Read at startup and again each time the roster is saved, not on every update tick: a key missing from the conf file is
        // logged each time it is asked for. Off with periodic online/offline, which rotates the roster on purpose (putting it back
        // would fight that).
        void LoadConfig()
        {
            s_enabled = sConfigMgr->GetOption<bool>("OllamaChat.Roster.KeepAcrossRestarts", true) &&
                        !sConfigMgr->GetOption<bool>("AiPlayerbot.EnablePeriodicOnlineOffline", false);
            s_intervalMs = std::max<uint32_t>(10, sConfigMgr->GetOption<uint32>("OllamaChat.Roster.SaveIntervalSec", 60)) * 1000;
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
        LoadConfig();
        if (!s_enabled)
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
        if (s_accumMs < s_intervalMs)
            return;
        s_accumMs = 0;
        LoadConfig();

        // A realm with nobody on has no roster yet: keep the last one rather than save nothing over it.
        if (!s_enabled || CountRows("add") == 0)
            return;

        auto transaction = PlayerbotsDatabase.BeginTransaction();
        transaction->Append("DELETE FROM playerbots_random_bots WHERE owner = 0 AND event = 'ollama_roster'");
        transaction->Append(
            "INSERT INTO playerbots_random_bots (owner, bot, `time`, validIn, event, `value`) "
            "SELECT 0, bot, UNIX_TIMESTAMP(), validIn, 'ollama_roster', 1 FROM playerbots_random_bots WHERE owner = 0 AND event = 'add'");
        PlayerbotsDatabase.CommitTransaction(transaction);
    }
}
