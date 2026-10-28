#include "State.h"
#include <array>
#include <cmath>
#include <cstring>

namespace beatweaver {
namespace {
uint32_t checksum(const uint8_t* data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 16777619u;
    return hash;
}
void integer(std::vector<uint8_t>& bytes, uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
void number(std::vector<uint8_t>& bytes, float value) {
    uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); integer(bytes, bits);
}
struct Reader {
    const uint8_t* data; size_t size; size_t offset = 0; bool valid = true;
    uint32_t integer() {
        if (offset + 4 > size) { valid = false; return 0; }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value |= uint32_t(data[offset++]) << (i * 8);
        return value;
    }
    float number() { uint32_t bits = integer(); float value; std::memcpy(&value, &bits, 4); return value; }
};
bool inRange(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }
}

std::vector<uint8_t> encodeState(const Model& model) {
    std::vector<uint8_t> bytes;
    bytes.reserve(80000);
    integer(bytes, 0x31565742u); // BWV1, little endian.
    integer(bytes, 1);
    integer(bytes, kParameterCount);
    integer(bytes, kSlotCount * 2);
    for (int p = 0; p < kParameterCount; ++p) number(bytes, model.getParam(static_cast<Param>(p)));
    for (int bank = 0; bank < 2; ++bank) {
        for (int slot = 0; slot < kSlotCount; ++slot) {
            const auto env = model.getEnvelope(bank != 0, slot);
            integer(bytes, env.count);
            bytes.insert(bytes.end(), env.name, env.name + kNameLength);
            for (int point = 0; point < env.count; ++point) {
                const auto& node = env.points[point];
                number(bytes, node.x); number(bytes, node.y); number(bytes, node.tension);
                integer(bytes, static_cast<uint32_t>(node.curve));
            }
        }
    }
    integer(bytes, checksum(bytes.data(), bytes.size()));
    return bytes;
}

bool decodeState(Model& model, const void* data, size_t size) {
    if (!data || size < 20 || size > 100000) return false;
    const auto* raw = static_cast<const uint8_t*>(data);
    Reader footer{raw + size - 4, 4};
    if (footer.integer() != checksum(raw, size - 4)) return false;
    Reader reader{raw, size - 4};
    if (reader.integer() != 0x31565742u || reader.integer() != 1 ||
        reader.integer() != kParameterCount || reader.integer() != kSlotCount * 2) return false;
    std::array<float, kParameterCount> params{};
    std::array<Envelope, kSlotCount * 2> envelopes{};
    for (auto& param : params) { param = reader.number(); if (!inRange(param, 0, 1)) return false; }
    for (auto& env : envelopes) {
        env.count = static_cast<int>(reader.integer());
        if (!reader.valid || env.count < 2 || env.count > kMaxPoints || reader.offset + kNameLength > reader.size) return false;
        std::memcpy(env.name, raw + reader.offset, kNameLength);
        env.name[kNameLength - 1] = '\0'; reader.offset += kNameLength;
        float previous = -1;
        for (int i = 0; i < env.count; ++i) {
            auto& point = env.points[i];
            point.x = reader.number(); point.y = reader.number(); point.tension = reader.number();
            point.curve = static_cast<int>(reader.integer());
            if (!reader.valid || !inRange(point.x, 0, 1) || !inRange(point.y, 0, 1) ||
                !inRange(point.tension, -1, 1) || point.curve < 0 || point.curve > static_cast<int>(Curve::HalfSine) || point.x < previous) return false;
            previous = point.x;
        }
        if (env.points[0].x != 0 || env.points[env.count - 1].x != 1) return false;
    }
    if (!reader.valid || reader.offset != reader.size) return false;
    // Slot publication is lock-free for readers; no partially parsed input is published.
    for (int i = 0; i < kSlotCount * 2; ++i) model.setEnvelope(i >= kSlotCount, i % kSlotCount, envelopes[i]);
    for (int p = 0; p < kParameterCount; ++p) model.setParam(static_cast<Param>(p), params[p]);
    return true;
}
}
