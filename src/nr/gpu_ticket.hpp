// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <mutex>
namespace lite::nr
{
// A recording is not a submission. A failed signal is never treated as completion.
class GpuTicket
{
  public:
    enum class State
    {
        Pending,
        Submitted,
        Discarded
    };
    void Submit(ID3D12Fence* f, UINT64 v, ID3D12CommandQueue* queue = nullptr)
    {
        std::lock_guard lock(mutex);
        fence = f;
        value = v;
        submissionQueue = queue;
        state = State::Submitted;
        if (timingRequested && queue && FAILED(queue->GetTimestampFrequency(&frequency)))
        {
            frequency = 0;
        }
    }
    void RequestTiming()
    {
        std::lock_guard lock(mutex);
        timingRequested = true;
    }
    UINT64 TimestampFrequency() const
    {
        std::lock_guard lock(mutex);
        return frequency;
    }
    void Discard()
    {
        std::lock_guard lock(mutex);
        if (state == State::Pending)
        {
            state = State::Discarded;
        }
    }
    State Status() const
    {
        std::lock_guard lock(mutex);
        return state;
    }
    bool Complete() const
    {
        std::lock_guard lock(mutex);
        if (state != State::Submitted || !fence || !value)
        {
            return false;
        }
        auto done = fence->GetCompletedValue();
        return done != UINT64_MAX && done >= value;
    }
    bool SubmittedOn(ID3D12CommandQueue* queue) const
    {
        std::lock_guard lock(mutex);
        return queue && state == State::Submitted && fence && value && submissionQueue.Get() == queue;
    }

  private:
    mutable std::mutex mutex;
    State state = State::Pending;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> submissionQueue;
    UINT64 value{};
    UINT64 frequency{};
    bool timingRequested{};
};
}
