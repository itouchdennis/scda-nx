// Versus has no console yet: commands are dropped and the level select is empty
#include "game/console.h"

namespace game {

void QueueCommand(std::string, Route) {}
void RunCommands(X86Context&, uint32_t, uint32_t) {}
std::vector<std::string> TakeOutput() { return {}; }
const std::vector<LevelList>& Levels() {
    static const std::vector<LevelList> kNone;
    return kNone;
}

}  // namespace game
