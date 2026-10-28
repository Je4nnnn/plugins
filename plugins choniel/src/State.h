#pragma once
#include "Core.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace beatweaver {
std::vector<uint8_t> encodeState(const Model& model);
// Fully validates before modifying model; called only on the control thread.
bool decodeState(Model& model, const void* data, size_t size);
}
