#include "backend.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdlib>

#include "ui/log_window.h"

// register poly types.
namespace
{
    zpp::serializer::register_types<
        zpp::serializer::make_type<Gekko::SyncMsg, zpp::serializer::make_id("Gekko::SyncMsg")>,
        zpp::serializer::make_type<Gekko::InputMsg, zpp::serializer::make_id("Gekko::InputMsg")>,
        zpp::serializer::make_type<Gekko::InputAckMsg, zpp::serializer::make_id("Gekko::InputAckMsg")>,
        zpp::serializer::make_type<Gekko::SessionHealthMsg, zpp::serializer::make_id("Gekko::SessionHealthMsg")>,
        zpp::serializer::make_type<Gekko::NetworkHealthMsg, zpp::serializer::make_id("Gekko::NetworkHealthMsg")>
    > _;

    const char* PacketTypeName(Gekko::PacketType type)
    {
        using namespace Gekko;
        switch (type) {
        case Inputs: return "Inputs";
        case SpectatorInputs: return "SpectatorInputs";
        case InputAck: return "InputAck";
        case SyncRequest: return "SyncRequest";
        case SyncResponse: return "SyncResponse";
        case SessionHealth: return "SessionHealth";
        case NetworkHealth: return "NetworkHealth";
        default: return "Unknown";
        }
    }

    const char* InputKindName(bool spectator)
    {
        return spectator ? "SpectatorInputs" : "Inputs";
    }

    bool GekkoNetTraceEnabled()
    {
        static int enabled = -1;
        if (enabled < 0) {
            const char* env = std::getenv("AS2_GEKKO_NET_TRACE");
            enabled = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
        }
        return enabled != 0;
    }

    void ResetNetoutAggregate(Gekko::NetoutAggregate& aggregate, u64 now)
    {
        aggregate = Gekko::NetoutAggregate{};
        aggregate.window_start_ms = now;
    }

    void ResetNetinAggregate(Gekko::NetinAggregate& aggregate, u64 now)
    {
        aggregate = Gekko::NetinAggregate{};
        aggregate.window_start_ms = now;
    }

    u32 MissingFramesCoveredByPacket(Frame start_frame, Frame last_frame, Frame contiguous_last_added)
    {
        if (last_frame < start_frame || last_frame <= contiguous_last_added) {
            return 0;
        }

        const Frame first_missing = std::max(start_frame, contiguous_last_added + 1);
        if (first_missing > last_frame) {
            return 0;
        }

        return (u32)(last_frame - first_missing + 1);
    }

    void RecordNetoutAggregate(std::map<Handle, Gekko::NetoutAggregate>& aggregates,
                               u64 now,
                               bool spectator,
                               Handle peer_handle,
                               bool cache_hit,
                               u32 packet_count,
                               Frame first_frame,
                               Frame last_frame,
                               Frame peer_last_ack,
                               u32 queue_size)
    {
        if (packet_count == 0) {
            return;
        }

        Gekko::NetoutAggregate& aggregate = aggregates[peer_handle];
        if (aggregate.window_start_ms == 0) {
            aggregate.window_start_ms = now;
        }

        aggregate.packets_sent += packet_count;
        if (cache_hit) {
            aggregate.cache_hit_resends++;
        } else {
            aggregate.cache_miss_rebuilds++;
        }
        if (last_frame >= first_frame) {
            aggregate.input_frames += (u32)(last_frame - first_frame + 1);
            aggregate.first_frame = std::min(aggregate.first_frame, first_frame);
            aggregate.last_frame = std::max(aggregate.last_frame, last_frame);
        }
        aggregate.peer_last_ack = peer_last_ack;
        aggregate.queue_size = queue_size;

        const bool trace = GekkoNetTraceEnabled();
        if (!trace && now - aggregate.window_start_ms < 1000) {
            return;
        }

        LOG_GEKKO_INFO(
            "GEKKO_NETOUT kind=%s peer=%d packets_sent=%u cache_hit_resends=%u cache_miss_rebuilds=%u input_frames=%u first_frame=%d last_frame=%d peer_last_ack=%d q_size=%u retry_ms=%llu window_ms=%llu trace=%d",
            InputKindName(spectator),
            peer_handle,
            aggregate.packets_sent,
            aggregate.cache_hit_resends,
            aggregate.cache_miss_rebuilds,
            aggregate.input_frames,
            aggregate.first_frame == INT_MAX ? -1 : aggregate.first_frame,
            aggregate.last_frame,
            aggregate.peer_last_ack,
            aggregate.queue_size,
            (unsigned long long)Gekko::NetStats::INPUT_RETRY_INTERVAL,
            (unsigned long long)(now - aggregate.window_start_ms),
            trace ? 1 : 0);
        ResetNetoutAggregate(aggregate, now);
    }

    void RecordNetinAggregate(std::map<Handle, Gekko::NetinAggregate>& aggregates,
                              u64 now,
                              bool spectator,
                              Handle peer_handle,
                              u32 input_slots,
                              u32 accepted_inputs,
                              u32 duplicate_inputs,
                              u32 gap_inputs,
                              u32 gap_before_frames,
                              u32 gap_after_frames,
                              Frame first_frame,
                              Frame last_frame,
                              Frame local_last_added,
                              u32 queue_size)
    {
        Gekko::NetinAggregate& aggregate = aggregates[peer_handle];
        if (aggregate.window_start_ms == 0) {
            aggregate.window_start_ms = now;
        }

        aggregate.packets_received++;
        aggregate.input_slots += input_slots;
        aggregate.accepted_inputs += accepted_inputs;
        aggregate.duplicate_inputs += duplicate_inputs;
        aggregate.gap_inputs += gap_inputs;
        aggregate.gap_before_frames += gap_before_frames;
        aggregate.gap_after_frames += gap_after_frames;
        if (last_frame >= first_frame) {
            aggregate.first_frame = std::min(aggregate.first_frame, first_frame);
            aggregate.last_frame = std::max(aggregate.last_frame, last_frame);
        }
        aggregate.local_last_added = local_last_added;
        aggregate.queue_size = queue_size;

        const bool trace = GekkoNetTraceEnabled();
        if (!trace && now - aggregate.window_start_ms < 1000) {
            return;
        }

        LOG_GEKKO_INFO(
            "GEKKO_NETIN kind=%s peer=%d packets=%u input_slots=%u accepted_inputs=%u duplicate_inputs=%u gap_inputs=%u gap_before=%u gap_after=%u first_frame=%d last_frame=%d local_last_added=%d q_size=%u window_ms=%llu trace=%d",
            InputKindName(spectator),
            peer_handle,
            aggregate.packets_received,
            aggregate.input_slots,
            aggregate.accepted_inputs,
            aggregate.duplicate_inputs,
            aggregate.gap_inputs,
            aggregate.gap_before_frames,
            aggregate.gap_after_frames,
            aggregate.first_frame == INT_MAX ? -1 : aggregate.first_frame,
            aggregate.last_frame,
            aggregate.local_last_added,
            aggregate.queue_size,
            (unsigned long long)(now - aggregate.window_start_ms),
            trace ? 1 : 0);
        ResetNetinAggregate(aggregate, now);
    }
}

Gekko::MessageSystem::MessageSystem()
{
    _num_players = 0;
	_input_size = 0;
    _last_sent_network_check = 0;

	// gen magic for session
    std::srand((unsigned int)std::time(nullptr));
	_session_magic = std::rand();
    LOG_GEKKO_INFO("[Gekko][MessageSystem] Constructed session_magic=0x%04X", _session_magic);

    session_events = SessionEventSystem();
}

void Gekko::MessageSystem::Init(u8 num_players, u32 input_size)
{
    _num_players = num_players;
	_input_size = input_size;

    _net_player_queue.resize(num_players);
    _netout_aggregate_by_peer.clear();
    _netin_aggregate_by_peer.clear();
    LOG_GEKKO_INFO("[Gekko][MessageSystem] Init players=%u input_size=%u input_retry_interval_ms=%llu",
        num_players,
        input_size,
        (unsigned long long)NetStats::INPUT_RETRY_INTERVAL);

}


bool Gekko::InputCache::IsValid(Frame current_ack, Frame current_last_input) const
{
    return !packets.empty()
        && last_acked_frame == current_ack
        && last_input_frame == current_last_input;
}

void Gekko::MessageSystem::NetInputQueue::TrimToAck(Frame min_ack, u32 max_size)
{
    if (inputs.empty()) return;

    // compute target size from ack
    u32 target = (u32)inputs.size();
    if (min_ack != INT_MAX) {
        Frame oldest = last_added_input - (Frame)inputs.size() + 1;
        Frame acked = std::max((Frame)0, min_ack - oldest + 1);
        target = (u32)inputs.size() - std::min((u32)acked, (u32)inputs.size());
    }

    // apply safety cap
    target = std::min(target, max_size);

    while (inputs.size() > target) {
        inputs.pop_front();
    }
}

void Gekko::MessageSystem::AddInput(Frame input_frame, Handle player, u8 input[], bool remote)
{
    auto& input_q = _net_player_queue[player];
	if (input_q.last_added_input + 1 == input_frame) {
        input_q.last_added_input++;
        input_q.inputs.push_back(std::make_unique<u8[]>(_input_size));
        std::memcpy(input_q.inputs.back().get(), input, _input_size);
	}

    // discard acked inputs (local) or just cap the queue (remote)
    Frame min_ack = remote ? (Frame)INT_MAX : GetMinLastAckedFrame(false);
    input_q.TrimToAck(min_ack, MAX_INPUT_QUEUE_SIZE);
}

void Gekko::MessageSystem::AddSpectatorInput(Frame input_frame, u8 input[])
{
    auto& input_q = _net_spectator_queue;
	if (input_q.last_added_input + 1 == input_frame) {
        input_q.last_added_input++;
        const size_t num_players = locals.size() + remotes.size();
        input_q.inputs.push_back(std::make_unique<u8[]>(_input_size * num_players));
        std::memcpy(input_q.inputs.back().get(), input, _input_size * num_players);
	}

    // discard acked inputs and cap the queue
    input_q.TrimToAck(GetMinLastAckedFrame(true), MAX_INPUT_QUEUE_SIZE);
}

void Gekko::MessageSystem::SendPendingOutput(GekkoNetAdapter* host)
{
	// send per-peer input packets to remotes
	if (!remotes.empty() && !locals.empty()) {
		for (auto& peer : remotes) {
			SendInputsToPeer(peer.get(), host, false);
		}
	}
	// check for disconnects (runs even in spectator sessions with no locals)
	if (!remotes.empty()) {
		HandleTooFarBehindActors(false);
	}

	// send per-peer input packets to spectators
	if (!spectators.empty() && !locals.empty()) {
		for (auto& peer : spectators) {
			SendInputsToPeer(peer.get(), host, true);
		}
	}
	// check for disconnects
	if (!spectators.empty()) {
		HandleTooFarBehindActors(true);
	}

	// drain remaining messages (acks, sync, health, etc.)
	while (!_pending_output.empty()) {
		auto& pkt = _pending_output.front();
        if ((pkt->pkt.header.type == SessionHealth || pkt->pkt.header.type == NetworkHealth) && pkt->addr.GetSize() == 0) {
            // send to remotes
            SendDataToAll(pkt.get(), host);
            // send to spectators
            SendDataToAll(pkt.get(), host, true);
        }
		else {
			SendDataTo(pkt.get(), host);
		}
		_pending_output.pop();
	}
}

void Gekko::MessageSystem::HandleData(GekkoNetAdapter* host, GekkoNetResult** data, u32 length)
{
    if (length > 0) {
        LOG_GEKKO_DEBUG("[Gekko][MessageSystem] HandleData count=%u", length);
    }
    for (u32 i = 0; i < length; i++) {
        auto res = data[i];
        auto addr = NetAddress(res->addr.data, res->addr.size);

        _bin_buffer.clear();

        try {
            _bin_buffer.insert(_bin_buffer.begin(), (u8*)res->data, (u8*)res->data + res->data_len);

            NetPacket pkt;
            zpp::serializer::memory_input_archive in(_bin_buffer);
            in(pkt.header, pkt.body);

            LOG_GEKKO_DEBUG("[Gekko][MessageSystem] Recv %s bytes=%u magic=0x%04X",
                            PacketTypeName(pkt.header.type),
                            res->data_len,
                            pkt.header.magic);

            ParsePacket(addr, pkt, res->data_len);
        }
        catch (const std::exception&) {
            printf("failed to deserialize packet\n");
        }

        // cleanup :)
        host->free_data(res->addr.data);
        host->free_data(res->data);
        host->free_data(res);
    }
}

void Gekko::MessageSystem::SendSyncRequest(NetAddress* addr)
{
    if (!addr) {
        return;
    }

    _pending_output.push(std::make_unique<NetData>());
	auto& message = _pending_output.back();

	message->addr.Copy(addr);

	message->pkt.header.type = SyncRequest;
	message->pkt.header.magic = 0;

    auto body = std::make_unique<SyncMsg>();
    body->rng_data = _session_magic;

    message->pkt.body = std::move(body);
    LOG_GEKKO_INFO("[Gekko][MessageSystem] Queue SyncRequest magic=0x%04X", _session_magic);
}

void Gekko::MessageSystem::SendSyncResponse(NetAddress* addr, u16 magic)
{
    if (!addr || magic == 0) {
        return;
    }

    _pending_output.push(std::make_unique<NetData>());
    auto& message = _pending_output.back();

	message->addr.Copy(addr);
	message->pkt.header.type = SyncResponse;
	message->pkt.header.magic = magic;

    auto body = std::make_unique<SyncMsg>();
    body->rng_data = _session_magic;

    message->pkt.body = std::move(body);
    LOG_GEKKO_INFO("[Gekko][MessageSystem] Queue SyncResponse remote_magic=0x%04X local_magic=0x%04X",
                   magic, _session_magic);
}

void Gekko::MessageSystem::SendInputAck(Handle player, Frame frame, i8 local_advantage)
{
	auto plyr = GetPlayerByHandle(player);

    if (!plyr) {
        return;
    }

    _pending_output.push(std::make_unique<NetData>());
    auto& message = _pending_output.back();

	message->addr.Copy(&plyr->address);
	message->pkt.header.magic = plyr->session_magic;
	message->pkt.header.type = InputAck;

    auto body = std::make_unique<InputAckMsg>();
	body->ack_frame = frame;
	body->frame_advantage = local_advantage;

    message->pkt.body = std::move(body);
}

std::vector<Handle> Gekko::MessageSystem::GetRemoteHandlesForAddress(NetAddress* addr)
{
	auto result = std::vector<Handle>();
	for (auto& player: remotes) {
		if (player->address.Equals(*addr)) {
			result.push_back(player->handle);
		}
	}
	return result;
}

Gekko::Player* Gekko::MessageSystem::GetPlayerByHandle(Handle handle) 
{
    std::vector<std::unique_ptr<Player>>* current = &locals;
    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &remotes;
        }

        for (auto& player : *current) {
            if (player->handle == handle) {
                return player.get();
            }
        }
    }

	return nullptr;
}

Frame Gekko::MessageSystem::GetMinLastAckedFrame(bool spectator) 
{
	Frame min = INT_MAX;
	for (auto& player : spectator ? spectators : remotes) {
		if (player->GetStatus() == Connected) {
			min = std::min(player->stats.last_acked_frame, min);
		}
	}
	return min;
}

Frame Gekko::MessageSystem::GetLastAddedInput(bool spectator)
{
    if (spectator) return _net_spectator_queue.last_added_input;

    Frame result = INT_MAX;
    for (auto& local : locals) {
        result = std::min(_net_player_queue[local->handle].last_added_input, result);
    }

    return result;
}

bool Gekko::MessageSystem::CheckStatusActors()
{
	i32 result = 0;
	u64 now = TimeSinceEpoch();

    std::vector<std::unique_ptr<Player>>* current = &remotes;

    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }
        for (auto& player : *current) {
            if (player->GetStatus() == Initiating) {
                if (player->stats.last_sent_sync_message + NetStats::SYNC_MSG_DELAY < now) {
                    if (player->sync_num == 0) {
                        SendSyncRequest(&player->address);
                        player->stats.last_sent_sync_message = now;
                        LOG_GEKKO_INFO("[Gekko][MessageSystem] Sent initial SyncRequest handle=%d", player->handle);
                    }
                    else if (player->sync_num < NUM_TO_SYNC) {
                        SendSyncResponse(&player->address, player->session_magic);
                        player->stats.last_sent_sync_message = now;
                        LOG_GEKKO_DEBUG("[Gekko][MessageSystem] Re-sent SyncResponse handle=%d sync=%u/%u magic=0x%04X",
                                        player->handle, player->sync_num, NUM_TO_SYNC, player->session_magic);
                    }
                    else {
                        player->SetStatus(Connected);
                        session_events.AddPlayerConnectedEvent(player->handle);
                        LOG_GEKKO_INFO("[Gekko][MessageSystem] Actor connected via CheckStatusActors handle=%d", player->handle);
                        result++;
                    }
                }
                result--;
            }
        }
    }

	return result == 0;
}

void Gekko::MessageSystem::SendSessionHealth(Frame frame, u32 checksum)
{
    _pending_output.push(std::make_unique<NetData>());
    auto& message = _pending_output.back();

    // the address and magic is set later so dont worry about it now
    message->pkt.header.type = SessionHealth;

    auto body = std::make_unique<SessionHealthMsg>();
    body->frame = frame;
    body->checksum = checksum;

    message->pkt.body = std::move(body);
}

void Gekko::MessageSystem::SendNetworkHealth()
{
    u64 now = TimeSinceEpoch();

    // dont want to spam the network with network health packets
    if (_last_sent_network_check + NetStats::NET_CHECK_DELAY > now) {
        return;
    }

    _pending_output.push(std::make_unique<NetData>());
    auto& message = _pending_output.back();

    // the address and magic is set later so dont worry about it now
    message->pkt.header.type = NetworkHealth;

    auto body = std::make_unique<NetworkHealthMsg>();
    body->send_time = now;
    body->received = false;

    message->pkt.body = std::move(body);

    _last_sent_network_check = now;
}

Frame Gekko::MessageSystem::GetLastAddedInputFrom(Handle player)
{
    return _net_player_queue[player].last_added_input;
}

std::deque<std::unique_ptr<u8[]>>& Gekko::MessageSystem::GetNetPlayerQueue(Handle player)
{
    return _net_player_queue[player].inputs;
}

void Gekko::MessageSystem::HandleTooFarBehindActors(bool spectator)
{
    const u64 now = TimeSinceEpoch();
	for (auto& actor : spectator ? spectators : remotes) {
		if (actor->GetStatus() == Connected) {
            // give the actor a chance to save itself.
            if (actor->stats.last_received_message == 0) {
                actor->stats.last_received_message = now;
                continue;
            }
            // check whether messages are being sent if not disconnect.
            const u64 msg_diff = now - actor->stats.last_received_message;
			if (msg_diff > NetStats::DISCONNECT_TIMEOUT) {
                session_events.AddPlayerDisconnectedEvent(actor->handle);
                actor->SetStatus(Disconnected);
                actor->sync_num = 0;
			}
		}
	}
}

u64 Gekko::MessageSystem::TimeSinceEpoch()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Gekko::MessageSystem::SendDataToAll(NetData* pkt, GekkoNetAdapter* host, bool spectators_only)
{
    auto& actors = spectators_only ? spectators : remotes;

    std::vector<u8> body_buffer;

    try {
        zpp::serializer::memory_output_archive out(body_buffer);
        out(pkt->pkt.body);
    }
    catch (const std::exception& e)
    {
        LOG_GEKKO_ERROR("[Gekko][MessageSystem] Failed to serialize packet body: %s", e.what());
        return;
    }


    for (auto& actor : actors) {
        _bin_buffer.clear();
        if (actor->address.GetSize() != 0 && actor->GetStatus() != Disconnected) {

            pkt->addr.Copy(&actor->address);
            pkt->pkt.header.magic = actor->session_magic;

            try {
                zpp::serializer::memory_output_archive out(_bin_buffer);
                out(pkt->pkt.header);
            }
            catch (const std::exception& e)
            {
                LOG_GEKKO_ERROR("[Gekko][MessageSystem] Failed to serialize packet header: %s", e.what());
                continue;
            }

            _bin_buffer.insert(
                _bin_buffer.end(),
                body_buffer.begin(),
                body_buffer.end()
            );

            auto addr = GekkoNetAddress();
            addr.data = actor->address.GetAddress();
            addr.size = actor->address.GetSize();

            host->send_data(&addr, (char*)_bin_buffer.data(), (int)_bin_buffer.size());
            actor->stats.bytes_sent_accum += (u32)_bin_buffer.size();
        }
    }
}

void Gekko::MessageSystem::SendDataTo(NetData* pkt, GekkoNetAdapter* host)
{
    _bin_buffer.clear();

    try {
        zpp::serializer::memory_output_archive out(_bin_buffer);
        out(pkt->pkt.header, pkt->pkt.body);
    }
    catch (const std::exception& e)
    {
        LOG_GEKKO_ERROR("[Gekko][MessageSystem] Failed to serialize packet: %s", e.what());
        return;
    }

    auto addr = GekkoNetAddress();
    addr.data = pkt->addr.GetAddress();
    addr.size = pkt->addr.GetSize();

    host->send_data(&addr, (char*)_bin_buffer.data(), (int)_bin_buffer.size());

    u32 sent_size = (u32)_bin_buffer.size();
    std::vector<std::unique_ptr<Player>>* current = &remotes;
    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }

        for (auto& actor : *current) {
            if (actor->address.Equals(pkt->addr)) {
                actor->stats.bytes_sent_accum += sent_size;
            }
        }
    }
}

void Gekko::MessageSystem::ParsePacket(NetAddress& addr, NetPacket& pkt, u32 packet_size)
{
    u64 now = TimeSinceEpoch();
    // update receive timers.
    std::vector<std::unique_ptr<Player>>* current = &remotes;
    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }

        for (auto& player : *current) {
            if (player->address.Equals(addr)) {
                player->stats.last_received_message = now;
                player->stats.bytes_received_accum += packet_size;
            }
        }
    }

    // handle packet.
    if (pkt.header.magic != _session_magic) {
        if (pkt.header.type == SyncRequest) {
            OnSyncRequest(addr, pkt);
        }
        else {
            printf("dropped packet!\n");
        }
    }
    else {
        switch (pkt.header.type)
        {
        case SyncResponse:
            OnSyncResponse(addr, pkt);
            return;
        case Inputs:
        case SpectatorInputs:
            OnInputs(addr, pkt);
            return;
        case InputAck:
            OnInputAck(addr, pkt);
            return;
        case SessionHealth:
            OnSessionHealth(addr, pkt);
            return;
        case NetworkHealth:
            OnNetworkHealth(addr, pkt);
            return;
        default:
            assert(false && "cannot process an unknown event!");
            return;
        }
    }
}

void Gekko::MessageSystem::OnSyncRequest(NetAddress& addr, NetPacket& pkt)
{
    i32 should_send = 0;
    u64 now = TimeSinceEpoch();
    auto body = (SyncMsg*)pkt.body.get();

    // handle requests and set the peer its session magic for both remotes and spectators
    std::vector<std::unique_ptr<Player>>* current = &remotes;
    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }

        for (auto& player : *current) {
            if (player->address.Equals(addr)) {
                player->session_magic = body->rng_data;
                LOG_GEKKO_INFO("[Gekko][MessageSystem] OnSyncRequest handle=%d remote_magic=0x%04X sync=%u",
                               player->handle, body->rng_data, player->sync_num);
                if (player->sync_num == 0) {
                    player->stats.last_sent_sync_message = now;
                    should_send++;
                }
            }
        }
    }

    if (should_send > 0) {
	    // send a packet containing the local session magic
	    SendSyncResponse(&addr, body->rng_data);
    }
}

void Gekko::MessageSystem::OnSyncResponse(NetAddress& addr, NetPacket& pkt)
{
    i32 should_send = 0;
    u64 now = TimeSinceEpoch();
    auto body = (SyncMsg*)pkt.body.get();

    // handle sync responses for both remotes and spectators
    std::vector<std::unique_ptr<Player>>* current = &remotes;
    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }

        for (auto& player : *current) {
            if (player->GetStatus() == Connected) {
                // connected but the remote is still asking ? maybe high packet loss? send a response again
                should_send++;
                continue;
            }

            if (player->address.Equals(addr)) {
                player->session_magic = body->rng_data;
                LOG_GEKKO_INFO("[Gekko][MessageSystem] OnSyncResponse handle=%d remote_magic=0x%04X sync=%u",
                               player->handle, body->rng_data, player->sync_num);
                if (player->sync_num < NUM_TO_SYNC) {
                    player->sync_num++;
                    should_send++;
                    player->stats.last_sent_sync_message = now;
                    session_events.AddPlayerSyncingEvent(
                        player->handle,
                        player->sync_num,
                        NUM_TO_SYNC
                    );
                    continue;
                }

                if (player->sync_num >= NUM_TO_SYNC) {
                    player->SetStatus(Connected);
                    session_events.AddPlayerConnectedEvent(player->handle);
                    LOG_GEKKO_INFO("[Gekko][MessageSystem] Actor connected handle=%d", player->handle);
                    continue;
                }
            }
        }
    }

    if (should_send > 0) {
    	// send a packet containing the local session magic
    	SendSyncResponse(&addr, body->rng_data);
    }
}

void Gekko::MessageSystem::OnInputs(NetAddress& addr, NetPacket& pkt)
{
    auto body = (InputMsg*)pkt.body.get();

    // RLE decompress if the sender compressed this packet
    if (body->compressed) {
        auto decompressed = Compression::RLEDecode(body->inputs.data(), (u32)body->inputs.size());
        body->inputs = std::move(decompressed);
    }

    const Frame start_frame = body->start_frame;
    const u32 input_count = body->input_count;

    const bool is_spectator = (pkt.header.type == SpectatorInputs);
    std::vector<Handle> handles;
    if (!is_spectator) {
        handles = GetRemoteHandlesForAddress(&addr);
        if (handles.empty()) {
            LOG_GEKKO_WARN("[Gekko][MessageSystem] Dropped Inputs from unknown address start=%d count=%u",
                           start_frame,
                           input_count);
            return;
        }
        for (Handle handle : handles) {
            if (handle < 0 || (size_t)handle >= _net_player_queue.size()) {
                LOG_GEKKO_WARN("[Gekko][MessageSystem] Dropped Inputs with invalid handle=%d queue_count=%zu",
                               handle,
                               _net_player_queue.size());
                return;
            }
        }
    }

    if (_input_size == 0 || input_count == 0) {
        LOG_GEKKO_WARN("[Gekko][MessageSystem] Dropped %s with invalid input_size=%u count=%u",
                       PacketTypeName(pkt.header.type),
                       _input_size,
                       input_count);
        return;
    }

    const size_t sender_slot_count = is_spectator ? _num_players : handles.size();
    if (sender_slot_count == 0) {
        LOG_GEKKO_WARN("[Gekko][MessageSystem] Dropped %s with no input slots",
                       PacketTypeName(pkt.header.type));
        return;
    }

    const size_t input_slots = (size_t)input_count * sender_slot_count;
    const size_t expected_size = input_slots * _input_size;
    if (body->inputs.size() < expected_size) {
        LOG_GEKKO_WARN("[Gekko][MessageSystem] Dropped short %s start=%d count=%u expected=%zu actual=%zu",
                       PacketTypeName(pkt.header.type),
                       start_frame,
                       input_count,
                       expected_size,
                       body->inputs.size());
        return;
    }

    const Frame last_packet_frame = input_count > 0
        ? start_frame + (Frame)input_count - 1
        : start_frame - 1;

    if (is_spectator) {
        u32 accepted_inputs = 0;
        u32 duplicate_inputs = 0;
        u32 gap_inputs = 0;
        u32 gap_before_frames = 0;
        u32 gap_after_frames = 0;
        for (u32 player = 0; player < _num_players; player++) {
            const Frame before_last_added = _net_player_queue[player].last_added_input;
            gap_before_frames += MissingFramesCoveredByPacket(start_frame,
                                                              last_packet_frame,
                                                              before_last_added);
        }

        for (u32 frame_idx = 0; frame_idx < input_count; frame_idx++) {
            const Frame recv_frame = start_frame + frame_idx;
            const size_t frame_offset = (size_t)frame_idx * _num_players * _input_size;

            for (u32 player = 0; player < _num_players; player++) {
                u8* input = &body->inputs[frame_offset + player * _input_size];
                auto& input_q = _net_player_queue[player];
                if (recv_frame == input_q.last_added_input + 1) {
                    accepted_inputs++;
                } else if (recv_frame <= input_q.last_added_input) {
                    duplicate_inputs++;
                } else {
                    gap_inputs++;
                }
                AddInput(recv_frame, player, input, true);
            }
        }

        Frame local_last_added = -1;
        u32 queue_size = 0;
        for (u32 player = 0; player < _num_players; player++) {
            local_last_added = std::max(local_last_added, _net_player_queue[player].last_added_input);
            queue_size += (u32)_net_player_queue[player].inputs.size();
            gap_after_frames += MissingFramesCoveredByPacket(start_frame,
                                                             last_packet_frame,
                                                             _net_player_queue[player].last_added_input);
        }
        Handle peer_handle = -1;
        auto sender_handles = GetRemoteHandlesForAddress(&addr);
        if (!sender_handles.empty()) {
            peer_handle = sender_handles[0];
        }

        RecordNetinAggregate(_netin_aggregate_by_peer,
                             TimeSinceEpoch(),
                             true,
                             peer_handle,
                             (u32)input_slots,
                             accepted_inputs,
                             duplicate_inputs,
                             gap_inputs,
                             gap_before_frames,
                             gap_after_frames,
                             start_frame,
                             last_packet_frame,
                             local_last_added,
                             queue_size);
    } else {
        const u32 player_count = (u32)handles.size();
        const u64 now = TimeSinceEpoch();

        for (u32 i = 0; i < player_count; i++) {
            const size_t player_offset = (size_t)i * input_count * _input_size;
            u32 accepted_inputs = 0;
            u32 duplicate_inputs = 0;
            u32 gap_inputs = 0;
            auto& input_q = _net_player_queue[handles[i]];
            const Frame before_last_added = input_q.last_added_input;
            const u32 gap_before_frames = MissingFramesCoveredByPacket(start_frame,
                                                                       last_packet_frame,
                                                                       before_last_added);

            for (u32 frame_idx = 0; frame_idx < input_count; frame_idx++) {
                const Frame recv_frame = start_frame + frame_idx;
                u8* input = &body->inputs[player_offset + frame_idx * _input_size];
                if (recv_frame == input_q.last_added_input + 1) {
                    accepted_inputs++;
                } else if (recv_frame <= input_q.last_added_input) {
                    duplicate_inputs++;
                } else {
                    gap_inputs++;
                }
                AddInput(recv_frame, handles[i], input, true);
            }

            auto player = GetPlayerByHandle(handles[i]);
            if (player) {
                player->stats.last_received_frame = now;
            }

            const u32 gap_after_frames = MissingFramesCoveredByPacket(start_frame,
                                                                      last_packet_frame,
                                                                      input_q.last_added_input);
            RecordNetinAggregate(_netin_aggregate_by_peer,
                                 now,
                                 false,
                                 handles[i],
                                 input_count,
                                 accepted_inputs,
                                 duplicate_inputs,
                                 gap_inputs,
                                 gap_before_frames,
                                 gap_after_frames,
                                 start_frame,
                                 last_packet_frame,
                                 input_q.last_added_input,
                                 (u32)input_q.inputs.size());
        }
    }
}

void Gekko::MessageSystem::OnInputAck(NetAddress& addr, NetPacket& pkt)
{
    auto body = (InputAckMsg*)pkt.body.get();
    const Frame ack_frame = body->ack_frame;
    const i8 remote_advantage = (i8)body->frame_advantage;

    for (auto& player : remotes) {
        if (player->address.Equals(addr) && player->stats.last_acked_frame < ack_frame) {
            player->stats.last_acked_frame = ack_frame;
            player->adv_history.SetRemoteAdvantage(remote_advantage);
        }
    }

    for (auto& player : spectators) {
        if (player->address.Equals(addr) && player->stats.last_acked_frame < ack_frame) {
            player->stats.last_acked_frame = ack_frame;
        }
    }
}

void Gekko::MessageSystem::OnSessionHealth(NetAddress& addr, NetPacket& pkt)
{
    auto body = (SessionHealthMsg*)pkt.body.get();

    const Frame frame = body->frame;
    const u32 checksum = body->checksum;

    for (auto& player : remotes) {
        if (player->address.Equals(addr)) {
            player->SetChecksum(frame, checksum);

            for (auto iter = player->session_health.begin();
                iter != player->session_health.end(); ) {
                if (iter->first < (_net_player_queue[player->handle].last_added_input - 128)) {
                    iter = player->session_health.erase(iter);
                } else {
                    ++iter;
                }
            }
            break;
        }
    }
}

void Gekko::MessageSystem::OnNetworkHealth(NetAddress& addr, NetPacket& pkt)
{
    auto body = (NetworkHealthMsg*)pkt.body.get();

    // ok if its not a returned packet then update it and send it back to its specifc peer.
    if (!body->received) {
        // find the sender in remotes or spectators
        Player* player = nullptr;
        auto handles = GetRemoteHandlesForAddress(&addr);
        if (!handles.empty()) {
            player = GetPlayerByHandle(handles.at(0));
        }

        // check spectators if not found in remotes
        if (!player) {
            for (auto& spec : spectators) {
                if (spec->address.Equals(addr)) {
                    player = spec.get();
                    break;
                }
            }
        }

        if (!player) {
            return;
        }

        _pending_output.push(std::make_unique<NetData>());
        auto& message = _pending_output.back();

        message->pkt.header.magic = player->session_magic;
        message->pkt.header.type = NetworkHealth;

        auto new_body = std::make_unique<NetworkHealthMsg>();
        new_body->send_time = body->send_time;
        new_body->received = true;

        message->pkt.body = std::move(new_body);
        message->addr.Copy(&addr);
        return;
    }

    // else update network stats
    u16 rtt_ms = (u16)(TimeSinceEpoch() - body->send_time);
    std::vector<std::unique_ptr<Player>>* current = &remotes;

    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }

        for (auto& actor : *current) {
            if (addr.Equals(actor->address)) {
                actor->stats.AddRTT(rtt_ms);
            }
        }
    }
}

void Gekko::MessageSystem::SendInputsToPeer(Player* peer, GekkoNetAdapter* host, bool spectator)
{
    if (peer->address.GetSize() == 0 || peer->GetStatus() == Disconnected) {
        return;
    }

    const u32 MAX_INPUT_SIZE = 512;
    const auto packet_type = spectator ? SpectatorInputs : Inputs;
    auto& queue = spectator ? _net_spectator_queue : _net_player_queue[locals[0]->handle];
    const u32 num_players = spectator ? _num_players : (u32)locals.size();
    const u32 input_stride = _input_size * num_players;
    if (input_stride == 0 || input_stride > MAX_INPUT_SIZE) {
        LOG_GEKKO_WARN("[Gekko][MessageSystem] Cannot send %s: input_size=%u players=%u stride=%u max=%u",
                       PacketTypeName(packet_type),
                       _input_size,
                       num_players,
                       input_stride,
                       MAX_INPUT_SIZE);
        return;
    }

    const u32 inputs_per_packet = MAX_INPUT_SIZE / input_stride;
    const u32 q_size = (u32)queue.inputs.size();

    if (q_size == 0) return;

    const Frame queue_oldest_frame = queue.last_added_input - (Frame)q_size + 1;
    const Frame last_input = spectator ? _net_spectator_queue.last_added_input : GetLastAddedInput(false);

    // peer is caught up, nothing to send
    if (peer->stats.last_acked_frame >= last_input) return;

    // check per-peer cache
    if (peer->input_cache.IsValid(peer->stats.last_acked_frame, last_input)) {
        // cache hit: nothing new to send, this is a pure re-send — rate limit it
        const u64 now = TimeSinceEpoch();
        if (peer->last_input_send_time + NetStats::INPUT_RETRY_INTERVAL > now) {
            return;
        }
        u32 packet_count = 0;
        Frame first_frame = INT_MAX;
        Frame last_frame = -1;
        for (const auto& cached_msg : peer->input_cache.packets) {
            NetData data;
            data.addr.Copy(&peer->address);
            data.pkt.header.type = packet_type;
            data.pkt.header.magic = peer->session_magic;

            auto message = std::make_unique<InputMsg>();
            message->Copy(&cached_msg);
            data.pkt.body = std::move(message);

            SendDataTo(&data, host);
            packet_count++;
            first_frame = std::min(first_frame, cached_msg.start_frame);
            if (cached_msg.input_count > 0) {
                last_frame = std::max(last_frame,
                                      cached_msg.start_frame + (Frame)cached_msg.input_count - 1);
            }
        }
        RecordNetoutAggregate(_netout_aggregate_by_peer,
                              now,
                              spectator,
                              peer->handle,
                              true,
                              packet_count,
                              first_frame,
                              last_frame,
                              peer->stats.last_acked_frame,
                              q_size);
        peer->last_input_send_time = now;
        return;
    }

    // cache miss: rebuild packets for this peer
    const Frame peer_start_frame = std::max(peer->stats.last_acked_frame + 1, queue_oldest_frame);
    const u32 peer_start_idx = (u32)(peer_start_frame - queue_oldest_frame);
    const u32 peer_input_count = q_size - peer_start_idx;

    if (peer_input_count == 0) return;

    peer->input_cache.packets.clear();

    const u32 packet_count = (peer_input_count + inputs_per_packet - 1) / inputs_per_packet;

    for (u32 pc = 0; pc < packet_count; pc++) {
        const u32 input_start_idx = peer_start_idx + pc * inputs_per_packet;
        const u32 input_end_idx = std::min(peer_start_idx + peer_input_count, input_start_idx + inputs_per_packet);
        const u32 input_count = input_end_idx - input_start_idx;

        InputMsg msg;
        msg.start_frame = queue_oldest_frame + input_start_idx;

        if (spectator) {
            for (u32 i = input_start_idx; i < input_end_idx; i++) {
                const auto& p_input = queue.inputs.at(i);
                msg.inputs.insert(msg.inputs.end(),
                    p_input.get(),
                    p_input.get() + _input_size * num_players);
            }
        }
        else {
            for (u32 player = 0; player < num_players; player++) {
                const auto& player_queue = _net_player_queue[locals[player]->handle];
                for (u32 i = input_start_idx; i < input_end_idx; i++) {
                    const auto& p_input = player_queue.inputs.at(i);
                    msg.inputs.insert(msg.inputs.end(),
                        p_input.get(),
                        p_input.get() + _input_size);
                }
            }
        }

        // RLE compress only when it actually reduces size
        msg.compressed = false;
        auto compressed = Compression::RLEEncode(msg.inputs.data(), (u32)msg.inputs.size());
        if (compressed.size() < msg.inputs.size()) {
            msg.inputs = std::move(compressed);
            msg.compressed = true;
        }

        msg.total_size = (u16)msg.inputs.size();
        msg.input_count = input_count;

        // cache and send
        InputMsg cached_msg;
        cached_msg.Copy(&msg);
        peer->input_cache.packets.push_back(std::move(cached_msg));

        NetData data;
        data.addr.Copy(&peer->address);
        data.pkt.header.type = packet_type;
        data.pkt.header.magic = peer->session_magic;
        data.pkt.body = std::make_unique<InputMsg>(std::move(msg));

        SendDataTo(&data, host);
    }

    const u64 send_time = TimeSinceEpoch();
    RecordNetoutAggregate(_netout_aggregate_by_peer,
                          send_time,
                          spectator,
                          peer->handle,
                          false,
                          packet_count,
                          peer_start_frame,
                          peer_start_frame + (Frame)peer_input_count - 1,
                          peer->stats.last_acked_frame,
                          q_size);
    peer->last_input_send_time = send_time;

    // update cache keys
    peer->input_cache.last_acked_frame = peer->stats.last_acked_frame;
    peer->input_cache.last_input_frame = last_input;
}

void Gekko::AdvantageHistory::Init()
{
	_local_frame_adv = 0;
    _remote_frame_adv = 0;
	std::memset(_local, 0, HISTORY_SIZE * sizeof(i8));
	std::memset(_remote, 0, HISTORY_SIZE * sizeof(i8));
}

void Gekko::AdvantageHistory::Update(Frame frame)
{
	const u32 update_frame = std::max(frame, 0);
	_local[update_frame % HISTORY_SIZE] = _local_frame_adv;
	_remote[update_frame % HISTORY_SIZE] = _remote_frame_adv;
}

f32 Gekko::AdvantageHistory::GetAverageAdvantage()
{
	f32 sum_local = 0.f;
	f32 sum_remote = 0.f;

	for (i32 i = 0; i < HISTORY_SIZE; i++) {
		sum_local += _local[i];
		sum_remote += _remote[i];
	}

	f32 avg_local = sum_local / HISTORY_SIZE;
	f32 avg_remote = sum_remote / HISTORY_SIZE;

	// return the frames ahead (halved: each peer corrects its share of the gap)
	return (avg_local - avg_remote) / 2.f;
}

void Gekko::AdvantageHistory::SetLocalAdvantage(i8 adv) {
	_local_frame_adv = adv;
}

void Gekko::AdvantageHistory::SetRemoteAdvantage(i8 adv) {
    _remote_frame_adv = adv;
}

