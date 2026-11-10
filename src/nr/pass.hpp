// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "white_point.hpp"
#include "ngx_parameters.hpp"
namespace lite::nr
{
// The pass records into the model runtime; there is exactly one implementation,
// so the interface is a plain reference instead of a virtual base.
class ModelRuntime;
struct PassSettings
{
    WhiteSettings white;
    bool gpuTiming{};       // runtime panel demand only; never persisted
    bool panelStatistics{}; // runtime demand only; never persisted
    float differenceGain = 1;
    bool composed = true;

    unsigned preset{}, style{}, autoMask = 1, uiCorrection{};
    float intensity = 1, structure = 1, tone = 1, skin = -1;
};
struct PassStatus
{
    HRESULT error = S_OK;
    bool frameSkipped{}; // input/recording rejection; does not latch a model failure
    unsigned ngxResult{};
    bool ready{}, edited{};
    unsigned readyPasses{}, actualPasses{}, failedPass{};
    bool timingValid{};
    float gpuMilliseconds{}, modelMilliseconds{};
    WhiteStatus white;
    bool hasInput{}, sceneLinear{}, gameExposureTexture{};
    bool preExposurePresent{}, exposureScalePresent{};
    float preExposure = 1, exposureScale = 1;
    HRESULT whiteMeasurementError = S_OK;
    bool gpuExposureValid{}, gpuDivisorValid{};
    float gpuExposure{}, gpuDivisor = 1;
    InputOrigin inputOrigin = InputOrigin::Unknown;
    UINT64 inputTime{};
    bool generatedMotion{}, depthFlipped{}, inputConverted[3]{};
    float motionScaleX = 1, motionScaleY = 1;
    const char* reason = "Waiting for NR input";
};
// Input and result rest in NON_PIXEL_SHADER_RESOURCE. The recording owner must
// submit/discard frame.ticket and retain keepAlive until its fence completes.
// Feature initialization uses the caller's list, as in Lite's default route.
// A trusted Frame12::orderedQueue permits steady-state evaluations to follow
// already submitted work; descriptor reuse and readback still require completion.
class PassExecutor12
{
  public:
    PassExecutor12(ID3D12Device*, ModelRuntime&);
    ~PassExecutor12();
    HRESULT Record(ID3D12GraphicsCommandList*, const Frame12&, const Exposure&, const PassSettings&,
                   bool reset, std::shared_ptr<void>& keepAlive, ID3D12Resource*& result);
    PassStatus Snapshot() const;
    void InvalidateHistory();
  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
bool LinearColorFormat(DXGI_FORMAT);
// Nonblocking retirement for features whose last recording outlived the pass.
void RetireModelFeatures();
}
