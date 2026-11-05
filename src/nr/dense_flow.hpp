#pragma once
#include "frame.hpp"
#include <vector>
namespace lite::nr
{
struct DenseFlowSettings
{
    float fov = 60, farPlane = 1000, depthMultiplier = 1;
    float depthScaleX = 1, depthScaleY = 1, depthOffsetX = 0, depthOffsetY = 0;
    float depthPixelOffsetX = 0, depthPixelOffsetY = 0;
    bool logarithmic = false;
    bool operator==(const DenseFlowSettings&) const = default;
};
class DenseFlow12
{
  public:
    explicit DenseFlow12(ID3D12Device*);
    ~DenseFlow12();
    HRESULT Record(ID3D12GraphicsCommandList*, const Frame12&, std::shared_ptr<void>&, const DenseFlowSettings&);
    // Consumer adapter: pixel units and NR loss mask never feed the LAUNCHPAD core.
    HRESULT RecordForNR(ID3D12GraphicsCommandList*, const Frame12&, std::shared_ptr<void>&, const DenseFlowSettings&);
    void Reset();
    void InvalidateHistory(); // busy before recording: keep resources and their tracked states
    ComPtr<ID3D12Resource> MotionUV() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
