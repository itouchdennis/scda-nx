#pragma once
#include <string>
#include <string_view>
// Text the recomp adds to the game's menus, in the game's other languages (translations.cpp)

namespace app {

// the game's language is English, or the phrase has no entry: nullptr. Phrases are the English text in any case.
const char16_t* Translation(std::string_view english);
// the text with each phrase that has a translation (the longest from each word start) in the game's language, the
// rest (all of it in English) through rest
std::u16string Translate(std::string_view text, std::string (*rest)(std::string_view));
// the F10 panel's capitals in the game's title case: "DISPLAY MODE" as "Display Mode", "SPACE/ENTER" as "Space/Enter"
std::string TitleCase(std::string_view text);

}  // namespace app
