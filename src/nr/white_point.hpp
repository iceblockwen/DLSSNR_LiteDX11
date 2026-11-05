// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "frame.hpp"
#include <span>
#include <vector>
namespace lite::nr
{
enum class WhiteMode : unsigned
{
    Game,
    Measured,
    Manual
};
enum class WhiteSource : unsigned
{
    Manual,
    Display,
    Game,
    Measured,
    Waiting,
    Held
};
struct WhiteSettings
{
    WhiteMode mode = WhiteMode::Game;
    bool reverseRatio{}, useTexture = true;
    bool usePreExposure = true, useExposureScale = true;
    float manual = 1, trim = 1, anchor = .18f, tau = .3f, deadband = .08f, attack = 1, release = .5f;
    // Exponent applied to the live game exposure ratio: white = (pre/exposure)^exponent * trim,
    // or (exposure/pre)^exponent * trim when reverseRatio is set. Only the fraction swaps.
    // 1 keeps the plain linear mapping; below 1 compresses the exposure range so one trim
    // covers both day and night, above 1 expands it.
    float exponent = 1;
    unsigned every = 4;
};
struct WhiteStatus
{
    WhiteSource source = WhiteSource::Waiting;
    bool valid{};
    bool statisticsValid{};
    float value = 1, target = 1, logAverage{}, mean{}, p50{}, p90{}, p99{}, peak{};
};
// Extracted Lite ConsumeMeter / ResolveWhitePoint / FilterWhite mechanisms.
// Per-stream state; never meter the model's edited output.
class WhitePoint
{
  public:
    void Consume(std::span<const float> grid);
    WhiteStatus Resolve(const WhiteSettings&, const Exposure&, float dt, bool reset = false);
    void Reset();

  private:
    WhiteStatus status;
    bool started{}, measured{};
    WhiteMode mode = WhiteMode::Game;
    WhiteSource previous = WhiteSource::Waiting;
};
struct WhiteReadback
{
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12Resource> source, grid, readback;
    ComPtr<ID3D12Resource> exposure;
    bool hasExposure{};
    ComPtr<ID3D12DescriptorHeap> heap;
    // Caller may consume only after the fence covering this recording is complete.
    HRESULT Consume(WhitePoint&, float* gameExposure = nullptr) const;
    void RetireInputs()
    {
        source.Reset();
        exposure.Reset();
    } // same completion contract as Consume
};
class WhiteMeter12
{
  public:
    explicit WhiteMeter12(ID3D12Device*);
    HRESULT Record(ID3D12GraphicsCommandList*, ID3D12Resource* uneditedSource, std::shared_ptr<WhiteReadback>&, ID3D12Resource* exposure = nullptr);

  private:
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    std::vector<std::shared_ptr<WhiteReadback>> pool;
};
}
