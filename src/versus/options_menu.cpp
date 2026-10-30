// RECOMP SETTINGS, MOUSE AND KEYBOARD and CONTROLLER under VOICE in the versus Options page. Each lists its sections
// (as story's) on P_Options' own rows; a section opens a page of the F10 panel's rows in P_Options_Game's frame. Magma
// 5.0 loads its packages from XML text, so what the stock packages lack is spliced into that text before it is parsed
// and the game builds it like its own: the three buttons (copies of the last one, Online, shown as VOICE, a row lower
// each, in the up / down loop between it and Game) and the two pages in Options, the rows page's row (the option rows'
// CHK_Default as a button: its bar and label, and a value on the right) in common, and the popup listing a row's values
// (a copy of P_TeamsActionPopup, on that package's own B_OptionItem rows, at the place of the map list's popup) in
// xboxlive. Labels and titles come through the text lookup (xbox_live.cpp) or are set when a page opens. As story's
// options menu: A flips an on / off row, opens the popup on a row with a list and steps the others, Left / Right step
// those.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/app.h"
#include "app/translations.h"
#include "xdk/hle.h"

using namespace kernel;

namespace dispatch {
uint32_t HostThunk(X86Func* fn);
}  // namespace dispatch

namespace {

constexpr uint32_t kMalloc = 0x149DF0;  // cdecl (size, tag)
constexpr uint32_t kFree = 0x11230;     // cdecl (pointer)
constexpr int kRowStep = 24;  // the stock rows' spacing, y 250 to 322

// Magma ids: crc32 of the lowercase name
constexpr uint32_t Id(std::string_view name) {
    uint32_t crc = ~0u;
    for (char c : name) {
        crc ^= uint8_t(c >= 'A' && c <= 'Z' ? c + 32 : c);
        for (int i = 0; i < 8; i++) crc = crc >> 1 ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
static_assert(Id("Online") == 0x9E32BEEA && Id("B_MainItem") == 3405425947u);
constexpr uint32_t kOptionsPage = Id("P_Options");
static_assert(kOptionsPage == 0xD7F513E8);

// menus: the base constructor (thiscall menu, FString* package, FString* page, ret 8) keeps the names the push
// (cdecl (menu, 1, 1)) finds the page by, then calls the open (vtable slot 6, thiscall, no arguments)
// Vtable slots here and below are byte offsets into the vtable.
constexpr uint32_t kMenuSize = 0x10C;
constexpr uint32_t kMenuCtor = 0x1AA210;
constexpr uint32_t kMenuVtable = 0x338BD8;
constexpr uint32_t kMenuSlots = 13;
constexpr uint32_t kOpenSlot = 0x18;
constexpr uint32_t kBaseOpen = 0x1AA0D0;
constexpr uint32_t kPush = 0x193950;
// slot 1 runs as a menu comes on top again (after the one over it went): the base's does nothing, P_Options' and
// P_Options_Game's set the legend, (B) Back (A) Ok, which the page over it changed
constexpr uint32_t kLegendSlot = 0x04;
constexpr uint32_t kOptionsLegend = 0x1C44B0;
constexpr uint32_t kFString = 0x16540;      // thiscall (FString, const char*): {wide text, count, capacity}
constexpr uint32_t kFStringSize = 0x0C;
constexpr uint32_t kMenuPage = 0x28;  // menu: its Magma page
// page
constexpr uint32_t kPageId = 0x04;
// an instance's handler (bound by thiscall menu (handler, instance id), ret 8): a refcounted object {vtable,
// refcount, ..} whose vtable +0x10 is the press (thiscall (one argument), ret 4) and +0x18 the other events
// (thiscall (?, event*), ret 8), both true when handled; the Game button's vtable
constexpr uint32_t kBind = 0x193A20;
constexpr uint32_t kHandlerVtable = 0x338E48;
constexpr uint32_t kHandlerSlots = 8;
constexpr uint32_t kPressSlot = 0x10;
constexpr uint32_t kEventSlot = 0x18;
// handler object layout: +4 refcount, +8 and +0xC as the Game button's, +0x10 ours
constexpr uint32_t kHandlerRefcount = 0x04;
constexpr uint32_t kHandlerWord8 = 0x08;  // 1, as the Game button's
constexpr uint32_t kHandlerWordC = 0x0C;
constexpr uint32_t kHandlerWordCValue = 0x3380E0;
constexpr uint32_t kHandlerKind = 0x10;  // ours: what it opens
// B as P_Options takes it, in its menu's key slot 10 (thiscall (key, action, ?), ret 0xC, true when handled): on
// B's release (IK_Joy2 0xC9, IST_Release 3) the menu is popped (cdecl, no arguments). Taken on the press (handler
// event 0x81) instead, the release would pop the menu under it as well.
constexpr uint32_t kKeySlot = 0x28;
constexpr uint32_t kJoyB = 0xC9;
constexpr uint32_t kRelease = 3;
constexpr uint32_t kPopMenu = 0x193E40;
constexpr uint32_t kBackSound = 2;
// events: the directions' virtual keys
constexpr uint32_t kLeft = 0x25;
constexpr uint32_t kUp = 0x26;
constexpr uint32_t kRight = 0x27;
constexpr uint32_t kDown = 0x28;
constexpr uint32_t kSelectSound = 1;             // as a stock page's X press plays
// a popup pushes over the page under it (cdecl (menu, 0, 1)), a page in its place (menu, 1, 1)
constexpr uint32_t kListSelect = 0x25EE00;  // list widget: thiscall (row, notify), ret 8
constexpr uint32_t kFind = 0x25A320;  // thiscall page (instance id)
// list widget (the instance's +0x10): Clear, AddItem (text, data) -> row, Part (element id) -> column,
// SetText (row, column, text), which keeps the pointer
constexpr uint32_t kInstanceWidget = 0x10;
constexpr uint32_t kClear = 0x260B40;
constexpr uint32_t kAddItem = 0x261AB0;
constexpr uint32_t kPart = 0x260540;
constexpr uint32_t kSetText = 0x2605F0;
constexpr uint32_t kEmpty = 0x31BA80;  // L""
constexpr uint32_t kTextString = 0x0C;  // text widget: its string
constexpr uint32_t kListSelection = 0x54;   // list widget: the row selected

// our pages, each a stock page's frame with a list in place of its own rows
struct List {
    std::string_view package;
    std::string_view page;
    std::string_view list;
    std::string_view item;
    std::string_view position;
    int rows;
};
// in P_Options' frame on its own rows (B_MainItem)
constexpr List kSectionList{"Options", "P_Options_RecompSections", "SectionList", "B_MainItem", "158 250", 3};
// in P_Options_Game's on ours, where its option rows were: 21 high from y 176, down to the frame's bottom
constexpr std::string_view kItem = "B_RecompItem";
constexpr List kRowList{"Options", "P_Options_Recomp", "RecompList", kItem, "50 176", 10};
constexpr int kRowListY = 176;
constexpr int kRowHeight = 21;
// a section's few rows sit in the middle of the frame under the title (y 84 to 405), a long one under the title
constexpr int kFrameMiddle = 245;
// the popup over the rows: P_TeamsActionPopup's list at the map list popup's place (P_createMatchParamsPopup in
// entry, whose B_OptionItem xboxlive has too), on a background as that one's
constexpr List kChoiceList{"xboxlive", "P_Options_RecompPopup", "ChoiceList", "B_OptionItem", "50 267", 4};
constexpr std::string_view kPopupBackground[] = {"246", "404"};  // top, bottom
// per button, the F10 headings listed as sections, as story's; at most kSectionList.rows
const std::vector<std::string_view> kSections[] = {
    {"DISPLAY", "GRAPHICS", "GENERAL"},
    {"MOUSE", "KEYBOARD"},
    {"GENERAL", "GYRO", "BUTTONS"},
};
// our menus keep their page (0 sections, 1 rows, 2 popup), the button's mode and the section heading's row past the
// base menu's fields (P_Options_Game's own start at +0xC8)
constexpr uint32_t kMenuKind = 0x100;
constexpr uint32_t kMenuMode = 0x104;
constexpr uint32_t kMenuSection = 0x108;
enum PageKind : uint32_t { kSectionsPage = 0, kRowsPage = 1, kPopupPage = 2 };
// what a handler's press opens (see Press): our three buttons, then one per PageKind in its order
enum HandlerKind : int {
    kSettingsButton = 0,
    kMouseKeyboardButton = 1,
    kControllerButton = 2,
    kSectionPick = 3,
    kRowPick = 4,
    kValuePick = 5,
};

struct Button {
    std::string_view name;
    std::string_view label;
    std::string_view title;
};
// labels in capitals as the stock rows, the page title as Options' own
constexpr Button kButtons[] = {{"RecompSettings", "RECOMP SETTINGS", "Recomp Settings"},
                               {"MouseKeyboard", "MOUSE AND KEYBOARD", "Mouse and Keyboard"},
                               {"Controller", "CONTROLLER", "Controller"}};

// game thread
uint32_t g_sections = 0;  // the sections menu open, for its list's press
struct Rows {
    // the rows menu open, its list widget and value column
    uint32_t menu = 0;
    uint32_t list = 0;
    uint32_t value = 0;
    int mode = 0;
    std::vector<const app::rows::Row*> rows;  // per list row, kGap a blank row, null restores defaults
};
// a blank row between headings and before RESTORE DEFAULTS, as story's: the lists have no divider of their own
const app::rows::Row kGap{app::rows::Kind::Heading, -1, ""};
Rows g_rows;
struct Popup {
    uint32_t menu = 0;
    const app::rows::Row* row = nullptr;  // the row whose values it lists
    std::vector<std::string> choices;
    int current = 0;
};
Popup g_popup;

// the package text

// tags whose lengths the splices step over
constexpr std::string_view kPositionOpen = "<POSITION>";
constexpr std::string_view kIdxOpen = "<IDX>";
constexpr std::string_view kKeyframeClose = "</KEYFRAME>";
constexpr std::string_view kCheckBoxOpen = "<CheckBox>";
constexpr std::string_view kChildrenOpen = "<CHILDREN>";
constexpr std::string_view kPageClose = "</Page>";

// the text between <tag> and </tag>, the first from `from`; false when there is none
bool SetTag(std::string& xml, std::string_view tag, std::string_view value, size_t from = 0) {
    std::string open = "<" + std::string(tag) + ">";
    std::string close = "</" + std::string(tag) + ">";
    size_t a = xml.find(open, from);
    size_t b = a == std::string::npos ? a : xml.find(close, a);
    if (b == std::string::npos) return false;
    xml.replace(a + open.size(), b - a - open.size(), value);
    return true;
}

void SetEvery(std::string& xml, std::string_view tag, std::string_view value) {
    for (size_t at = 0; (at = xml.find("<" + std::string(tag) + ">", at)) != std::string::npos; at++)
        SetTag(xml, tag, value, at);
}

// every keyframe's POSITION "x y" moved down
void MoveDown(std::string& xml, int dy) {
    for (size_t at = xml.find(kPositionOpen); at != std::string::npos; at = xml.find(kPositionOpen, at + 1)) {
        size_t start = at + kPositionOpen.size();
        size_t end = xml.find("</POSITION>", start);
        size_t space = xml.find(' ', start);
        if (end == std::string::npos || space > end) return;
        int y = std::atoi(xml.c_str() + space + 1);
        xml.replace(space + 1, end - space - 1, std::to_string(y + dy));
    }
}

struct Span {
    size_t begin;
    size_t end;
};

// the <tag> block holding <NAME>name</NAME>, from `from`: [begin, end)
std::optional<Span> Block(const std::string& xml, std::string_view tag, std::string_view name, size_t from) {
    std::string open = "<" + std::string(tag) + ">";
    std::string close = "</" + std::string(tag) + ">";
    size_t at = xml.find("<NAME>" + std::string(name) + "</NAME>", from);
    if (at == std::string::npos) return std::nullopt;
    size_t begin = xml.rfind(open, at);
    size_t end = xml.find(close, at);
    if (begin == std::string::npos || begin < from || end == std::string::npos) return std::nullopt;
    end += close.size();
    return Span{begin, end};
}

std::string Cut(const std::string& xml, Span s) { return xml.substr(s.begin, s.end - s.begin); }

std::string_view NewLine(const std::string& xml) { return xml.find("\r\n") != std::string::npos ? "\r\n" : "\n"; }

// the keyframes of an element block whose IDX is one of `keep`
std::string KeepFrames(std::string block, std::initializer_list<int> keep) {
    for (size_t at = block.find("<KEYFRAME>"); at != std::string::npos;) {
        size_t end = block.find(kKeyframeClose, at);
        size_t idx = block.find(kIdxOpen, at);
        if (end == std::string::npos || idx == std::string::npos) break;
        end += kKeyframeClose.size();
        int frame = std::atoi(block.c_str() + idx + kIdxOpen.size());
        if (std::find(keep.begin(), keep.end(), frame) != keep.end()) {
            at = block.find("<KEYFRAME>", end);
            continue;
        }
        while (end < block.size() && (block[end] == '\r' || block[end] == '\n')) end++;
        block.erase(at, end - at);
        at = block.find("<KEYFRAME>", at);
    }
    return block;
}

// common: B_RecompItem, a button made of the option rows' CHK_Default (frames 0 enabled, 20 disabled, 30 selected,
// which are a button's too): its bar `back`, its label LBL_Text as ItemText and a copy of it on the right, ItemValue
bool SpliceItem(std::string& xml) {
    if (xml.find("<NAME>" + std::string(kItem) + "</NAME>") != std::string::npos) return false;
    std::optional<Span> check_at = Block(xml, "CheckBox", "CHK_Default", 0);
    if (!check_at) return false;
    std::string check = Cut(xml, *check_at);
    size_t children = check.find(kChildrenOpen);
    if (children == std::string::npos) return false;
    std::optional<Span> back_at = Block(check, "IMAGE", "back", 0);
    if (!back_at) return false;
    std::optional<Span> label_at = Block(check, "TEXT", "LBL_Text", 0);
    if (!label_at) return false;
    std::string_view nl = NewLine(xml);
    size_t head_length = children + kChildrenOpen.size() - kCheckBoxOpen.size();
    std::string head = "<Button>" + check.substr(kCheckBoxOpen.size(), head_length);
    std::string back = KeepFrames(Cut(check, *back_at), {0, 20, 30});
    std::string label = KeepFrames(Cut(check, *label_at), {0, 20, 30});
    std::string value = label;
    if (!SetTag(head, "ID", std::to_string(Id(kItem))) || !SetTag(head, "NAME", kItem)) return false;
    SetTag(label, "ID", std::to_string(Id("ItemText")));
    SetTag(label, "NAME", "ItemText");
    SetEvery(label, "RIGHT", "400");
    SetTag(value, "ID", std::to_string(Id("ItemValue")));
    SetTag(value, "NAME", "ItemValue");
    SetEvery(value, "LEFT", "260");
    SetEvery(value, "RIGHT", "525");
    SetEvery(value, "ALIGNMENT", "RIGHT");
    std::string n(nl);
    std::string item = head + n + back + n + label + n + value + n + "</CHILDREN>" + n + "<TIMINGS>" + n +
                       "<ENABLED>0</ENABLED>" + n + "<PRESSED>10</PRESSED>" + n + "<DISABLED>20</DISABLED>" + n +
                       "<SELECTED>30</SELECTED>" + n + "<OVER>40</OVER>" + n + "</TIMINGS>" + n + "</Button>";
    xml.insert(check_at->end, n + item);
    return true;
}

std::string ListXml(const List& l, std::string_view nl) {
    std::string keyframes;
    for (auto [idx, alpha] : {std::pair{0, 0}, std::pair{3, 255}}) {
        const char* interpolation = idx ? "Linear" : "None";
        keyframes += "<KEYFRAME>\n";
        keyframes += "<IDX>" + std::to_string(idx) + "</IDX>\n";
        keyframes += "<INTERPOLATION>" + std::string(interpolation) + "</INTERPOLATION>\n";
        keyframes += "<POSITION>" + std::string(l.position) + "</POSITION>\n";
        keyframes += "<ROTATION>0</ROTATION>\n";
        keyframes += "<STATECOLOR>255 255 255 " + std::to_string(alpha) + "</STATECOLOR>\n";
        keyframes += "<ACTIONS/>\n";
        keyframes += "</KEYFRAME>\n";
    }
    std::string list = "<LIST>\n";
    list += "<ID>" + std::to_string(Id(l.list)) + "</ID>\n";
    list += "<NAME>" + std::string(l.list) + "</NAME>\n";
    list += "<HIDDEN>0</HIDDEN>\n"
            "<LOCKED>0</LOCKED>\n"
            "<MASKMODE>NOMASK</MASKMODE>\n"
            "<KEYFRAMES>\n";
    list += keyframes;
    list += "</KEYFRAMES>\n"
            "<KEYFRAMESOPT>1</KEYFRAMESOPT>\n";
    list += "<BUTTONCOUNT>" + std::to_string(l.rows) + "</BUTTONCOUNT>\n";
    list += "<HEADERFOOTERPOS>Top and Bottom</HEADERFOOTERPOS>\n"
            "<ITEMLINK>\n"
            "<PACKAGE>common</PACKAGE>\n";
    list += "<AREA>" + std::to_string(Id(l.item)) + "</AREA>\n";
    list += "</ITEMLINK>\n"
            "<HEADERLINK/>\n"
            "<FOOTERLINK/>\n"
            "<NEIGHBORS>\n"
            "<UP>NULL</UP>\n"
            "<DOWN>NULL</DOWN>\n"
            "<LEFT>NULL</LEFT>\n"
            "<RIGHT>NULL</RIGHT>\n"
            "</NEIGHBORS>\n"
            "<DEFAULT>1</DEFAULT>\n"
            "</LIST>\n";
    std::string out;
    for (char c : list) {
        if (c == '\n') out += nl;
        else out += c;
    }
    return out;
}

// erases from `begin` to the first `close` after it, with the line break after that
void EraseBlock(std::string& xml, size_t begin, const std::string& close, std::string_view nl) {
    size_t end = xml.find(close, begin) + close.size();
    if (xml.compare(end, nl.size(), nl) == 0) end += nl.size();
    xml.erase(begin, end - begin);
}

// every <tag> block
void DropEvery(std::string& xml, std::string_view tag, std::string_view nl) {
    std::string open = "<" + std::string(tag) + ">";
    std::string close = "</" + std::string(tag) + ">";
    for (size_t begin = xml.find(open); begin != std::string::npos; begin = xml.find(open))
        EraseBlock(xml, begin, close, nl);
}

// every <tag> block named `name`
void DropNamed(std::string& xml, std::string_view tag, std::string_view name, std::string_view nl) {
    std::string close = "</" + std::string(tag) + ">";
    for (std::optional<Span> s = Block(xml, tag, name, 0); s; s = Block(xml, tag, name, 0))
        EraseBlock(xml, s->begin, close, nl);
}

// the <Page> block holding <NAME>name</NAME>, npos ends when there is none
Span PageOf(const std::string& xml, std::string_view name) {
    size_t at = xml.find("<NAME>" + std::string(name) + "</NAME>");
    if (at == std::string::npos) return {at, at};
    return {xml.rfind("<Page>", at), xml.find(kPageClose, at)};
}

// a copy of page `source` after it, without its option rows (buttons, checkboxes, the aiming slider with its label,
// value and bar) and with list `l` instead
bool AddPage(std::string& xml, std::string_view source, const List& l, std::string_view nl) {
    Span page = PageOf(xml, source);
    if (page.begin == std::string::npos || page.end == std::string::npos) return false;
    page.end += kPageClose.size();
    std::string ours = Cut(xml, page);
    DropEvery(ours, "BUTTONINST", nl);
    DropEvery(ours, "CHECKBOXINST", nl);
    DropEvery(ours, "SLIDER", nl);
    DropNamed(ours, "TEXT", "Lbl_Aiming", nl);
    DropNamed(ours, "TEXT", "Lbl_AimingValue", nl);
    DropNamed(ours, "IMAGE", "back", nl);
    if (!SetTag(ours, "ID", std::to_string(Id(l.page))) || !SetTag(ours, "NAME", l.page)) return false;
    size_t children = ours.rfind("</CHILDREN>");
    if (children == std::string::npos) return false;
    ours.insert(children, ListXml(l, nl));
    xml.insert(page.end, std::string(nl) + ours);
    return true;
}

// Options: our buttons after Online, and our pages
bool SpliceOptions(std::string& xml) {
    size_t page = xml.find("<NAME>P_Options</NAME>");
    if (page == std::string::npos) return false;
    if (xml.find("<NAME>" + std::string(kRowList.page) + "</NAME>") != std::string::npos) return false;
    std::optional<Span> online_at = Block(xml, "BUTTONINST", "Online", page);
    if (!online_at) return false;
    std::string online = Cut(xml, *online_at);
    std::string added;
    std::string_view up = "Online";
    for (size_t i = 0; i < std::size(kButtons); i++) {
        std::string b = online;
        std::string_view down = i + 1 < std::size(kButtons) ? kButtons[i + 1].name : "Game";
        if (!SetTag(b, "ID", std::to_string(Id(kButtons[i].name))) || !SetTag(b, "NAME", kButtons[i].name) ||
            !SetTag(b, "LABEL", kButtons[i].label) || !SetTag(b, "UP", up) || !SetTag(b, "DOWN", down))
            return false;
        MoveDown(b, kRowStep * int(i + 1));
        added += b;
        up = kButtons[i].name;
    }
    if (!SetTag(online, "DOWN", kButtons[0].name)) return false;
    xml.replace(online_at->begin, online_at->end - online_at->begin, online + added);
    std::optional<Span> game_at = Block(xml, "BUTTONINST", "Game", page);
    if (!game_at || !SetTag(xml, "UP", kButtons[std::size(kButtons) - 1].name, game_at->begin)) return false;

    std::string_view nl = NewLine(xml);
    if (!AddPage(xml, "P_Options_Game", kRowList, nl) || !AddPage(xml, "P_Options", kSectionList, nl)) return false;
    return true;
}

// xboxlive: our popup, a copy of P_TeamsActionPopup with its list where the map list's popup has it
bool SplicePopup(std::string& xml) {
    Span page = PageOf(xml, "P_TeamsActionPopup");
    if (page.begin == std::string::npos || page.end == std::string::npos) return false;
    if (xml.find("<NAME>" + std::string(kChoiceList.page) + "</NAME>") != std::string::npos) return false;
    page.end += kPageClose.size();
    std::string ours = Cut(xml, page);
    if (!SetTag(ours, "ID", std::to_string(Id(kChoiceList.page))) || !SetTag(ours, "NAME", kChoiceList.page))
        return false;
    std::optional<Span> fond_at = Block(ours, "IMAGE", "fond", 0);
    if (!fond_at) return false;
    std::string fond = Cut(ours, *fond_at);
    SetEvery(fond, "TOP", kPopupBackground[0]);
    SetEvery(fond, "BOTTOM", kPopupBackground[1]);
    ours.replace(fond_at->begin, fond_at->end - fond_at->begin, fond);
    std::optional<Span> list_at = Block(ours, "LIST", "list", 0);
    if (!list_at) return false;
    std::string list = Cut(ours, *list_at);
    if (!SetTag(list, "ID", std::to_string(Id(kChoiceList.list))) || !SetTag(list, "NAME", kChoiceList.list) ||
        !SetTag(list, "BUTTONCOUNT", std::to_string(kChoiceList.rows)))
        return false;
    SetEvery(list, "POSITION", kChoiceList.position);
    ours.replace(list_at->begin, list_at->end - list_at->begin, list);
    xml.insert(page.end, std::string(NewLine(xml)) + ours);
    return true;
}

// every <tag> closed by its </tag> in order (<tag/> stands alone): a splice gone wrong would send the game's
// parser past its first node
bool Balanced(std::string_view xml) {
    std::vector<std::string_view> open;
    for (size_t at = xml.find('<'); at != std::string_view::npos; at = xml.find('<', at + 1)) {
        size_t end = xml.find('>', at);
        if (end == std::string_view::npos) return false;
        std::string_view tag = xml.substr(at + 1, end - at - 1);
        if (tag.empty() || tag.back() == '/') continue;
        if (tag[0] != '/') {
            open.push_back(tag);
        } else {
            if (open.empty() || open.back() != tag.substr(1)) return false;
            open.pop_back();
        }
    }
    return open.empty();
}

// text

std::u16string Caps(std::u16string text) {
    for (char16_t& c : text)
        if ((c >= u'a' && c <= u'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) c -= 0x20;
    return text;
}

std::u16string Translated(std::string_view text) {
    return app::Translate(text, [](std::string_view rest) { return std::string(rest); });
}

// a row's text in the game's language, in its title case
std::u16string RowText(std::string_view text) { return app::Translate(text, app::TitleCase); }

// a lasting guest copy: Magma keeps the pointers it is given, so each text is made once
uint32_t Guest(const std::u16string& text) {
    static std::map<std::u16string, uint32_t> guest;
    uint32_t& va = guest[text];
    if (!va) {
        va = mem::AllocSystem(uint32_t(text.size() + 1) * 2);
        for (size_t i = 0; i < text.size(); i++) W16(va + uint32_t(i) * 2, uint16_t(text[i]));
        W16(va + uint32_t(text.size()) * 2, 0);
    }
    return va;
}

// the pages

std::u16string Titled(std::string_view text) {
    const char16_t* t = app::Translation(text);
    if (t) return t;
    std::string titled = app::TitleCase(text);
    return std::u16string(titled.begin(), titled.end());
}

// a row a section's list shows
bool Listed(const app::rows::Row& r) {
    using app::rows::Kind;
    if (r.kind == Kind::Value) return app::rows::Shown(r);
    return r.kind == Kind::Bind || r.kind == Kind::PadBind;
}

// the button's sections that have rows to list, as their headings' rows (versus hides the gyro)
std::vector<int> ListedSections(int mode) {
    size_t count = 0;
    const app::rows::Row* rows = app::rows::Page(mode, &count);
    std::vector<int> out;
    for (std::string_view name : kSections[mode]) {
        for (size_t i = 0; i < count; i++) {
            if (rows[i].kind != app::rows::Kind::Heading || rows[i].label != name) continue;
            bool any = false;
            for (size_t j = i + 1; !any && j < count && rows[j].kind != app::rows::Kind::Heading; j++)
                any = Listed(rows[j]);
            if (any) out.push_back(int(i));
            break;
        }
    }
    return out;
}

// an instance's place: its keyframes' (+0x18 begin, end; keyframe +0x18) and its widget's (+4) position frames
// (vtable 0x361FC8, +0xE y, page units)
constexpr uint32_t kKeyframes = 0x18;
constexpr uint32_t kKeyframesEnd = 0x1C;
constexpr uint32_t kKeyFrame = 0x18;
constexpr uint32_t kWidgetFrame = 0x04;
constexpr uint32_t kPositionFrameVtable = 0x361FC8;
constexpr uint32_t kFrameY = 0x0E;
constexpr uint32_t kMaxKeyframes = 64;

void SetFrameY(uint32_t frame, int y) {
    if (frame && R32(frame) == kPositionFrameVtable) W16(frame + kFrameY, uint16_t(int16_t(y)));
}

void SetY(uint32_t inst, int y) {
    uint32_t begin = R32(inst + kKeyframes);
    uint32_t end = R32(inst + kKeyframesEnd);
    if (begin && end >= begin && end - begin <= kMaxKeyframes * 4) {
        for (uint32_t it = begin; it < end; it += 4)
            if (uint32_t key = R32(it)) SetFrameY(R32(key + kKeyFrame), y);
    }
    if (uint32_t widget = R32(inst + kInstanceWidget)) SetFrameY(R32(widget + kWidgetFrame), y);
}

uint32_t CallThis(X86Context& ctx, uint32_t fn, uint32_t self, std::initializer_list<uint32_t> args = {}) {
    ctx.ecx = self;
    return CallGuest(ctx, fn, args);
}

// ecx is set to the page even when it is 0 and nothing is called
uint32_t FindInstance(X86Context& ctx, uint32_t page, uint32_t id) {
    ctx.ecx = page;
    return page ? CallGuest(ctx, kFind, {id}) : 0;
}

uint32_t ListOf(X86Context& ctx, uint32_t page, std::string_view name) {
    uint32_t inst = FindInstance(ctx, page, Id(name));
    return inst ? R32(inst + kInstanceWidget) : 0;
}

// the page is built once with its package: the title is set for what opened it
void SetTitle(X86Context& ctx, uint32_t page, const std::u16string& text) {
    uint32_t title = FindInstance(ctx, page, Id("Title"));
    if (title && R32(title + kInstanceWidget)) W32(R32(title + kInstanceWidget) + kTextString, Guest(text));
}

void FillSections(X86Context& ctx, uint32_t menu) {
    uint32_t page = R32(menu + kMenuPage);
    uint32_t list = ListOf(ctx, page, kSectionList.list);
    int mode = int(R32(menu + kMenuMode));
    SetTitle(ctx, page, Titled(kButtons[mode].label));
    if (!list) {
        LOGI("versus options: no list on %s", std::string(kSectionList.page).c_str());
        return;
    }
    uint32_t label = CallThis(ctx, kPart, list, {Id("ItemText")});
    CallThis(ctx, kClear, list);
    // in capitals as the Options rows
    size_t count = 0;
    const app::rows::Row* rows = app::rows::Page(mode, &count);
    for (int heading : ListedSections(mode)) {
        uint32_t row = CallThis(ctx, kAddItem, list, {kEmpty, 0});
        CallThis(ctx, kSetText, list, {row, label, Guest(Caps(Translated(rows[heading].label)))});
    }
    g_sections = menu;
}

// the rows' values, as P_Options_Game writes its rows; key and button names are not translated: the controller's
// BACK is no Back
void WriteValues(X86Context& ctx) {
    using app::rows::Kind;
    constexpr uint32_t kMaxColumns = 16;
    if (!g_rows.list || g_rows.value >= kMaxColumns) return;
    for (size_t i = 0; i < g_rows.rows.size(); i++) {
        if (!g_rows.rows[i] || g_rows.rows[i] == &kGap) {
            CallThis(ctx, kSetText, g_rows.list, {uint32_t(i), g_rows.value, kEmpty});
            continue;
        }
        const app::rows::Row& r = *g_rows.rows[i];
        std::string v = app::rows::Value(r);
        std::string key = app::TitleCase(v);
        bool translated = r.kind == Kind::Value || v == "UNBOUND";
        std::u16string text = translated ? RowText(v) : std::u16string(key.begin(), key.end());
        CallThis(ctx, kSetText, g_rows.list, {uint32_t(i), g_rows.value, Guest(text)});
    }
}

// the rows our list shows, into g_rows
void CollectRows(const app::rows::Row* rows, size_t count, int section, bool in_section, bool restore) {
    using app::rows::Kind;
    // a section's rows up to the next heading, else all with a blank row for each heading but the first; MOUSE AND
    // KEYBOARD and CONTROLLER then a blank row and RESTORE DEFAULTS
    for (size_t i = in_section ? size_t(section) + 1 : 0; i < count; i++) {
        const app::rows::Row& r = rows[i];
        if (r.kind == Kind::Heading && in_section) break;
        if (r.kind == Kind::Heading && !g_rows.rows.empty()) g_rows.rows.push_back(&kGap);
        if (r.kind == Kind::Heading || r.kind == Kind::Text || (r.kind == Kind::Value && !app::rows::Shown(r)))
            continue;
        g_rows.rows.push_back(&r);
    }
    if (restore) {
        g_rows.rows.push_back(&kGap);
        g_rows.rows.push_back(nullptr);
    }
}

// one page serves them all: its list's place is set for each
void PlaceList(X86Context& ctx, uint32_t page, bool in_section) {
    if (uint32_t inst = CallThis(ctx, kFind, page, {Id(kRowList.list)})) {
        int rows_shown = int(std::min<size_t>(g_rows.rows.size(), size_t(kRowList.rows)));
        int middle = std::max(kRowListY, kFrameMiddle - rows_shown * kRowHeight / 2);
        SetY(inst, in_section ? middle : kRowListY);
    }
}

// the list's rows with their labels
void AddRows(X86Context& ctx, uint32_t list, uint32_t label) {
    for (const app::rows::Row* r : g_rows.rows) {
        uint32_t row = CallThis(ctx, kAddItem, list, {kEmpty, 0});
        const char* name = r ? r->label : "RESTORE DEFAULTS";
        uint32_t text = r == &kGap ? kEmpty : Guest(RowText(name));
        CallThis(ctx, kSetText, list, {row, label, text});
    }
}

void FillRows(X86Context& ctx, uint32_t menu) {
    int mode = int(R32(menu + kMenuMode));
    int section = int(R32(menu + kMenuSection));
    uint32_t page = R32(menu + kMenuPage);
    uint32_t list = ListOf(ctx, page, kRowList.list);
    size_t count = 0;
    const app::rows::Row* rows = app::rows::Page(mode, &count);
    bool in_section = section >= 0 && size_t(section) < count;
    SetTitle(ctx, page, Titled(in_section ? rows[section].label : kButtons[mode].label));
    if (!list) {
        LOGI("versus options: no list on %s", std::string(kRowList.page).c_str());
        return;
    }
    uint32_t label = CallThis(ctx, kPart, list, {Id("ItemText")});
    uint32_t value = CallThis(ctx, kPart, list, {Id("ItemValue")});
    CallThis(ctx, kClear, list);
    g_rows = {menu, list, value, mode, {}};
    CollectRows(rows, count, section, in_section, mode >= 1);
    PlaceList(ctx, page, in_section);
    AddRows(ctx, list, label);
    WriteValues(ctx);
    std::string button = std::string(kButtons[mode].label);
    const char* shown = in_section ? rows[section].label : button.c_str();
    LOGI("versus options: %s, %zu rows", shown, g_rows.rows.size());
}

void FillPopup(X86Context& ctx, uint32_t menu) {
    g_popup.menu = menu;
    uint32_t list = ListOf(ctx, R32(menu + kMenuPage), kChoiceList.list);
    if (!list) {
        LOGI("versus options: no list on %s", std::string(kChoiceList.page).c_str());
        return;
    }
    // xboxlive's B_OptionItem names its text ItemText (entry's, label)
    uint32_t label = CallThis(ctx, kPart, list, {Id("ItemText")});
    CallThis(ctx, kClear, list);
    // in capitals as the map list's popup
    for (const std::string& choice : g_popup.choices) {
        uint32_t row = CallThis(ctx, kAddItem, list, {kEmpty, 0});
        CallThis(ctx, kSetText, list, {row, label, Guest(Caps(Translated(choice)))});
    }
    CallThis(ctx, kListSelect, list, {uint32_t(g_popup.current), 1});
}

uint32_t Handler(HandlerKind kind);

// the list on our page of this kind
std::string_view ListName(uint32_t kind) {
    if (kind == kSectionsPage) return kSectionList.list;
    if (kind == kRowsPage) return kRowList.list;
    return kChoiceList.list;
}

// our pages' open (thiscall menu): the base one, then the list and its handler
void MenuOpen(X86Context& __restrict ctx, uint8_t*) {
    uint32_t menu = ctx.ecx;
    CallGuest(ctx, kBaseOpen, {});
    uint32_t kind = R32(menu + kMenuKind);
    if (kind == kSectionsPage) FillSections(ctx, menu);
    else if (kind == kRowsPage) FillRows(ctx, menu);
    else FillPopup(ctx, menu);
    // a page's list press: kSectionPick, kRowPick or kValuePick
    uint32_t h = Handler(HandlerKind(int(kind) + kSectionPick));
    W32(h + kHandlerRefcount, R32(h + kHandlerRefcount) + 1);  // the bind takes a counted reference
    CallThis(ctx, kBind, menu, {h, Id(ListName(kind))});
    ctx.esp += 4;
}

void ClosePopup(X86Context& ctx, uint32_t sound);
void Sound(X86Context& ctx, uint32_t sound);

// our menus' keys (thiscall menu (key, action, ?), ret 0xC): B goes back a page, as P_Options' does
void MenuKey(X86Context& __restrict ctx, uint8_t*) {
    uint32_t menu = ctx.ecx;
    uint32_t key = R32(ctx.esp + 4);
    uint32_t action = R32(ctx.esp + 8);
    uint32_t handled = 0;
    if (key == kJoyB && action == kRelease) {
        if (R32(menu + kMenuKind) == kPopupPage) {
            ClosePopup(ctx, kBackSound);
        } else {
            Sound(ctx, kBackSound);
            CallGuest(ctx, kPopMenu, {});
        }
        handled = 1;
    }
    ctx.eax = handled;
    ctx.esp += 16;
}

uint32_t MenuVtable() {
    static uint32_t vtable = 0;
    if (!vtable) {
        vtable = mem::AllocSystem(kMenuSlots * 4);
        for (uint32_t i = 0; i < kMenuSlots; i++) W32(vtable + i * 4, R32(kMenuVtable + i * 4));
        W32(vtable + kOpenSlot, dispatch::HostThunk(MenuOpen));
        W32(vtable + kLegendSlot, kOptionsLegend);
        W32(vtable + kKeySlot, dispatch::HostThunk(MenuKey));
    }
    return vtable;
}

// a guest copy of a page or package name, for its FString
uint32_t Name(std::string_view name) {
    static std::map<std::string_view, uint32_t> names;
    uint32_t& va = names[name];
    if (!va) {
        va = mem::AllocSystem(uint32_t(name.size()) + 1);
        for (size_t i = 0; i < name.size(); i++) G<uint8_t>(va + uint32_t(i)) = uint8_t(name[i]);
        G<uint8_t>(va + uint32_t(name.size())) = 0;
    }
    return va;
}

// a base menu on one of our pages, pushed as the stock buttons push theirs
void OpenPage(X86Context& ctx, const List& l, int mode, int section) {
    static uint32_t names = mem::AllocSystem(2 * kFStringSize);  // the package and page FStrings
    uint32_t menu = CallGuest(ctx, kMalloc, {kMenuSize, 0});
    if (!menu) return;
    std::memset(HostPtr(menu), 0, kMenuSize);
    CallThis(ctx, kFString, names, {Name(l.package)});
    CallThis(ctx, kFString, names + kFStringSize, {Name(l.page)});
    CallThis(ctx, kMenuCtor, menu, {names, names + kFStringSize});
    for (uint32_t s : {names, names + kFStringSize})
        if (uint32_t text = R32(s)) CallGuest(ctx, kFree, {text});
    W32(menu, MenuVtable());
    bool popup = &l == &kChoiceList;
    PageKind kind = kRowsPage;
    if (&l == &kSectionList) kind = kSectionsPage;
    else if (popup) kind = kPopupPage;
    W32(menu + kMenuKind, kind);
    W32(menu + kMenuMode, uint32_t(mode));
    W32(menu + kMenuSection, uint32_t(section));
    CallGuest(ctx, kPush, {menu, popup ? 0u : 1u, 1});
}

// the sound player's vtable +8
constexpr uint32_t kSoundPlayer = 0x3D8D78;
constexpr uint32_t kPlaySoundSlot = 0x08;

void Sound(X86Context& ctx, uint32_t sound) {
    if (uint32_t player = R32(kSoundPlayer)) CallThis(ctx, R32(R32(player) + kPlaySoundSlot), player, {sound});
}

int Selected(uint32_t list) { return list ? int(R32(list + kListSelection)) : -1; }

// A popup closing does not bring the page under it on top again as a page closing does (the page was never taken
// off), and its open hid the profile name and changed the legend: the rows menu's legend slot puts both back.
void ClosePopup(X86Context& ctx, uint32_t sound) {
    g_popup.row = nullptr;
    Sound(ctx, sound);
    CallGuest(ctx, kPopMenu, {});
    if (g_rows.menu) CallThis(ctx, kOptionsLegend, g_rows.menu, {0});
}

// a row of the rows page, as story's: step 0 is A, which flips an on / off row, opens the popup on a row with a list
// and steps the others; Left / Right (step -1 / 1) step those
bool Act(X86Context& ctx, int step) {
    using app::rows::Kind;
    int row = Selected(g_rows.list);
    if (row < 0 || row >= int(g_rows.rows.size()) || g_rows.rows[size_t(row)] == &kGap) return false;
    if (!g_rows.rows[size_t(row)]) {
        if (step) return false;
        app::rows::Defaults(g_rows.mode);
        WriteValues(ctx);
        Sound(ctx, kSelectSound);
        return true;
    }
    const app::rows::Row& r = *g_rows.rows[size_t(row)];
    if (r.kind != Kind::Value) return false;  // binding comes later
    int current = 0;
    std::vector<std::string> choices = app::rows::Choices(r, &current);
    bool flip = choices.size() == 2 && choices[0] == "OFF" && choices[1] == "ON";
    if (choices.size() > 1 && (!flip || step)) {
        if (step) return false;
        g_popup = {0, &r, std::move(choices), current};
        OpenPage(ctx, kChoiceList, 0, -1);
        return true;
    }
    app::rows::Step(r, step ? step : 1);
    WriteValues(ctx);
    Sound(ctx, kSelectSound);
    return true;
}

// a press (thiscall handler, one argument, ret 4): the handler's +0x10 is what it opens, 0..2 our buttons, 3 the
// section picked on the sections list, 4 a row, 5 the value picked in the popup
void Press(X86Context& __restrict ctx, uint8_t*) {
    int kind = int(R32(ctx.ecx + kHandlerKind));
    if (kind <= kControllerButton) {
        OpenPage(ctx, kSectionList, kind, -1);
    } else if (kind == kSectionPick) {
        uint32_t list = g_sections ? ListOf(ctx, R32(g_sections + kMenuPage), kSectionList.list) : 0;
        int mode = g_sections ? int(R32(g_sections + kMenuMode)) : 0;
        std::vector<int> sections = ListedSections(mode);
        int row = Selected(list);
        if (row >= 0 && row < int(sections.size())) OpenPage(ctx, kRowList, mode, sections[size_t(row)]);
    } else if (kind == kRowPick) {
        Act(ctx, 0);
    } else if (g_popup.row) {
        uint32_t list = g_popup.menu ? ListOf(ctx, R32(g_popup.menu + kMenuPage), kChoiceList.list) : 0;
        int pick = Selected(list);
        const app::rows::Row* r = g_popup.row;
        ClosePopup(ctx, kSelectSound);
        // the value it has would be applied again
        if (pick >= 0 && pick < int(g_popup.choices.size()) && pick != g_popup.current) app::rows::Choose(*r, pick);
        WriteValues(ctx);
    }
    ctx.eax = 1;
    ctx.esp += 8;
}

// Up / Down past a blank row: from the row before it to the one after, or off it when it was selected anyway (the
// mouse); the list moves by itself otherwise
bool PastGap(X86Context& ctx, int dir) {
    int row = Selected(g_rows.list);
    int rows = int(g_rows.rows.size());
    if (row < 0 || row >= rows) return false;
    auto gap = [&](int r) { return r >= 0 && r < rows && g_rows.rows[size_t(r)] == &kGap; };
    int to = -1;
    if (gap(row)) to = row + dir;
    else if (gap(row + dir)) to = row + 2 * dir;
    if (to < 0 || to >= rows) return false;
    CallThis(ctx, kListSelect, g_rows.list, {uint32_t(to), 1});
    return true;
}

// an event on our lists (thiscall handler, ?, event*, ret 8): Left / Right step a row, Up / Down pass the blank rows;
// our buttons leave theirs to P_Options
void Event(X86Context& __restrict ctx, uint8_t*) {
    int kind = int(R32(ctx.ecx + kHandlerKind));
    uint32_t event = R32(ctx.esp + 8);
    uint32_t key = event ? R32(event) : 0;
    uint32_t handled = 0;
    if (kind == kRowPick && (key == kLeft || key == kRight)) {
        handled = Act(ctx, key == kLeft ? -1 : 1);
    } else if (kind == kRowPick && (key == kUp || key == kDown)) {
        handled = PastGap(ctx, key == kUp ? -1 : 1);
    }
    ctx.eax = handled;
    ctx.esp += 12;
}

// the handlers, laid out as the Game button's {vtable, refcount, 1, 0x3380E0} plus what they open; never released
uint32_t Handler(HandlerKind kind) {
    constexpr uint32_t kHandlers = 6;
    constexpr uint32_t kSize = 0x18;
    static uint32_t handlers = 0;
    if (!handlers) {
        uint32_t vtable = mem::AllocSystem(kHandlerSlots * 4 + kSize * kHandlers);
        for (uint32_t i = 0; i < kHandlerSlots; i++) W32(vtable + i * 4, R32(kHandlerVtable + i * 4));
        W32(vtable + kPressSlot, dispatch::HostThunk(Press));
        W32(vtable + kEventSlot, dispatch::HostThunk(Event));
        handlers = vtable + kHandlerSlots * 4;
        for (uint32_t i = 0; i < kHandlers; i++) {
            uint32_t h = handlers + i * kSize;
            W32(h, vtable);
            W32(h + kHandlerRefcount, 0x10000);  // refcount: never reaches 0
            W32(h + kHandlerWord8, 1);
            W32(h + kHandlerWordC, kHandlerWordCValue);
            W32(h + kHandlerKind, i);
        }
    }
    return handlers + uint32_t(kind) * kSize;
}

// the package text with ours spliced in, as a guest copy now given to the reader; 0 when it is not changed
uint32_t SplicedCopy(X86Context& ctx, uint32_t text) {
    std::string_view view(static_cast<const char*>(HostPtr(text)));
    bool options = view.find("<NAME>P_Options</NAME>") != std::string_view::npos;
    bool common = view.find("<NAME>B_MainItem</NAME>") != std::string_view::npos;
    bool xboxlive = view.find("<NAME>P_TeamsActionPopup</NAME>") != std::string_view::npos;
    if (!options && !common && !xboxlive) return 0;
    const char* name = "xboxlive";
    if (options) name = "Options";
    else if (common) name = "common";
    std::string xml(view);
    // | and not ||: every splice that applies runs
    bool changed = (options && SpliceOptions(xml)) | (common && SpliceItem(xml)) | (xboxlive && SplicePopup(xml));
    if (changed && !Balanced(xml)) {
        LOGE("versus options: the spliced %s does not balance, the stock text is kept", name);
        changed = false;
    }
    uint32_t copy = 0;
    if (changed) copy = CallGuest(ctx, kMalloc, {uint32_t(xml.size() + 1), 0});
    if (copy) {
        std::memcpy(HostPtr(copy), xml.c_str(), xml.size() + 1);
        W32(ctx.esp + 4, copy);
    }
    LOGI("versus options: %s %s", name, copy ? "spliced" : "not changed");
    return copy;
}

}  // namespace

namespace versus {

// mouse.cpp, every update with the top page: a change from F10 shows at once on our rows page
void OptionsTick(X86Context& ctx, uint32_t top) {
    static uint32_t revision = 0;
    uint32_t r = app::SettingsRevision();
    if (r == revision || !top || R32(top + kPageId) != Id(kRowList.page) || !g_rows.list) return;
    revision = r;
    WriteValues(ctx);
}

// xbox_live.cpp's text lookup: our buttons' labels, in capitals as the stock rows, and our pages' titles until they
// open; 0 for the rest
uint32_t OptionsText(uint32_t section, uint32_t section2, uint32_t key) {
    auto in = [&](uint32_t page) { return section == page || section2 == page; };
    if ((in(Id(kRowList.page)) || in(Id(kSectionList.page))) && key == Id("Title"))
        return Guest(Titled(kButtons[0].label));
    if (!in(kOptionsPage)) return 0;
    for (const Button& b : kButtons)
        if (key == Id(b.name)) return Guest(Caps(Translated(b.label)));
    return 0;
}

}  // namespace versus

// the XML reader's parse (thiscall reader, text, ret 4): the text is copied in, so ours is freed after
X86_EXTERN_FUNC(__imp__sub_0026F0B0);
X86_FUNC(sub_0026F0B0) {
    uint32_t text = R32(ctx.esp + 4);
    uint32_t copy = 0;
    if (text) copy = SplicedCopy(ctx, text);
    __imp__sub_0026F0B0(ctx, base);
    if (copy) {
        uint32_t result = ctx.eax;
        CallGuest(ctx, kFree, {copy});
        ctx.eax = result;
    }
}

// P_Options' open (thiscall menu): the stock buttons get their handlers, ours theirs
X86_EXTERN_FUNC(__imp__sub_001AD030);
X86_FUNC(sub_001AD030) {
    uint32_t menu = ctx.ecx;
    __imp__sub_001AD030(ctx, base);
    for (int i = 0; i < int(std::size(kButtons)); i++) {
        uint32_t h = Handler(HandlerKind(i));
        W32(h + kHandlerRefcount, R32(h + kHandlerRefcount) + 1);  // the bind takes a counted reference
        CallThis(ctx, kBind, menu, {h, Id(kButtons[i].name)});
    }
}
