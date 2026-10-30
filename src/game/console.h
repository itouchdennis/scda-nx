#pragma once
#include <string>
#include <vector>

#include "cpu/x86_context.h"

// console commands from the host, run on the game thread
namespace game {

// Engine: UGameEngine::Exec alone (open, travel). Player: UPlayer::Exec, as the game's own console sends a line:
// the player's interactions, controller and pawn first, then the engine.
enum class Route { Engine, Player };
void QueueCommand(std::string command, Route route = Route::Engine);  // any thread
// from the input update; a command such as open may load a map, so nothing may use the level after this
void RunCommands(X86Context& ctx, uint32_t engine, uint32_t viewport);
// what the commands printed to their output device since the last call; any thread
std::vector<std::string> TakeOutput();

// the F10 level select: maps "open" loads, by section
struct LevelList {
    const char* title;
    std::vector<const char*> maps;
};
const std::vector<LevelList>& Levels();

}  // namespace game
