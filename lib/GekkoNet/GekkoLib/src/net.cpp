#include "gekko_types.h"
#include "net.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>

u32 Gekko::NetAddress::GetSize()
{
    return _size;
}

Gekko::NetAddress::NetAddress(void* data, u32 size)
{
    _size = size;
    _data = std::make_unique<u8[]>(_size);
    // copy address data
    std::memcpy(_data.get(), data, _size);
}

Gekko::NetAddress::NetAddress()
{
    _size = 0;
    _data = nullptr;
}

void Gekko::NetAddress::Copy(NetAddress* other)
{
    if (!other) {
        return;
    }

    _size = other->_size;

    if (_data) {
        _data.reset();
    }

    _data = std::make_unique<u8[]>(_size);
    // copy address data
    std::memcpy(_data.get(), other->GetAddress(), _size);
}

bool Gekko::NetAddress::Equals(NetAddress& other)
{
    return _size == other._size && std::memcmp(_data.get(), other._data.get(), _size) == 0;
}

u8* Gekko::NetAddress::GetAddress()
{
    return _data.get();
}

float Gekko::NetStats::CalculateJitter()
{
    if (rtt.size() < 2) {
        return 0.f;
    }

    float sum = 0.f;
    for (i32 i = 1; i < rtt.size(); ++i) {
        sum += std::abs((float)rtt[i] - (float)rtt[i - 1]);
    }

    float jitter = sum / (rtt.size() - 1);
    return jitter;
}

float Gekko::NetStats::CalculateAvgRTT()
{
    if (rtt.empty()) {
        return 0.f;
    }

    float sum = 0.f;
    for (i32 i = 0; i < rtt.size(); i++) {
        sum += rtt[i];
    }

    float avg_rtt = sum / rtt.size();
    return avg_rtt;
}

float Gekko::NetStats::CalculateRTTPercentile(float p)
{
    if (rtt.empty()) {
        return 0.f;
    }

    auto sorted = rtt;
    std::sort(sorted.begin(), sorted.end());

    const float clamped = std::max(0.f, std::min(1.f, p));
    const size_t idx = clamped <= 0.f
        ? 0
        : (size_t)std::ceil(clamped * (float)sorted.size()) - 1;
    return (float)sorted[std::min(idx, sorted.size() - 1)];
}

float Gekko::NetStats::CalculateJitterPercentile(float p)
{
    if (rtt.size() < 2) {
        return 0.f;
    }

    std::vector<float> deltas;
    deltas.reserve(rtt.size() - 1);
    for (i32 i = 1; i < rtt.size(); ++i) {
        deltas.push_back(std::abs((float)rtt[i] - (float)rtt[i - 1]));
    }

    std::sort(deltas.begin(), deltas.end());

    const float clamped = std::max(0.f, std::min(1.f, p));
    const size_t idx = clamped <= 0.f
        ? 0
        : (size_t)std::ceil(clamped * (float)deltas.size()) - 1;
    return deltas[std::min(idx, deltas.size() - 1)];
}

void Gekko::NetStats::AddRTT(u16 rtt_ms)
{
    rtt.push_back(rtt_ms);
    while (rtt.size() > RTT_HISTORY_SIZE) {
        rtt.erase(rtt.begin());
    }
}

void Gekko::NetStats::RecordInputDelivery(u32 input_slots,
                                          u32 accepted_inputs,
                                          u32 duplicate_inputs,
                                          u32 gap_inputs,
                                          u32 gap_before_frames,
                                          u32 gap_after_frames)
{
    const u32 observed = input_slots > 0
        ? input_slots
        : accepted_inputs + duplicate_inputs + gap_inputs;
    if (observed == 0 && gap_before_frames == 0 && gap_after_frames == 0) {
        return;
    }

    const float gap_sample = observed > 0
        ? std::min(1.0f, (float)gap_inputs / (float)observed)
        : 1.0f;
    packet_loss_ewma = packet_loss_ewma * 0.95f + gap_sample * 0.05f;

    const bool blocked_contiguous_frontier =
        gap_inputs > 0 || gap_after_frames > 0 || gap_before_frames > 0;
    if (blocked_contiguous_frontier) {
        const int burst_units = (int)std::max<u32>(1, std::max(gap_inputs, gap_after_frames));
        loss_burst_current = std::max(loss_burst_current + 1, burst_units);
        loss_burst_max = std::max(loss_burst_max, loss_burst_current);
    } else {
        loss_burst_current = 0;
        if (loss_burst_max > 0) {
            loss_burst_max--;
        }
        packet_loss_ewma *= 0.98f;
    }
}

void Gekko::NetStats::UpdateBandwidth()
{
    using namespace std::chrono;
    u64 now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();

    if (last_bandwidth_update == 0) {
        last_bandwidth_update = now;
        return;
    }

    u64 elapsed = now - last_bandwidth_update;
    if (elapsed >= 1000) {
        kb_sent_per_sec = (bytes_sent_accum * 1000.f / elapsed) / 1024.f;
        kb_received_per_sec = (bytes_received_accum * 1000.f / elapsed) / 1024.f;
        bytes_sent_accum = 0;
        bytes_received_accum = 0;
        last_bandwidth_update = now;
    }
}

u32 Gekko::NetStats::LastRTT()
{
    if (rtt.empty()) {
        return 0;
    }

    return rtt.back();
}
