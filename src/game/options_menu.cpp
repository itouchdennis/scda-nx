// RECOMP SETTINGS, MOUSE AND KEYBOARD and CONTROLLER under Shadow Calibration in the options menu. Each opens the
// Display page (P_Video) with the F10 panel's rows in its list until B: A opens the list of a row's values (an on / off
// row flips) or rebinds; a range (no list) steps on Left, Right and A. Each first lists its sections (the F10
// headings named here) on P_Options itself, drawn as the stock two-bar pages (Solo > Load Game).
#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "app/app.h"
#include "app/translations.h"
#include "core/input.h"
#include "game/magma.h"
#include "xdk/hle.h"

using namespace kernel;

namespace dispatch {
uint32_t HostThunk(X86Func* fn);
}  // namespace dispatch

namespace {

// COptionsMenu: +8 the listener its OnPress, OnBack and OnKey get, +0x14 owner, +0x28 P_Options, +0x2C its list,
// +0x44 P_Video, +0x48 its list. Owner +0x14 is set when a key was taken.
constexpr uint32_t kListener = 8;
constexpr uint32_t kOwner = 0x14;
constexpr uint32_t kOptionsList = 0x2C;
constexpr uint32_t kVideo = 0x44;
constexpr uint32_t kVideoList = 0x48;
constexpr uint32_t kTaken = 0x14;
constexpr uint32_t kOptionsName = 0x545D74;
constexpr uint32_t kVideoName = 0x545D4C;
constexpr uint32_t kListName = 0x545E04;
constexpr uint32_t kValidateName = 0x545E10;
constexpr uint32_t kPopupName = 0x54638C;
constexpr uint32_t kTitleName = 0x5420F8;
constexpr uint32_t kSection = 0x546318;
constexpr uint32_t kVideoTitle = 0x546238;
constexpr uint32_t kPackage = 0x541544;
constexpr uint32_t kIsPage = 0x28FA86;      // thiscall owner (name): that page has the screen
constexpr uint32_t kIsElement = 0x1A6090;   // cdecl (instance, name)
constexpr uint32_t kFind = 0x17D0E8;        // cdecl (container, name): instance, its element at +8
constexpr uint32_t kSetTitle = 0x1A6235;    // cdecl (page, element, section, key, package)
constexpr uint32_t kFillVideo = 0x1A4012;   // thiscall menu
constexpr uint32_t kFillOptions = 0x1A53B5; // thiscall menu
constexpr uint32_t kResetFocus = 0x2C0730;  // cdecl (page)
constexpr uint32_t kShow = 0x292318;        // thiscall owner (page, show, 1, 0)
constexpr uint32_t kSetText = 0x183598;     // thiscall FString (text)
constexpr uint32_t kSetEnabled = 0x2C87E0;  // thiscall instance (bool)
constexpr uint32_t kSoundPlayer = 0x1838BD;
constexpr uint32_t kSoundOwner = 0x17B720;
// the player's vtable +0x78 plays (sound, owner +4's +0x5C, 3, 0, 0, 0, 0), the sound at +0x2AC of the sound owner
constexpr uint32_t kPlay = 0x78;
constexpr uint32_t kSelectSound = 0x2AC;
constexpr uint32_t kMenuPlayer = 0x04;
constexpr uint32_t kPlayerSlot = 0x5C;
// the stock option popup as a Difficulty press opens it, through the Display options (menu +0x88) and their popup
// controller at +0x18. Open (thiscall options, FString array, row, option, ret 0xC; only the options' entry is a
// function) lists the array; the option's vtable is called back, +0 with the row picked and +4 on B (thiscall row,
// item, FString*, ret 0xC).
constexpr uint32_t kVideoOptions = 0x88;
constexpr uint32_t kPopupController = 0x18;
constexpr uint32_t kOpenPopup = 0x1BF28E;
constexpr uint32_t kOnCancel = 4;
constexpr uint32_t kPopupList = 0x88 + 8;  // controller: +0x88 the popup, its +8 list
constexpr uint32_t kPopupSize = 0x4000;
constexpr uint32_t kMaxChoices = 128;  // brightness and contrast list 101, field of view 96
// the options' base vtable (15 slots): the popup also calls +8 as its selection moves
constexpr uint32_t kOptionVtable = 0x547C24;
constexpr uint32_t kOptionSlots = 15;
// list: thiscall Clear, AddItem(text, 0), SetCell(row, column, text), Layout, Select(row, 0, 1), ColumnCount on
// +0x6C; +0x60 items begin, +0x64 end (0x38 each), +0x9A rows shown, +0x9C selection
constexpr uint32_t kClear = 0x2C4D00;
constexpr uint32_t kAddItem = 0x2C5160;
constexpr uint32_t kSetCell = 0x2C40D0;
constexpr uint32_t kLayout = 0x2C1710;
constexpr uint32_t kSelect = 0x2C2040;
constexpr uint32_t kColumns = 0x18EBFD;
constexpr uint32_t kColumnsPart = 0x6C;
constexpr uint32_t kItems = 0x60;
constexpr uint32_t kItemsEnd = 0x64;
constexpr uint32_t kRowsShown = 0x9A;
constexpr uint32_t kSelection = 0x9C;
constexpr uint32_t kItemSize = 0x38;
// instance: +0x18 bit 0 visible, +0x4C flags
// instance: +4 its id, +8 its widget
constexpr uint32_t kInstanceId = 4;
constexpr uint32_t kInstanceWidget = 8;
constexpr uint32_t kVisible = 0x18;
constexpr uint32_t kFlags = 0x4C;
// a box of instances (a page's, an area's children): +8 begin, +0xC end
constexpr uint32_t kBoxBegin = 8;
constexpr uint32_t kBoxEnd = 0xC;
// virtual key codes
constexpr uint32_t kLeft = 0x25;
constexpr uint32_t kRight = 0x27;
constexpr uint32_t kEscape = 0x1B;
const char* const kCaptions[] = {"Recomp Settings", "Mouse and Keyboard", "Controller"};
// the captions listed, by mode: the Switch has no mouse or keyboard
#ifdef SCDA_SWITCH
constexpr int kListedModes[] = {0, 2};
#else
constexpr int kListedModes[] = {0, 1, 2};
#endif
constexpr int kCaptionCount = int(std::size(kListedModes));
// per caption
const std::vector<std::string_view> kSections[] = {
    {"DISPLAY", "GRAPHICS", "GENERAL"},
    {"MOUSE", "KEYBOARD"},
    {"GENERAL", "GYRO", "BUTTONS"},
};
static_assert(std::size(kSections) == std::size(kCaptions));

// as the game shows it: in the game's language, what has no translation (numbers, units) in title case
std::u16string Text(std::string_view text) { return app::Translate(text, app::TitleCase); }

// a row a section's list shows
bool Listed(const app::rows::Row& r) {
    using app::rows::Kind;
    if (r.kind == Kind::Value) return app::rows::Shown(r);
    return r.kind == Kind::Bind || r.kind == Kind::PadBind;
}

// a key or button name as it is: the controller's BACK is no Back button
std::u16string Name(std::string_view text) {
    std::string titled = app::TitleCase(text);
    return std::u16string(titled.begin(), titled.end());
}

struct Take {
    int mode = -1;
    uint32_t menu = 0;
    uint32_t owner = 0;
    uint32_t page = 0;
    uint32_t list = 0;
    bool seen = false;
    bool leaving = false;
    std::vector<const app::rows::Row*> rows;  // per list row, null restores defaults
    int section = -1;                         // RECOMP SETTINGS: the heading shown
    bool refill = false;                      // a pick can change which rows show
    int last_row = 0;                         // the selection last tick, which way it moved onto a gap
};
Take g_take;  // game thread

// a blank row between the MOUSE and KEYBOARD rows: the lists have no divider of their own
const app::rows::Row kGap{app::rows::Kind::Heading, -1, ""};
int g_first = -1;          // P_Options row of RECOMP SETTINGS
uint8_t g_shown = 0;       // P_Video's own rows shown
constexpr size_t kRowsMax = 14;  // ours shown at once, from kListTop to the frame's bottom
// a list's place is its +0x10 x, +0x12 y (GetPosition `sub_002C1030` on its +8 part); ours starts right under the
// titles, where the stock page leaves room above its few rows
constexpr uint32_t kListY = 0x12;
constexpr int16_t kListTop = 116;
// P_Video's own (options.mgm): before that page has been shown the list's y is still 0, so it is not read back
constexpr int16_t kStockListY = 205;
constexpr int16_t kRowHeight = 20;
// a long section (KEYBOARD, BUTTONS) starts under the titles, a short one sits in the middle of the room under them,
// which the stock place is for its few rows
int16_t ListTop() {
    if (g_take.rows.empty()) return kStockListY;
    size_t rows = std::min(g_take.rows.size(), kRowsMax);
    return int16_t(kListTop + int(kRowsMax - rows) * kRowHeight / 2);
}
uint8_t g_validate[2] = {};
bool g_validate_saved = false;
bool g_restore = false;
// P_Video's upper title (L_Text, "OPTIONS"): RECOMP SETTINGS shows its name there and the section's below
constexpr uint32_t kTopTextId = 0xC9E6E44B;
std::u16string g_top_text;
bool g_top_saved = false;
int g_capture = -1;
bool g_armed = false;
uint32_t g_text = 0;
uint32_t g_popup = 0;  // our option (+0) and its vtable (+0x20), the array (+0x60), its strings, their text
constexpr uint32_t kPopupVtable = 0x20;
constexpr uint32_t kPopupArray = 0x60;
int g_popup_row = -1;
int g_popup_current = 0;
constexpr uint32_t kWideSize = 0x400;
constexpr uint32_t kMaxWideChars = 0x1FF;

uint32_t Wide16(const std::u16string& text) {
    if (!g_text) g_text = mem::AllocSystem(kWideSize);
    size_t n = std::min<size_t>(text.size(), kMaxWideChars);
    for (size_t i = 0; i < n; i++) W16(g_text + uint32_t(i) * 2, uint16_t(text[i]));
    W16(g_text + uint32_t(n) * 2, 0);
    return g_text;
}

// a text widget's wstring at +0x2C: +4 its characters (a pointer from 8 on), +0x14 length, +0x18 capacity
constexpr uint32_t kTextString = 0x2C;
constexpr uint32_t kStringData = 4;
constexpr uint32_t kStringLength = 0x14;
constexpr uint32_t kStringCapacity = 0x18;
constexpr uint32_t kStringInline = 8;
constexpr uint32_t kMaxReadChars = 0x100;
std::u16string ReadWide(uint32_t text) {
    uint32_t string = text + kTextString;
    uint32_t length = std::min<uint32_t>(R32(string + kStringLength), kMaxReadChars);
    uint32_t data = R32(string + kStringCapacity) >= kStringInline ? R32(string + kStringData) : string + kStringData;
    std::u16string out;
    for (uint32_t i = 0; i < length; i++) out += char16_t(R16(data + i * 2));
    return out;
}

uint32_t PageChild(uint32_t page, uint32_t id) {
    uint32_t begin = page ? R32(page + kBoxBegin) : 0;
    uint32_t end = page ? R32(page + kBoxEnd) : 0;
    if (!begin || end < begin || end - begin > 4 * 512) return 0;
    for (uint32_t it = begin; it < end; it += 4)
        if (uint32_t instance = R32(it); instance && R32(instance + kInstanceId) == id) return instance;
    return 0;
}

int Count(uint32_t list) {
    uint32_t begin = R32(list + kItems);
    uint32_t end = R32(list + kItemsEnd);
    return begin && end > begin ? int((end - begin) / kItemSize) : 0;
}

bool IsPage(X86Context& ctx, uint32_t owner, uint32_t name) {
    ctx.ecx = owner;
    return CallGuest(ctx, kIsPage, {name}) & 0xFF;
}

void Sound(X86Context& ctx, uint32_t owner) {
    ctx.ecx = owner;
    uint32_t player = CallGuest(ctx, kSoundPlayer, {});
    if (!player) return;
    uint32_t sounds = CallGuest(ctx, kSoundOwner, {});
    ctx.ecx = player;
    CallGuest(ctx, R32(R32(player) + kPlay),
              {R32(sounds + kSelectSound), R32(R32(owner + kMenuPlayer) + kPlayerSlot), 3, 0, 0, 0, 0});
}

void Unblock() { input::SetBlocked(app::MenuOpen() || app::ConsoleOpen()); }

void Refresh(X86Context& ctx) {
    ctx.ecx = g_take.list + kColumnsPart;
    bool two = int8_t(CallGuest(ctx, kColumns, {})) > 1;
    for (size_t i = 0; i < g_take.rows.size(); i++) {
        const app::rows::Row* r = g_take.rows[i];
        std::u16string label = Text(r ? r->label : "RESTORE DEFAULTS");
        std::u16string value;
        const char* prompt = r && r->kind == app::rows::Kind::PadBind ? "PRESS A BUTTON" : "PRESS A KEY";
        if (r && g_capture == int(i)) value = Text(prompt);
        else if (r) {
            std::string v = app::rows::Value(*r);
            value = r->kind == app::rows::Kind::Value || v == "UNBOUND" ? Text(v) : Name(v);
        }
        ctx.ecx = g_take.list;
        if (!two) {
            CallGuest(ctx, kSetCell, {uint32_t(i), 0, Wide16(r ? label + u"  " + value : label)});
            continue;
        }
        // as the stock rows: the item's own text is not drawn
        CallGuest(ctx, kSetCell, {uint32_t(i), 0, Wide16(label)});
        ctx.ecx = g_take.list;
        CallGuest(ctx, kSetCell, {uint32_t(i), 1, Wide16(value)});
    }
}

void Picked(X86Context& __restrict ctx, uint8_t*) {
    int pick = int(R32(ctx.esp + 4));
    const app::rows::Row* r = g_take.mode >= 0 && g_popup_row >= 0 && g_popup_row < int(g_take.rows.size())
                                  ? g_take.rows[size_t(g_popup_row)] : nullptr;
    // the value it has would be applied again
    if (r && pick != g_popup_current) {
        app::rows::Choose(*r, pick);
        Refresh(ctx);
        g_take.refill = true;
    }
    g_popup_row = -1;
    ctx.esp += 16;  // ret 0xC
}

void Cancelled(X86Context& __restrict ctx, uint8_t*) {
    g_popup_row = -1;
    ctx.esp += 16;  // ret 0xC
}

// stock option: +4 its options, +0x10 its value
constexpr uint32_t kOptionOptions = 4;
constexpr uint32_t kOptionValue = 0x10;
// FString, and the array of them: +0 data, +4 count, +8 capacity
constexpr uint32_t kFStringCount = 4;
constexpr uint32_t kFStringCapacity = 8;
constexpr uint32_t kFStringSize = 0xC;

// the choices' FStrings at strings, their text past room for kMaxChoices of them: the count that fit
uint32_t WriteChoices(uint32_t strings, const std::vector<std::string>& choices) {
    uint32_t text = strings + kMaxChoices * kFStringSize;
    uint32_t n = 0;
    for (const std::string& choice : choices) {
        std::u16string t = Text(choice);
        uint32_t length = uint32_t(t.size()) + 1;  // FString counts its terminator
        if (n == kMaxChoices || text + length * 2 > g_popup + kPopupSize) break;
        for (uint32_t i = 0; i < length; i++) W16(text + i * 2, i < t.size() ? uint16_t(t[i]) : 0);
        W32(strings + n * kFStringSize, text);
        W32(strings + n * kFStringSize + kFStringCount, length);
        W32(strings + n * kFStringSize + kFStringCapacity, length);
        text += length * 2;
        n++;
    }
    return n;
}

void Popup(X86Context& ctx, int row, const std::vector<std::string>& choices, int current) {
    if (!R32(g_take.menu + kVideoOptions + kPopupController)) {
        LOGI("options menu: no popup controller");
        return;
    }
    // laid out as a stock option (+4 its options, +0x10 its value) for the base methods
    if (!g_popup) {
        g_popup = mem::AllocSystem(kPopupSize);
        uint32_t vtable = g_popup + kPopupVtable;
        for (uint32_t i = 0; i < kOptionSlots; i++) W32(vtable + i * 4, R32(kOptionVtable + i * 4));
        W32(vtable, dispatch::HostThunk(Picked));
        W32(vtable + kOnCancel, dispatch::HostThunk(Cancelled));
        W32(g_popup, vtable);
    }
    W32(g_popup + kOptionOptions, g_take.menu + kVideoOptions);
    W32(g_popup + kOptionValue, uint32_t(current));
    uint32_t array = g_popup + kPopupArray;
    uint32_t strings = array + kFStringSize;  // right after the array's own header
    uint32_t n = WriteChoices(strings, choices);
    W32(array, strings);
    W32(array + kFStringCount, n);
    W32(array + kFStringCapacity, n);
    g_popup_row = row;
    g_popup_current = current;
    ctx.ecx = g_take.menu + kVideoOptions;
    CallGuest(ctx, kOpenPopup, {array, uint32_t(current), g_popup});
    // opening resets the page's focus, which puts its list back on the first row
    uint32_t list = R32(R32(g_take.menu + kVideoOptions + kPopupController) + kPopupList);
    if (list) {
        ctx.ecx = list;
        CallGuest(ctx, kSelect, {uint32_t(current), 0, 1});
    }
    LOGI("options menu: popup of %u for row %d", n, row);
}

void Fill(X86Context& ctx, int select = 0) {
    using app::rows::Kind;
    size_t count = 0;
    const app::rows::Row* page = app::rows::Page(g_take.mode, &count);
    g_take.rows.clear();
    for (size_t i = size_t(g_take.section) + 1; i < count && page[i].kind != Kind::Heading; i++)
        if (Listed(page[i])) g_take.rows.push_back(&page[i]);
    // RECOMP SETTINGS has no restore row, as before sections
    if (g_take.mode >= 1) {
        g_take.rows.push_back(&kGap);
        g_take.rows.push_back(nullptr);
    }
    uint32_t list = g_take.list;
    ctx.ecx = list;
    CallGuest(ctx, kClear, {});
    for (const app::rows::Row* r : g_take.rows)
        CallGuest(ctx, kAddItem, {Wide16(Text(r ? r->label : "RESTORE DEFAULTS")), 0});
    Refresh(ctx);
    G<uint8_t>(list + kRowsShown) = uint8_t(std::min<size_t>(g_take.rows.size(), kRowsMax));
    W16(list + kListY, uint16_t(ListTop()));
    ctx.ecx = list;
    CallGuest(ctx, kLayout, {});
    CallGuest(ctx, kSelect, {uint32_t(select), 0, 1});
}

// as 1A6235: text elements (type 8) keep it at +0x2C, buttons (2, 3, 4) at +0x28
constexpr uint32_t kGetType = 0xC;  // element vtable
constexpr uint32_t kTextType = 8;
constexpr uint32_t kFirstButtonType = 2;
constexpr uint32_t kLastButtonType = 4;
constexpr uint32_t kButtonString = 0x28;
void SetTitle(X86Context& ctx, uint32_t page, const std::u16string& text) {
    uint32_t instance = CallGuest(ctx, kFind, {page, kTitleName});
    uint32_t element = instance ? R32(instance + kInstanceWidget) : 0;
    if (!element) return;
    ctx.ecx = element;
    uint32_t type = CallGuest(ctx, R32(R32(element) + kGetType), {});
    if (type != kTextType && (type < kFirstButtonType || type > kLastButtonType)) return;
    ctx.ecx = element + (type == kTextType ? kTextString : kButtonString);
    CallGuest(ctx, kSetText, {Wide16(text)});
}

// the in-game Confirm Changes button
void RestoreStock(X86Context& ctx, uint32_t menu) {
    uint32_t page = R32(menu + kVideo);
    if (!page) return;
    CallGuest(ctx, kSetTitle, {page, kTitleName, kSection, kVideoTitle, kPackage});
    uint32_t top = g_top_saved ? PageChild(page, kTopTextId) : 0;
    if (top && R32(top + kInstanceWidget)) {
        ctx.ecx = R32(top + kInstanceWidget) + kTextString;
        CallGuest(ctx, kSetText, {Wide16(g_top_text)});
    }
    g_top_saved = false;
    uint32_t validate = CallGuest(ctx, kFind, {page, kValidateName});
    if (validate && g_validate_saved) {
        G<uint8_t>(validate + kVisible) = g_validate[0];
        G<uint8_t>(validate + kFlags) = g_validate[1];
    }
    g_validate_saved = false;
}

void Enter(X86Context& ctx, uint32_t menu, int mode, int section = -1) {
    uint32_t owner = R32(menu + kOwner);
    uint32_t page = R32(menu + kVideo);
    uint32_t list = R32(menu + kVideoList);
    if (!owner || !page || !list || section < 0) return;
    if (!g_shown) g_shown = std::max<uint8_t>(G<uint8_t>(list + kRowsShown), 4);
    g_take = {mode, menu, owner, page, list, false, false, {}, section};
    W16(list + kListY, uint16_t(ListTop()));
    Fill(ctx);
    size_t count = 0;
    const app::rows::Row* rows = app::rows::Page(mode, &count);
    SetTitle(ctx, page, Text(section < 0 ? kCaptions[mode] : rows[section].label));
    uint32_t top = section < 0 ? 0 : PageChild(page, kTopTextId);
    uint32_t top_text = top ? R32(top + kInstanceWidget) : 0;
    if (top_text) {
        if (!g_top_saved) g_top_text = ReadWide(top_text);
        g_top_saved = true;
        ctx.ecx = top_text + kTextString;
        CallGuest(ctx, kSetText, {Wide16(Text(kCaptions[mode]))});
    }
    if (uint32_t validate = CallGuest(ctx, kFind, {page, kValidateName})) {
        g_validate[0] = G<uint8_t>(validate + kVisible);
        g_validate[1] = G<uint8_t>(validate + kFlags);
        g_validate_saved = true;
        ctx.ecx = validate;
        CallGuest(ctx, kSetEnabled, {0});
        G<uint8_t>(validate + kVisible) &= ~1;
    }
    CallGuest(ctx, kResetFocus, {page});
    ctx.ecx = owner;
    CallGuest(ctx, kShow, {page, 1, 1, 0});
    Sound(ctx, owner);
    LOGI("options menu: %s, %zu rows, %u shown", kCaptions[mode], g_take.rows.size(), g_shown);
}

void StopCapture() {
    if (g_capture < 0) return;
    g_capture = -1;
    Unblock();
}

void Leave(X86Context& ctx) {
    StopCapture();
    W16(g_take.list + kListY, uint16_t(kStockListY));
    ctx.ecx = g_take.menu;
    CallGuest(ctx, kFillVideo, {});
    ctx.ecx = g_take.list;
    CallGuest(ctx, kLayout, {});
    RestoreStock(ctx, g_take.menu);
    g_take = {};
}

// step 0 is A
void Act(X86Context& ctx, int step) {
    int row = int16_t(R16(g_take.list + kSelection));
    LOGI("options menu: row %d step %d", row, step);
    if (row < 0 || row >= int(g_take.rows.size())) return;
    const app::rows::Row* r = g_take.rows[row];
    if (r == &kGap) return;
    if (!r) {
        if (step) return;
        app::rows::Defaults(g_take.mode);
    } else if (r->kind == app::rows::Kind::Value) {
        if (!app::rows::Enabled(*r)) return;
        int current = 0;
        std::vector<std::string> choices = app::rows::Choices(*r, &current);
        // a row with a list changes only through the popup, an on / off one flips on A
        bool flip = choices.size() == 2 && choices[0] == "OFF" && choices[1] == "ON";
        if (choices.size() > 1 && (!flip || step)) {
            if (!step) Popup(ctx, row, choices, current);
            return;
        }
        app::rows::Step(*r, step ? step : 1);
    } else {
        if (step) return;
        g_capture = row;
        g_armed = false;
        input::SetBlocked(true);
    }
    Refresh(ctx);
    Sound(ctx, g_take.owner);
}

bool OurPage(X86Context& ctx, uint32_t menu) {
    // the popup over it keeps its own keys
    return g_take.mode >= 0 && !g_take.leaving && menu == g_take.menu &&
           IsPage(ctx, g_take.owner, kVideoName) && !IsPage(ctx, g_take.owner, kPopupName);
}


// RECOMP SETTINGS lists its sections in P_Options' own list (the gray buttons), under the two-bar title of the stock
// pages like Solo > Load Game (system.mgm area 768229913, the one-bar 1515491561 with a second bar 30 lower): the
// bar and the title text draw a bar higher as well, the lower one with our caption, so the list stays put under them
// (a list's place comes from its instance, not the widget rect).
constexpr uint32_t kOptionsPage = 0x28;
constexpr uint32_t kBarId = 0x32542A69;
constexpr uint32_t kTitleId = 0xCFCADA23;
// widget: +0x10 left, +0x12 right, +0x14 top, +0x16 bottom
constexpr uint32_t kRect = 0x10;
constexpr uint32_t kChildren = 0x1C;
constexpr uint32_t kRectTop = kRect + 4;
constexpr uint32_t kRectBottom = kRect + 6;
constexpr int16_t kBarStep = 30;
constexpr uint32_t kElementDraw = 0x2C64E0;  // thiscall instance
struct Sections {
    bool on = false;
    int mode = 0;  // the caption they belong to
    int pending = 0;  // 1 shows them, -1 brings the options back, once the list's key handling is over
    uint32_t menu = 0;
    uint32_t page = 0;
    uint32_t list = 0;
    uint32_t bar = 0;
    uint32_t title = 0;
};
Sections g_sections;  // game thread
bool g_drawing = false;

// the caption's sections that have rows to list, as their headings' rows (versus hides the gyro)
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

bool OnSections(X86Context& ctx, uint32_t menu) {
    return g_sections.on && g_take.mode < 0 && menu == g_sections.menu &&
           IsPage(ctx, R32(menu + kOwner), kOptionsName) && !IsPage(ctx, R32(menu + kOwner), kPopupName);
}

void EndSections() { g_sections.on = false; }

// shown again, so its opening animation plays as on a stock page change
void Replay(X86Context& ctx, int row) {
    Sections& s = g_sections;
    uint32_t owner = R32(s.menu + kOwner);
    ctx.ecx = owner;
    CallGuest(ctx, kShow, {s.page, 0, 1, 0});
    CallGuest(ctx, kResetFocus, {s.page});
    ctx.ecx = owner;
    CallGuest(ctx, kShow, {s.page, 1, 1, 0});
    ctx.ecx = s.list;
    CallGuest(ctx, kSelect, {uint32_t(row), 0, 1});
}

void SectionsTick(X86Context& ctx) {
    Sections& s = g_sections;
    int pending = s.pending;
    s.pending = 0;
    if (pending > 0 && s.list) {
        s.on = true;
        uint32_t list = s.list;
        ctx.ecx = list;
        CallGuest(ctx, kClear, {});
        size_t count = 0;
        const app::rows::Row* rows = app::rows::Page(s.mode, &count);
        std::vector<int> sections = ListedSections(s.mode);
        for (int heading : sections) {
            ctx.ecx = list;
            CallGuest(ctx, kAddItem, {Wide16(Text(rows[heading].label)), 0});
        }
        G<uint8_t>(list + kRowsShown) = uint8_t(sections.size());
        ctx.ecx = list;
        CallGuest(ctx, kLayout, {});
        Replay(ctx, 0);
        LOGI("options menu: sections, bar %08X title %08X", s.bar, s.title);
    } else if (pending < 0) {
        EndSections();
        ctx.ecx = s.menu;
        CallGuest(ctx, kFillOptions, {});
        Replay(ctx, g_first);
    }
}

// an area's images carry their own page place, so the bar moves by its children (area instance +0x1C, a box of them)
void Raise(uint32_t widget, bool area, int dy) {
    auto move = [dy](uint32_t w) {
        W16(w + kRectTop, uint16_t(int16_t(R16(w + kRectTop)) + dy));
        W16(w + kRectBottom, uint16_t(int16_t(R16(w + kRectBottom)) + dy));
    };
    if (!area) return move(widget);
    uint32_t box = R32(widget + kChildren);
    uint32_t begin = box ? R32(box + kBoxBegin) : 0;
    uint32_t end = box ? R32(box + kBoxEnd) : 0;
    if (!begin || end < begin || end - begin > 4 * 64) return;
    for (uint32_t it = begin; it < end; it += 4)
        if (uint32_t child = R32(it); child && R32(child + kInstanceWidget)) move(R32(child + kInstanceWidget));
}

}  // namespace

X86_FUNC(sub_002C64E0);

namespace game {

// hud.cpp's element draw: true when it drew the instance
bool OptionsElement(X86Context& ctx, uint8_t* base, uint32_t instance) {
    Sections& s = g_sections;
    if (!s.on || g_drawing) return false;
    uint32_t widget = R32(instance + kInstanceWidget);
    if (!widget || (instance != s.bar && instance != s.title)) return false;
    g_drawing = true;
    Raise(widget, instance == s.bar, -kBarStep);
    // through CallGuest, which pushes its own return: a hook called directly returns past its caller's
    ctx.ecx = instance;
    CallGuest(ctx, kElementDraw, {});
    Raise(widget, instance == s.bar, kBarStep);
    std::u16string text;
    if (instance == s.title) {
        text = ReadWide(widget);
        ctx.ecx = widget + kTextString;
        CallGuest(ctx, kSetText, {Wide16(Text(kCaptions[s.mode]))});
    }
    ctx.ecx = instance;
    sub_002C64E0(ctx, base);
    if (instance == s.title) {
        ctx.ecx = widget + kTextString;
        CallGuest(ctx, kSetText, {Wide16(text)});
    }
    g_drawing = false;
    ctx.ecx = instance;
    return true;
}

// the id of the context whose draw or input stack holds the page (a player's menus: its player number), 0 for none
uint32_t StackOwner(uint32_t page) {
    // the Magma manager: +0x3C contexts, +0x40 count; context +0x14 draw stack, +0x18 input stack, +0x1C id
    constexpr uint32_t kMagma = 0x6A6638;
    constexpr uint32_t kContexts = 0x3C;
    constexpr uint32_t kContextCount = 0x40;
    constexpr uint32_t kDrawStack = 0x14;
    constexpr uint32_t kInputStack = 0x18;
    constexpr uint32_t kContextId = 0x1C;
    // stack: +4 begin, +8 end, 8 bytes an entry with the page first
    constexpr uint32_t kStackBegin = 4;
    constexpr uint32_t kStackEnd = 8;
    uint32_t m = R32(kMagma);
    uint32_t list = m ? R32(m + kContexts) : 0;
    uint32_t count = m ? R32(m + kContextCount) : 0;
    if (!page || !list || count > 16) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t context = R32(list + i * 4);
        for (uint32_t offset : {kDrawStack, kInputStack}) {
            uint32_t stack = context ? R32(context + offset) : 0;
            uint32_t begin = stack ? R32(stack + kStackBegin) : 0;
            uint32_t end = stack ? R32(stack + kStackEnd) : 0;
            if (!begin || end < begin || end - begin > 8 * 64) continue;
            for (uint32_t it = begin; it < end; it += 8)
                if (R32(it) == page) return R32(context + kContextId);
        }
    }
    return 0;
}

bool OnStack(uint32_t page) { return StackOwner(page) != 0; }

// mouse.cpp: the cursor leaves the selection alone over our blank row, else the pass over it flickers
bool BlankRow(uint32_t list, int row) {
    return g_take.mode >= 0 && list == g_take.list && row >= 0 && row < int(g_take.rows.size()) &&
           g_take.rows[size_t(row)] == &kGap;
}

// the page's keyframes put the list back every tick: mouse.cpp calls this before its hit test (a list's rect comes
// from its place), MenuTick again
void MenuPlace(X86Context& ctx) {
    if (g_take.mode < 0 || g_take.leaving || int16_t(R16(g_take.list + kListY)) == ListTop()) return;
    W16(g_take.list + kListY, uint16_t(ListTop()));
    ctx.ecx = g_take.list;
    CallGuest(ctx, kLayout, {});
}

static void PassGap(X86Context& ctx) {
    // the gap between headings is passed over, on in the direction the selection came from
    int row = int16_t(R16(g_take.list + kSelection));
    int rows = int(g_take.rows.size());
    if (row >= 0 && row < rows && g_take.rows[size_t(row)] == &kGap) {
        row = std::clamp(row + (g_take.last_row < row ? 1 : -1), 0, rows - 1);
        ctx.ecx = g_take.list;
        CallGuest(ctx, kSelect, {uint32_t(row), 0, 1});
    }
    g_take.last_row = row;
}

// Delete on a binding row
static void Clear(X86Context& ctx) {
    int row = int16_t(R16(g_take.list + kSelection));
    if (row < 0 || row >= int(g_take.rows.size()) || !OurPage(ctx, g_take.menu)) return;
    const app::rows::Row* r = g_take.rows[size_t(row)];
    if (!r || (r->kind != app::rows::Kind::Bind && r->kind != app::rows::Kind::PadBind)) return;
    std::string was = app::rows::Value(*r);
    if (r->kind == app::rows::Kind::Bind) input::Unbind(size_t(r->id));
    else input::BindPad(size_t(r->id), -1);
    if (app::rows::Value(*r) == was) return;
    Refresh(ctx);
    Sound(ctx, g_take.owner);
}

// a rebind waiting for its key or button
static void CaptureTick(X86Context& ctx) {
    // a controller row takes any controller button, Escape cancels either
    const app::rows::Row* r = g_take.rows[size_t(g_capture)];
    bool pad = r && r->kind == app::rows::Kind::PadBind;
    int vk = input::PressedKey();
    int button = pad ? input::PressedPadButton() : -1;
    if (!g_armed) {
        g_armed = !vk && button < 0;
        return;
    }
    if (button >= 0) input::BindPad(size_t(r->id), button);
    else if (vk && !pad && vk != kEscape && r) input::Bind(size_t(r->id), vk);
    else if (vk != kEscape) return;
    StopCapture();
    Refresh(ctx);
    Sound(ctx, g_take.owner);
}

// game thread, every input update
void MenuTick(X86Context& ctx) {
    SectionsTick(ctx);
    if (g_take.mode < 0) return;
    // the stock rows go back only once the list's key handling is over, a refill under it leaves the page stale
    if (g_take.leaving) {
        Leave(ctx);
        LOGI("options menu: stock rows back");
        return;
    }
    // a change from F10 shows at once
    static uint32_t revision = 0;
    if (uint32_t r = app::SettingsRevision(); r != revision && g_capture < 0 && g_popup_row < 0) {
        revision = r;
        g_take.refill = true;
    }
    if (g_take.refill) {
        g_take.refill = false;
        Fill(ctx, std::max(0, int(int16_t(R16(g_take.list + kSelection)))));
    }
    if (OnStack(g_take.page)) {
        g_take.seen = true;
    } else if (g_take.seen) {
        // closed under us (the pause menu went): the stock rows come back with the next options page
        StopCapture();
        W16(g_take.list + kListY, uint16_t(kStockListY));
        g_take = {};
        g_restore = true;
        return;
    }
    MenuPlace(ctx);
    PassGap(ctx);
    // read every tick, so a Delete that answers a capture is not a fresh press once it ends
    bool clear = input::TakeDeletePress();
    if (g_capture >= 0) return CaptureTick(ctx);
    if (clear) Clear(ctx);
}

}  // namespace game

// options list fill (thiscall menu): Controls, Display, Sound, Shadow Calibration, then ours
X86_EXTERN_FUNC(__imp__sub_001A53B5);
X86_FUNC(sub_001A53B5) {
    uint32_t menu = ctx.ecx;
    EndSections();
    __imp__sub_001A53B5(ctx, base);
    uint32_t list = R32(menu + kOptionsList);
    int count = list ? Count(list) : 0;
    if (count < 2) return;  // split screen shows Controls only
    g_first = count;
    ctx.ecx = list;
    for (int mode : kListedModes) CallGuest(ctx, kAddItem, {Wide16(Text(kCaptions[mode])), 0});
    G<uint8_t>(list + kRowsShown) = uint8_t(count + kCaptionCount);
    CallGuest(ctx, kLayout, {});
    if (g_restore) RestoreStock(ctx, menu);
    g_restore = false;
}

// a caption pressed: its sections show once the list's key handling is over
static void OpenSections(X86Context& ctx, uint32_t menu, uint32_t list, uint32_t owner, int mode) {
    uint32_t page = R32(menu + kOptionsPage);
    g_sections.pending = 1;
    g_sections.mode = mode;
    g_sections.menu = menu;
    g_sections.page = page;
    g_sections.list = list;
    g_sections.bar = PageChild(page, kBarId);
    g_sections.title = PageChild(page, kTitleId);
    Sound(ctx, owner);
}

// OnPress (thiscall listener, instance, ret 4)
X86_EXTERN_FUNC(__imp__sub_001A4161);
X86_FUNC(sub_001A4161) {
    uint32_t menu = ctx.ecx - kListener;
    uint32_t instance = R32(ctx.esp + 4);
    uint32_t owner = R32(menu + kOwner);
    uint32_t list = R32(menu + kOptionsList);
    bool ours_list = instance && (CallGuest(ctx, kIsElement, {instance, kListName}) & 0xFF);
    int row = ours_list ? int16_t(R16(R32(instance + kInstanceWidget) + kSelection)) : -1;
    if (ours_list && R32(instance + kInstanceWidget) == list && OnSections(ctx, menu)) {
        std::vector<int> sections = ListedSections(g_sections.mode);
        if (row >= 0 && row < int(sections.size())) Enter(ctx, menu, g_sections.mode, sections[size_t(row)]);
        ctx.ecx = menu + kListener;
        G<uint8_t>(owner + kTaken) = 1;
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    if (ours_list && g_take.mode < 0 && g_first >= 0 && R32(instance + kInstanceWidget) == list && row >= g_first &&
        row < g_first + kCaptionCount && IsPage(ctx, owner, kOptionsName)) {
        OpenSections(ctx, menu, list, owner, kListedModes[row - g_first]);
        ctx.ecx = menu + kListener;
        G<uint8_t>(owner + kTaken) = 1;
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    if (ours_list && OurPage(ctx, menu)) {
        if (g_capture < 0) Act(ctx, 0);
        G<uint8_t>(owner + kTaken) = 1;
        ctx.eax = 1;
        ctx.esp += 8;
        return;
    }
    ctx.ecx = menu + kListener;
    __imp__sub_001A4161(ctx, base);
}

// OnBack (thiscall listener, one argument, ret 4)
X86_EXTERN_FUNC(__imp__sub_001A5C41);
X86_FUNC(sub_001A5C41) {
    uint32_t menu = ctx.ecx - kListener;
    if (OurPage(ctx, menu)) {
        LOGI("options menu: back");
        uint32_t owner = g_take.owner;
        ctx.ecx = owner;
        CallGuest(ctx, kShow, {g_take.page, 0, 1, 0});
        StopCapture();
        g_take.leaving = true;
        G<uint8_t>(owner + kTaken) = 0;
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    if (OnSections(ctx, menu)) {
        g_sections.pending = -1;
        Sound(ctx, R32(menu + kOwner));
        G<uint8_t>(R32(menu + kOwner) + kTaken) = 0;
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    ctx.ecx = menu + kListener;
    __imp__sub_001A5C41(ctx, base);
}

// OnKey (thiscall listener, instance, int* key, ret 8): arrow key codes
X86_EXTERN_FUNC(__imp__sub_001A5786);
X86_FUNC(sub_001A5786) {
    uint32_t menu = ctx.ecx - kListener;
    uint32_t key_at = R32(ctx.esp + 8);
    uint32_t key = key_at ? R32(key_at) : 0;
    if ((key == kLeft || key == kRight) && OurPage(ctx, menu)) {
        if (g_capture < 0) Act(ctx, key == kLeft ? -1 : 1);
        G<uint8_t>(g_take.owner + kTaken) = 1;
        ctx.eax = 1;
        ctx.esp += 12;
        return;
    }
    ctx.ecx = menu + kListener;
    __imp__sub_001A5786(ctx, base);
}
