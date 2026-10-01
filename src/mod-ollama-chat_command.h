#ifndef MOD_OLLAMA_CHAT_COMMAND_H
#define MOD_OLLAMA_CHAT_COMMAND_H

#include "ScriptMgr.h"
#include "Chat.h"

class OllamaChatConfigCommand : public CommandScript
{
public:
    OllamaChatConfigCommand();
    Acore::ChatCommands::ChatCommandTable GetCommands() const override;

    static bool HandleOllamaReloadCommand(ChatHandler* handler);
    // The Ascension client still polls ".localspecstate" on cores that removed it (jealous-sound #5507 moved the
    // talent state to native packets). Unknown, the server answers every poll with a red "does not exist" line.
    static bool HandleLegacyClientPollCommand(ChatHandler* handler);
    static bool HandleOllamaSentimentViewCommand(ChatHandler* handler, Optional<std::string> botName, Optional<std::string> playerName);
    static bool HandleOllamaSentimentSetCommand(ChatHandler* handler, std::string botName, std::string playerName, float sentimentValue);
    static bool HandleOllamaSentimentResetCommand(ChatHandler* handler, Optional<std::string> botName, Optional<std::string> playerName);
    static bool HandleOllamaPersonalityGetCommand(ChatHandler* handler, std::string botName);
    static bool HandleOllamaPersonalitySetCommand(ChatHandler* handler, std::string botName, std::string personality);
    static bool HandleOllamaPersonalityListCommand(ChatHandler* handler);
    static bool HandleOllamaGatewayStatusCommand(ChatHandler* handler);
    static bool HandleOllamaGatewayTestCommand(ChatHandler* handler, std::string botName, Acore::ChatCommands::Tail prompt);
    static bool HandleOllamaGatewayCostsCommand(ChatHandler* handler, Optional<uint32> hours);
    static bool HandleOllamaGatewayPruneCommand(ChatHandler* handler);
    static bool HandleOllamaTacticalStatusCommand(ChatHandler* handler, Optional<uint64> botGuid);
    static bool HandleOllamaDirectorStatusCommand(ChatHandler* handler);
    static bool HandleOllamaDirectorOffCommand(ChatHandler* handler);
    static bool HandleOllamaDirectorOnCommand(ChatHandler* handler);
    static bool HandleOllamaDirectorForceConserveCommand(ChatHandler* handler);
    static bool HandleOllamaDirectorForceCcCommand(ChatHandler* handler);

    // Player-facing controls (SEC_PLAYER)
    static bool HandleOllamaOptOutCommand(ChatHandler* handler);
    static bool HandleOllamaOptInCommand(ChatHandler* handler);
    static bool HandleOllamaMuteCommand(ChatHandler* handler, std::string botName);
    static bool HandleOllamaUnmuteCommand(ChatHandler* handler, std::string botName);
};

#endif // MOD_OLLAMA_CHAT_COMMAND_H
