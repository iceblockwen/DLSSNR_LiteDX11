#pragma once
#include "runtime.hpp"
#include <dxgi1_6.h>

namespace lite
{
struct GraphicsStatus
{
    State panel = State::Waiting;
    bool visible = false;
    unsigned width = 0, height = 0;
    HRESULT error = S_OK;
    wchar_t reason[256]{};
};
void AttachFactory(REFIID iid, void** output) noexcept;
}
