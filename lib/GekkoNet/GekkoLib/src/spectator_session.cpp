#include "session.h"

#include "ui/log_window.h"


Gekko::SpectatorSession::SpectatorSession()
{
	_host = nullptr;
	_started = false;
    _delay_spectator = false;
    _last_saved_frame = GameInput::NULL_FRAME - 1;
    _config = GekkoConfig();
}

void Gekko::SpectatorSession::Init(GekkoConfig* config)
{
    _host = nullptr;

    _started = false;

    // get given configs
    std::memcpy(&_config, config, sizeof(GekkoConfig));

    // setup input buffer for the players (add size for spectator delay)
    u32 buffer_size = InputBuffer::DEFAULT_BUFF_SIZE + _config.spectator_delay;
    _sync.Init(_config.num_players, _config.input_size, buffer_size);

    // setup message system.
    // AS2 patch: plumb configurable liveness timeouts (0 = defaults).
    _msg.Init(_config.num_players, _config.input_size,
              _config.disconnect_timeout_ms, _config.interrupt_timeout_ms);

    // setup game event system
    _game_events.Init(_config.input_size * _config.num_players);

    // start paused so the buffer fills before playback begins
    _delay_spectator = (_config.spectator_delay > 0);
    LOG_GEKKO_INFO("[Gekko][SpectatorSession] Init players=%u input=%u delay=%u",
                   _config.num_players, _config.input_size, _config.spectator_delay);
}

void Gekko::SpectatorSession::SetLocalDelay(i32 player, u8 delay)
{
    // no-op: spectators don't have local players
}

void Gekko::SpectatorSession::SetNetAdapter(GekkoNetAdapter* adapter)
{
    _host = adapter;
    LOG_GEKKO_INFO("[Gekko][SpectatorSession] Net adapter set=%p", adapter);
}

i32 Gekko::SpectatorSession::AddActor(GekkoPlayerType type, GekkoNetAddress* addr)
{
    const i32 ERR = -1;
    std::unique_ptr<NetAddress> address;

    // only accept a single remote player (the host)
    if (type != GekkoRemotePlayer) {
        return ERR;
    }

    if (addr == nullptr) {
        return ERR;
    }

    if (!_msg.remotes.empty()) {
        return ERR;
    }

    address = std::make_unique<NetAddress>(addr->data, addr->size);

    u32 new_handle = (u32)_msg.remotes.size();
    _msg.remotes.push_back(std::make_unique<Player>(new_handle, type, address.get()));
    LOG_GEKKO_INFO("[Gekko][SpectatorSession] Add remote actor handle=%u", new_handle);

    return new_handle;
}

void Gekko::SpectatorSession::AddLocalInput(i32 player, void* input)
{
    // no-op: spectators don't add local input
}

GekkoGameEvent** Gekko::SpectatorSession::UpdateSession(i32* count)
{
    // reset session events
    _msg.session_events.Reset();

    // connection handling
    Poll();

    // clear GameEvents
    _game_events.Clear();

    // gameplay
    if (AllActorsValid()) {
        // reset the game event buffer before doing anything else
        _game_events.Reset();

        // spectator session buffer
        if (ShouldDelaySpectator()) {
            *count = _game_events.Count();
            return _game_events.Data();
        }

        // then advance the session
        if (_game_events.AddAdvanceEvent(_sync, false)) {
            _sync.IncrementFrame();
        }
    }

    *count = _game_events.Count();
    return _game_events.Data();
}

GekkoSessionEvent** Gekko::SpectatorSession::Events(i32* count)
{
    *count = (i32)_msg.session_events.GetRecentEvents().size();
    return _msg.session_events.GetRecentEvents().data();
}

f32 Gekko::SpectatorSession::FramesAhead()
{
    return 0.f;
}

void Gekko::SpectatorSession::NetworkStats(i32 player, GekkoNetworkStats* stats)
{
    for (auto& actor : _msg.remotes) {
        if (actor->handle == player) {
            actor->stats.UpdateBandwidth();
            stats->kb_sent = actor->stats.kb_sent_per_sec;
            stats->kb_received = actor->stats.kb_received_per_sec;
            stats->last_ping = actor->stats.LastRTT();
            stats->jitter = actor->stats.CalculateJitter();
            stats->avg_ping = actor->stats.CalculateAvgRTT();
            stats->rtt_p90 = actor->stats.CalculateRTTPercentile(0.90f);
            stats->rtt_p95 = actor->stats.CalculateRTTPercentile(0.95f);
            stats->jitter_p95 = actor->stats.CalculateJitterPercentile(0.95f);
            stats->packet_loss_ewma = actor->stats.packet_loss_ewma;
            stats->loss_burst_max = actor->stats.loss_burst_max;
            return;
        }
    }
}

void Gekko::SpectatorSession::NetworkPoll()
{
    _msg.session_events.Reset();
    Poll();

    if (!_started) {
        LOG_GEKKO_DEBUG("[Gekko][SpectatorSession] NetworkPoll pre-start remotes=%zu", _msg.remotes.size());
        AllActorsValid();
    }
}

i32 Gekko::SpectatorSession::CurrentFrame()
{
    return _sync.GetCurrentFrame();
}

i32 Gekko::SpectatorSession::LastReceivedFrame(i32 player)
{
    return _sync.GetLastReceivedFrom(player);
}

i32 Gekko::SpectatorSession::MinReceivedFrame()
{
    return _sync.GetMinReceivedFrame();
}

void Gekko::SpectatorSession::Poll()
{
	// return if no host is defined.
    if (!_host) {
        return;
    }

    // fetch data from network
    int length = 0;
	auto data = _host->receive_data(&length);

    // process the data we received
    _msg.HandleData(_host, data, length);

	// handle received inputs
	HandleReceivedInputs();

	// send network health update
	_msg.SendNetworkHealth();

	// now send data
	_msg.SendPendingOutput(_host);
}

bool Gekko::SpectatorSession::AllActorsValid()
{
	if (!_started) {
		if (!_msg.CheckStatusActors()) {
            LOG_GEKKO_DEBUG("[Gekko][SpectatorSession] Waiting for actors to finish sync");
			return false;
		}

		// if none returned that the session is ready!
        _msg.session_events.AddSessionStartedEvent();
        LOG_GEKKO_INFO("[Gekko][SpectatorSession] All actors valid — session started");
        if (_config.spectator_delay > 0) {
            _msg.session_events.AddSpectatorPausedEvent();
        }
        _started = true;

		return true;
	}

	return true;
}

void Gekko::SpectatorSession::HandleReceivedInputs()
{
    for (auto& remote : _msg.remotes) {
        if (remote->GetStatus() != Connected) continue;

        // spectators receive combined inputs for ALL players from the host
        std::vector<Handle> handles;
        for (u32 i = 0; i < _config.num_players; i++) {
            handles.push_back(i);
        }

        for (u32 i = 0; i < handles.size(); i++) {
            auto handle = handles[i];
            const Frame last_recv = _sync.GetLastReceivedFrom(handle) + 1;
            const Frame last_added = _msg.GetLastAddedInputFrom(handle);

            auto& input_q = _msg.GetNetPlayerQueue(handle);
            const Frame min_frame = last_added - (i32)input_q.size() + 1;
            for (int i = last_recv; i <= last_added; i++) {
                if (i >= min_frame) {
                    int current_idx = i - min_frame;
                    u8* input = input_q[current_idx].get();
                    _sync.AddRemoteInput(handle, input, i);
                    _msg.SendInputAck(handle, i, 0);
                }
            }
        }
    }
}

bool Gekko::SpectatorSession::ShouldDelaySpectator()
{
    if (_config.spectator_delay == 0) {
        return false;
    }

    const u32 delay = _config.spectator_delay;
    const Frame current = _sync.GetCurrentFrame();
    const Frame min = _sync.GetMinReceivedFrame();
    const u32 diff = std::abs(min - current);

    if (_delay_spectator) {
        if (diff >= delay) {
            _delay_spectator = false;
            _msg.session_events.AddSpectatorUnpausedEvent();
            return false;
        }
        return true;
    }

    // Re-pause only when the buffer is completely exhausted
    if (diff == 0) {
        _delay_spectator = true;
        _msg.session_events.AddSpectatorPausedEvent();
        return true;
    }

    return false;
}
