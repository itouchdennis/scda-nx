// The console on its key (backtick by default). A line goes to the game through UPlayer::Exec on the next input update, as the
// game's own console would send it, and what the command prints comes back into the log.
#include <algorithm>
#include <atomic>
#include <iterator>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "app/panel_colors.h"
#include "core/input.h"
#include "game/console.h"

namespace app {

namespace {

constexpr size_t kMaxLog = 500;

std::atomic<bool> g_open{false};
bool g_focus = false;
bool g_scroll = false;
char g_line[256] = {};
std::vector<std::string> g_log, g_history;
int g_recall = -1;  // history entry on the line, -1 for a new one

void Add(std::string line) {
    g_log.push_back(std::move(line));
    if (g_log.size() > kMaxLog) g_log.erase(g_log.begin());
    g_scroll = true;
}

int LineCallback(ImGuiInputTextCallbackData* data) {
    // the console key's own character
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) return data->EventChar == '`' || data->EventChar == '~' || input::ConsoleChar(data->EventChar);
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || g_history.empty()) return 0;
    int n = int(g_history.size());
    if (data->EventKey == ImGuiKey_UpArrow) g_recall = g_recall < 0 ? n - 1 : std::max(0, g_recall - 1);
    else if (data->EventKey == ImGuiKey_DownArrow) g_recall = g_recall < 0 || g_recall + 1 >= n ? -1 : g_recall + 1;
    data->DeleteChars(0, data->BufTextLen);
    if (g_recall >= 0) data->InsertChars(0, g_history[size_t(g_recall)].c_str());
    return 0;
}

void Submit() {
    std::string line = g_line;
    g_line[0] = 0;
    g_recall = -1;
    line.erase(0, line.find_first_not_of(' '));
    line.erase(line.find_last_not_of(' ') + 1);
    if (line.empty()) return;
    Add("> " + line);
    if (g_history.empty() || g_history.back() != line) g_history.push_back(line);
    game::QueueCommand(line, game::Route::Player);
}

}  // namespace

bool ConsoleOpen() { return g_open.load(std::memory_order_relaxed); }

void ToggleConsole() {
    bool open = !ConsoleOpen();
    g_open.store(open, std::memory_order_relaxed);
    g_focus = open;
    input::SetBlocked(open || MenuOpen());
}

void DrawConsole() {
    for (std::string& line : game::TakeOutput()) Add(std::move(line));
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y * 0.4f), ImGuiCond_Always);
    for (const auto& [col, color] : kPanelColors) ImGui::PushStyleColor(col, color);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##console", nullptr, kFlags)) {
        ImGui::BeginChild("##log", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
        for (const std::string& line : g_log) {
            bool echo = line.rfind("> ", 0) == 0;
            if (echo) ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
            ImGui::TextUnformatted(line.c_str());
            if (echo) ImGui::PopStyleColor();
        }
        if (g_scroll) ImGui::SetScrollHereY(1.0f);
        g_scroll = false;
        ImGui::EndChild();
        ImGui::SetNextItemWidth(-1.0f);
        if (g_focus) ImGui::SetKeyboardFocusHere();
        g_focus = false;
        constexpr ImGuiInputTextFlags kLineFlags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackHistory;
        if (ImGui::InputText("##line", g_line, sizeof(g_line), kLineFlags, LineCallback)) {
            Submit();
            g_focus = true;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(int(std::size(kPanelColors)));
}

}  // namespace app
