// The process-lifetime registry the graphics layer owns: the panel bound to each
// swapchain and the lock that guards it. panel.cpp includes this to reach the same
// lock, nothing else does.
#pragma once
#include "graphics.hpp"
#include "ui/panel.hpp"
#include <map>
#include <memory>
#include <mutex>
namespace lite
{
struct GraphicsRegistry
{
    std::recursive_mutex mutex;
    std::map<IDXGISwapChain*, std::shared_ptr<Panel>> panels;
};
GraphicsRegistry& Graphics();
}
