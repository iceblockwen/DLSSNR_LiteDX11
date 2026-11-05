// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "white_point.hpp"
#include "ngx_parameters.hpp"
#include <string>
namespace lite::nr
{
// Runtime backend owns NGX capabilities and device validation.
class FeatureBackend
{
  public:
    virtual ~FeatureBackend() = default;
    virtual HRESULT Initialize(ID3D12Device*) = 0;
    virtual Parameters* Capability() const = 0;
    virtual unsigned Create(ID3D12GraphicsCommandList*, void**) = 0;
    virtual unsigned Evaluate(ID3D12GraphicsCommandList*, void*) = 0;
    virtual unsigned Release(void*) = 0;
    virtual void SetFloat(const char* key, float v)
    {
        Capability()->Set(key, v);
    }
    virtual void SetUnsigned(const char* key, unsigned v)
    {
        Capability()->Set(key, v);
    }
    virtual void SetResource(const char* key, ID3D12Resource* v)
    {
        Capability()->Set(key, reinterpret_cast<unsigned long long>(v));
    }
};
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
    unsigned modelResult{};
    HRESULT modelError{};
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
    PassExecutor12(ID3D12Device*, FeatureBackend&);
    ~PassExecutor12();
    HRESULT Record(ID3D12GraphicsCommandList*, const Frame12&, const Exposure&, const PassSettings&,
                   bool reset, std::shared_ptr<void>& keepAlive, ID3D12Resource*& result);
    PassStatus Snapshot() const;
    void Reset();
    void InvalidateHistory();
    void RetireCompletedInputs(); // drop game texture references after the real fence
  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
bool LinearColorFormat(DXGI_FORMAT);
// Nonblocking retirement for features whose last recording outlived the pass.
void RetireModelFeatures();
}
