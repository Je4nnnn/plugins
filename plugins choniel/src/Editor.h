#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <memory>
#include "Core.h"

namespace beatweaver {

using AutomationCallback = void (*)(void* context, int parameter, float value);

class Editor {
public:
    explicit Editor(Model& model, AutomationCallback callback = nullptr, void* context = nullptr);
    ~Editor();
    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;
    bool open(HWND parent, HINSTANCE instance);
    void close();
    HWND window() const noexcept;
    static constexpr int width() noexcept { return 1120; }
    static constexpr int height() noexcept { return 740; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace beatweaver
