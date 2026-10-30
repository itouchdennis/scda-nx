#pragma once
// Magma names pages and elements by crc32 of the name in the .mgm
#include <cstdint>
#include <string_view>

namespace game::magma {

constexpr uint32_t Id(std::string_view name) {
    uint32_t crc = ~0u;
    for (char c : name) {
        crc ^= uint8_t(c);
        for (int i = 0; i < 8; i++) crc = crc >> 1 ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
static_assert(Id("P_Start") == 0xDBFB017D);

}  // namespace game::magma
