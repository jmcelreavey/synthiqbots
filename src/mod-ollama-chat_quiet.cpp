#include "mod-ollama-chat_quiet.h"

#include "Object.h"
#include "Player.h"

#include <ctime>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace OllamaChat::Quiet
{
    namespace
    {
        std::mutex s_mutex;
        std::unordered_map<uint64_t, time_t> s_mutedUntil;
    }

    void Mute(uint64_t botGuid, uint32_t seconds)
    {
        const time_t now = time(nullptr);
        std::lock_guard<std::mutex> lock(s_mutex);
        s_mutedUntil[botGuid] = now + seconds;
        if (s_mutedUntil.size() > 4000)       // a bot logged out mid-window leaves its entry behind: sweep the ones that have run out
            for (auto it = s_mutedUntil.begin(); it != s_mutedUntil.end();)
                it = it->second < now ? s_mutedUntil.erase(it) : std::next(it);
    }

    bool IsMuted(uint64_t botGuid)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_mutedUntil.find(botGuid);
        return it != s_mutedUntil.end() && time(nullptr) < it->second;
    }
}

// What mod-playerbots asks before a bot reports to its master (a weak extern in PlayerbotAI.cpp; see the header).
std::function<bool(Player*)> g_ollamaMuteReports;

namespace
{
    struct MuteReportsInstaller
    {
        MuteReportsInstaller()
        {
            g_ollamaMuteReports = [](Player* bot) { return bot && OllamaChat::Quiet::IsMuted(bot->GetGUID().GetRawValue()); };
        }
    } s_installer;
}
