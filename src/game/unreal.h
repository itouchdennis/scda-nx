#pragma once
// Script objects by name. This build keeps no name strings: an FName is appStrihash of the text (UTF-16 upper
// case through the engine's CRC table, from 0), and each class links only its own properties, compacted.
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace game::unreal {

constexpr std::array<uint32_t, 256> kCrcTable = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int j = 0; j < 8; j++) c = c & 0x80000000u ? c << 1 ^ 0x04C11DB7u : c << 1;
        t[i] = c;
    }
    return t;
}();

constexpr uint32_t Name(std::string_view text) {
    uint32_t hash = 0;
    for (char ch : text) {
        uint32_t c = ch >= 'a' && ch <= 'z' ? uint32_t(ch - 32) : uint8_t(ch);
        for (uint32_t b : {c & 0xFF, c >> 8}) hash = (hash >> 8 & 0x00FFFFFF) ^ kCrcTable[(hash ^ b) & 0xFF];
    }
    return hash;
}
static_assert(Name("XboxViewport") == 0xA812A2F5);

uint32_t NameOf(uint32_t object);
uint32_t StateOf(uint32_t object);  // the running state's name, 0 without one
bool IsA(uint32_t object, uint32_t class_name);
// byte offset of a property in the object's class or its supers, 0 when there is none
uint32_t Offset(uint32_t object, uint32_t property);

}  // namespace game::unreal
