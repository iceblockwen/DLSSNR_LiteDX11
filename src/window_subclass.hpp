// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <algorithm>
#include <map>
#include <vector>

namespace lite {
// Caller serializes all operations. A foreign subclass may retain our callback
// after the last panel is gone, so keep its original forwarding target alive.
template<class Owner> class WindowSubclassRegistry {
public:
    struct Route {
        WNDPROC original{};
        std::vector<Owner*> owners;
        Owner* Active() const { return owners.empty() ? nullptr : owners.back(); }
    };
    HRESULT Attach(HWND window, Owner* owner, WNDPROC callback) {
        auto [it, inserted] = routes.try_emplace(window);
        auto& route = it->second;
        if (std::find(route.owners.begin(), route.owners.end(), owner) != route.owners.end()) return S_OK;
        try { route.owners.push_back(owner); }
        catch (...) { if (inserted) routes.erase(it); throw; }
        const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
        if (inserted || current == route.original) {
            if (current == callback) {
                route.owners.pop_back();
                if (inserted) routes.erase(it);
                return E_UNEXPECTED; // Never save our own callback as the native target.
            }
            SetLastError(0);
            auto previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(callback)));
            if (!previous) {
                const auto error = GetLastError();
                route.owners.pop_back();
                if (inserted) routes.erase(it);
                return error ? HRESULT_FROM_WIN32(error) : E_UNEXPECTED;
            }
            route.original = previous;
        }
        return S_OK;
    }
    void Detach(HWND window, Owner* owner, WNDPROC callback) {
        const auto it = routes.find(window);
        if (it == routes.end()) return;
        auto& route = it->second;
        const auto found = std::find(route.owners.begin(), route.owners.end(), owner);
        if (found == route.owners.end()) return;
        route.owners.erase(found);
        if (!route.owners.empty()) return;
        if (!IsWindow(window)) { routes.erase(it); return; }
        if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == callback) {
            SetLastError(0);
            if (SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(route.original))) routes.erase(it);
        }
    }
    Route* Find(HWND window) {
        const auto it = routes.find(window);
        return it == routes.end() ? nullptr : &it->second;
    }
    void Erase(HWND window) { routes.erase(window); }
private:
    std::map<HWND, Route> routes;
};
}
