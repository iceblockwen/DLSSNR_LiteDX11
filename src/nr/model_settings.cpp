// SPDX-License-Identifier: GPL-3.0-or-later
#include "model_settings.hpp"
#include "model_runtime.hpp"
#include "capture11.hpp"
#include "../config.hpp"
#include "../graphics.hpp"
#include <mutex>
#include <atomic>
#include <cmath>
#include <algorithm>
namespace lite::nr
{
namespace
{
std::mutex mutex;
ModelSettings settings;
PassStatus last;
std::atomic<bool> enabled{}, blocked{};
std::atomic<UINT64> revision{1};
float Float(const wchar_t* ini, const wchar_t* key, const wchar_t* fallback)
{
    wchar_t text[48]{};
    GetPrivateProfileStringW(L"NeuralRendering", key, fallback, text, 48, ini);
    return float(_wtof(text));
}
void Save(const wchar_t* key, float value)
{
    wchar_t text[48]{};
    swprintf_s(text, L"%.7g", double(value));
    QueueConfig(L"NeuralRendering", key, text);
}
}
void ConfigureModel(const wchar_t* ini)
{
    ModelSettings s;
    s.enabled = GetPrivateProfileIntW(L"NeuralRendering", L"Enabled", 0, ini) != 0;
    auto& p = s.pass;
    p.white.mode = WhiteMode(GetPrivateProfileIntW(L"NeuralRendering", L"WhiteMode", 0, ini));
    p.white.manual = Float(ini, L"ManualWhite", L"1");
    p.white.trim = Float(ini, L"WhiteTrim", L"1");
    p.white.anchor = Float(ini, L"WhiteAnchor", L"0.18");
    p.white.tau = Float(ini, L"WhiteTau", L"0.3");
    p.white.deadband = Float(ini, L"WhiteDeadband", L"0.08");
    p.white.attack = Float(ini, L"WhiteAttack", L"1");
    p.white.release = Float(ini, L"WhiteRelease", L"0.5");
    p.white.every = GetPrivateProfileIntW(L"NeuralRendering", L"WhiteEvery", 4, ini);
    p.intensity = Float(ini, L"Intensity", L"1");
    p.structure = Float(ini, L"Structure", L"1");
    p.tone = Float(ini, L"Tone", L"1");
    p.skin = Float(ini, L"Skin", L"-1");
    p.preset = GetPrivateProfileIntW(L"NeuralRendering", L"Preset", 0, ini);
    p.style = GetPrivateProfileIntW(L"NeuralRendering", L"Style", 0, ini);
    p.autoMask = GetPrivateProfileIntW(L"NeuralRendering", L"AutoMask", 1, ini);
    p.uiCorrection = GetPrivateProfileIntW(L"NeuralRendering", L"UICorrection", 0, ini);
    p.white.reverseRatio = GetPrivateProfileIntW(L"NeuralRendering", L"WhiteRatio", 0, ini) != 0;
    p.white.useTexture = GetPrivateProfileIntW(L"NeuralRendering", L"WhiteUseTexture", 1, ini) != 0;
    p.white.usePreExposure = GetPrivateProfileIntW(L"NeuralRendering", L"WhiteUsePreExposure", 1, ini) != 0;
    // Legacy texture-off configs used pre-exposure alone.
    p.white.useExposureScale = GetPrivateProfileIntW(L"NeuralRendering", L"WhiteUseExposureScale", p.white.useTexture ? 1 : 0, ini) != 0;
    p.white.exponent = Float(ini, L"WhiteExponent", L"1");
    p.differenceGain = Float(ini, L"DifferenceGain", L"1");
    p.composed = GetPrivateProfileIntW(L"NeuralRendering", L"ComposedRestore", 1, ini) != 0;
    p.sharpness = Float(ini, L"Sharpness", L"0");

    SetModelSettings(s, false);
}
ModelSettings GetModelSettings()
{
    std::lock_guard lock(mutex);
    auto s = settings;
    s.pass.gpuTiming = s.pass.panelStatistics = PanelVisible();
    return s;
}
void SetModelSettings(const ModelSettings& input, bool persist)
{
    auto s = input;
    auto& p = s.pass;
    if (p.preset > 2u)
    {
        p.preset = 0;
    }
    p.style = std::min(p.style, 2u);
    p.autoMask = p.autoMask ? 1u : 0u;
    p.uiCorrection = p.uiCorrection ? 1u : 0u;
    p.differenceGain = Clamped(p.differenceGain, 0, 2, 1);
    p.sharpness = Clamped(p.sharpness, 0, 2, 0);

    p.intensity = Clamped(p.intensity, 0, 2, 1);
    p.structure = Clamped(p.structure, 0, 2, 1);
    p.tone = Clamped(p.tone, 0, 2, 1);
    p.skin = Clamped(p.skin, -1, 2, -1);
    p.gpuTiming = p.panelStatistics = false;
    auto& w = p.white;
    if (unsigned(w.mode) > 2)
    {
        w.mode = WhiteMode::Game;
    }
    w.manual = Clamped(w.manual, 1e-8f, 1024, 1);
    w.trim = Clamped(w.trim, .001f, 1024, 1);
    w.exponent = Clamped(w.exponent, .1f, 4, 1);
    w.anchor = Clamped(w.anchor, .02f, 1, .18f);
    w.tau = Clamped(w.tau, .001f, 60, .3f);
    w.deadband = Clamped(w.deadband, 0, 1, .08f);
    w.attack = Clamped(w.attack, 0, 16, 1);
    w.release = Clamped(w.release, 0, 16, .5f);
    w.every = std::clamp(w.every, 1u, 120u);
    {
        std::lock_guard lock(mutex);
        if (settings.enabled != s.enabled)
        {
            if (s.enabled)
            {
                Runtime().RetryFailedInitialization();
            }
            ++revision;
            blocked = false;
            last = {};
        }
        settings = s;
        enabled = s.enabled;
    }
    if (!persist)
    {
        return;
    }

    Save(L"WhiteRatio", w.reverseRatio ? 1.f : 0.f);
    Save(L"WhiteUseTexture", w.useTexture ? 1.f : 0.f);
    Save(L"WhiteUsePreExposure", w.usePreExposure ? 1.f : 0.f);
    Save(L"WhiteUseExposureScale", w.useExposureScale ? 1.f : 0.f);
    Save(L"DifferenceGain", p.differenceGain);
    Save(L"ComposedRestore", p.composed ? 1.f : 0.f);
    Save(L"Sharpness", p.sharpness);
    Save(L"Enabled", s.enabled ? 1.f : 0.f);
    Save(L"Preset", float(p.preset));
    Save(L"Style", float(p.style));
    Save(L"AutoMask", float(p.autoMask));
    Save(L"UICorrection", float(p.uiCorrection));
    Save(L"Intensity", p.intensity);
    Save(L"Structure", p.structure);
    Save(L"Tone", p.tone);
    Save(L"Skin", p.skin);
    Save(L"WhiteMode", float(unsigned(w.mode)));
    Save(L"ManualWhite", w.manual);
    Save(L"WhiteTrim", w.trim);
    Save(L"WhiteExponent", w.exponent);
    Save(L"WhiteAnchor", w.anchor);
    Save(L"WhiteTau", w.tau);
    Save(L"WhiteDeadband", w.deadband);
    Save(L"WhiteAttack", w.attack);
    Save(L"WhiteRelease", w.release);
    Save(L"WhiteEvery", float(w.every));
}
bool ModelRequested()
{
    return enabled.load();
}
bool ModelActive()
{
    return ModelRequested() && !blocked.load() && Runtime().Available();
}
bool ProcessingActive()
{
    return PreviewActive() || ModelActive();
}
void PublishPass(const PassStatus& s)
{
    {
        std::lock_guard lock(mutex);
        last = s;
        if (FAILED(s.error) && !s.frameSkipped)
        {
            blocked = true;
        }
    }
}
PassStatus LastPass()
{
    std::lock_guard lock(mutex);
    return last;
}
UINT64 ModelRevision()
{
    return revision.load();
}
}
