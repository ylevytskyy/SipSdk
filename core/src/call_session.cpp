#include "call_session.h"
#include "engine.h"

#include <re.h>
#include <baresip.h>

#include <chrono>
#include <cstring>

sip_call_t::sip_call_t(sip_client_t *owner, sip_core::Engine *engine, struct call *call,
                       const char *remote_uri, bool is_outgoing, bool with_video)
    : m_owner(owner)
    , m_engine(engine)
    , m_raw_call(call)
    , m_is_outgoing(is_outgoing)
    , m_has_video(with_video)
{
    if (call != nullptr) {
        const char *cid = call_id(call);
        if (cid) {
            m_id = cid;
        }
        const char *peer_name = call_peername(call);
        if (peer_name) {
            m_peer_display_name = peer_name;
        }
        const char *peer_uri = call_peeruri(call);
        if (peer_uri) {
            m_remote_uri = peer_uri;
        }
    }
    if (m_remote_uri.empty() && remote_uri) {
        m_remote_uri = remote_uri;
    }
    if (m_id.empty()) {
        m_id = "call_" + std::to_string(reinterpret_cast<uintptr_t>(this));
    }
    m_stats_tmr = static_cast<struct tmr *>(mem_zalloc(sizeof(struct tmr), nullptr));
    if (m_stats_tmr) {
        tmr_init(m_stats_tmr);
    }
    m_hold_check_tmr = static_cast<struct tmr *>(mem_zalloc(sizeof(struct tmr), nullptr));
    if (m_hold_check_tmr) {
        tmr_init(m_hold_check_tmr);
    }
}

sip_call_t::~sip_call_t()
{
    stop_stats_timer();
    stop_hold_check_timer();
    if (m_consult_call) {
        m_consult_call->release();
        m_consult_call = nullptr;
    }
    m_stats_tmr = static_cast<struct tmr *>(mem_deref(m_stats_tmr));
    m_hold_check_tmr = static_cast<struct tmr *>(mem_deref(m_hold_check_tmr));
    m_raw_call = nullptr;
}

const char *sip_call_t::get_id() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_id.c_str();
}

const char *sip_call_t::get_remote_uri() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_remote_uri.c_str();
}

const char *sip_call_t::get_peer_display_name() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_peer_display_name.c_str();
}

bool sip_call_t::has_video() const
{
    return m_has_video;
}

bool sip_call_t::is_outgoing() const
{
    return m_is_outgoing;
}

bool sip_call_t::is_on_hold() const
{
    return m_on_hold.load();
}

bool sip_call_t::is_muted() const
{
    return m_is_muted.load();
}

bool sip_call_t::is_closed() const
{
    return m_closed.load();
}

bool sip_call_t::is_secure() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_raw_call) return false;
    struct audio *au = call_audio(m_raw_call);
    if (!au) return false;
    struct stream *strm = audio_strm(au);
    if (!strm) return false;
    return stream_is_secure(strm);
}

int sip_call_t::get_stats(sip_call_stats_t *stats) const
{
    if (!stats) return EINVAL;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_raw_call) {
        *stats = m_last_stats;
        return 0;
    }

    std::memset(stats, 0, sizeof(*stats));

    // 1. Audio stream stats
    struct audio *au = call_audio(m_raw_call);
    if (au) {
        const struct aucodec *ac = audio_codec(au, false /* rx */);
        if (!ac) {
            ac = audio_codec(au, true /* tx */);
        }
        if (ac && ac->name) {
            snprintf(stats->audio_codec, sizeof(stats->audio_codec), "%s", ac->name);
        }

        struct stream *strm = audio_strm(au);
        if (strm) {
            uint32_t tx_bps = stream_metric_get_tx_bitrate(strm);
            double tx_avg = stream_metric_get_tx_avg_bitrate(strm);
            uint32_t rx_bps = stream_metric_get_rx_bitrate(strm);
            double rx_avg = stream_metric_get_rx_avg_bitrate(strm);
            uint32_t tx_bytes = stream_metric_get_tx_n_bytes(strm);
            uint32_t rx_bytes = stream_metric_get_rx_n_bytes(strm);
            uint32_t tx_pkts = stream_metric_get_tx_n_packets(strm);
            uint32_t rx_pkts = stream_metric_get_rx_n_packets(strm);

            // Bitrate calculation (bps -> kbps)
            uint32_t bps = tx_bps;
            if (bps == 0) {
                bps = static_cast<uint32_t>(tx_avg);
            }
            if (bps == 0) {
                bps = rx_bps;
            }
            if (bps == 0) {
                bps = static_cast<uint32_t>(rx_avg);
            }
            if (bps == 0) {
                uint32_t bytes = tx_bytes + rx_bytes;
                uint32_t pkts = tx_pkts + rx_pkts;
                if (pkts > 0 && bytes > 0) {
                    bps = (bytes * 8) / pkts * 50; // 50 pps for 20ms ptime
                }
            }
            stats->audio_bitrate_kbps = (bps + 500) / 1000;

            // RTCP stats
            const struct rtcp_stats *rtcp = stream_rtcp_stats(strm);
            if (rtcp) {
                stats->audio_rtt_ms = rtcp->rtt / 1000;
                uint32_t jit_us = rtcp->rx.jit ? rtcp->rx.jit : rtcp->tx.jit;
                stats->audio_jitter_ms = jit_us / 1000;

                uint32_t total = rtcp->rx.sent + (rtcp->rx.lost > 0 ? rtcp->rx.lost : 0);
                if (total > 0 && rtcp->rx.lost > 0) {
                    stats->audio_loss_rate = static_cast<float>(rtcp->rx.lost) / static_cast<float>(total);
                }
            }

            // Jitter buffer fallback
            struct jbuf_stat jstat{};
            if (stream_jbuf_stats(strm, &jstat) == 0) {
                if (stats->audio_jitter_ms == 0 && jstat.c_jitter > 0) {
                    stats->audio_jitter_ms = jstat.c_jitter;
                }
                if (stats->audio_loss_rate == 0.0f && (jstat.n_put + jstat.n_lost) > 0 && jstat.n_lost > 0) {
                    stats->audio_loss_rate = static_cast<float>(jstat.n_lost) / static_cast<float>(jstat.n_put + jstat.n_lost);
                }
            }

            // Packet error count fallback
            if (stats->audio_loss_rate == 0.0f) {
                uint32_t n_err = stream_metric_get_rx_n_err(strm);
                uint32_t n_pkts = stream_metric_get_rx_n_packets(strm);
                if ((n_pkts + n_err) > 0 && n_err > 0) {
                    stats->audio_loss_rate = static_cast<float>(n_err) / static_cast<float>(n_pkts + n_err);
                }
            }
        }
    }

    // 2. Video stream stats
    struct video *v = call_video(m_raw_call);
    if (v) {
        const struct vidcodec *vc = video_codec(v, false /* rx */);
        if (!vc) {
            vc = video_codec(v, true /* tx */);
        }
        if (vc && vc->name) {
            snprintf(stats->video_codec, sizeof(stats->video_codec), "%s", vc->name);
        }

        struct stream *vstrm = video_strm(v);
        if (vstrm) {
            uint32_t vbps = stream_metric_get_tx_bitrate(vstrm);
            if (vbps == 0) {
                vbps = static_cast<uint32_t>(stream_metric_get_tx_avg_bitrate(vstrm));
            }
            if (vbps == 0) {
                vbps = stream_metric_get_rx_bitrate(vstrm);
            }
            if (vbps == 0) {
                vbps = static_cast<uint32_t>(stream_metric_get_rx_avg_bitrate(vstrm));
            }
            stats->video_bitrate_kbps = (vbps + 500) / 1000;

            const struct rtcp_stats *vrtcp = stream_rtcp_stats(vstrm);
            if (vrtcp) {
                uint32_t vjit_us = vrtcp->rx.jit ? vrtcp->rx.jit : vrtcp->tx.jit;
                stats->video_jitter_ms = vjit_us / 1000;
                uint32_t vtotal = vrtcp->rx.sent + (vrtcp->rx.lost > 0 ? vrtcp->rx.lost : 0);
                if (vtotal > 0 && vrtcp->rx.lost > 0) {
                    stats->video_loss_rate = static_cast<float>(vrtcp->rx.lost) / static_cast<float>(vtotal);
                }
            }

            struct jbuf_stat vjstat{};
            if (stream_jbuf_stats(vstrm, &vjstat) == 0) {
                if (stats->video_jitter_ms == 0 && vjstat.c_jitter > 0) {
                    stats->video_jitter_ms = vjstat.c_jitter;
                }
                if (stats->video_loss_rate == 0.0f && (vjstat.n_put + vjstat.n_lost) > 0 && vjstat.n_lost > 0) {
                    stats->video_loss_rate = static_cast<float>(vjstat.n_lost) / static_cast<float>(vjstat.n_put + vjstat.n_lost);
                }
            }

            stats->video_fps = 30;
            const char *attr = sdp_media_rattr(stream_sdpmedia(vstrm), "framerate");
            if (attr) {
                double fps = atof(attr);
                if (fps > 0.0) {
                    stats->video_fps = static_cast<uint32_t>(fps + 0.5);
                }
            }
        }
    }

    const_cast<sip_call_t *>(this)->m_last_stats = *stats;
    return 0;
}

void sip_call_t::start_stats_timer()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_stats_tmr || m_stats_timer_running || m_closed.load()) {
        return;
    }
    m_stats_timer_running = true;
    tmr_start(m_stats_tmr, 2000, stats_timer_handler, this);
}

void sip_call_t::stop_stats_timer()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stats_tmr && m_stats_timer_running) {
        tmr_cancel(m_stats_tmr);
        m_stats_timer_running = false;
    }
}

void sip_call_t::stats_timer_handler(void *arg)
{
    auto *call = static_cast<sip_call_t *>(arg);
    if (!call || call->is_closed()) {
        return;
    }

    sip_call_stats_t stats{};
    if (call->get_stats(&stats) == 0 && call->m_engine) {
        call->m_engine->dispatch_call_stats(call, &stats);
    }

    std::lock_guard<std::mutex> lock(call->m_mutex);
    if (call->m_stats_tmr && call->m_stats_timer_running && !call->m_closed.load()) {
        tmr_start(call->m_stats_tmr, 2000, stats_timer_handler, call);
    }
}

void sip_call_t::start_hold_check_timer()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hold_check_tmr || m_closed.load()) {
        return;
    }
    m_hold_check_ticks.store(0);
    m_hold_reinvite_pending.store(true);
    tmr_start(m_hold_check_tmr, 60, hold_check_timer_handler, this);
}

void sip_call_t::stop_hold_check_timer()
{
    m_hold_reinvite_pending.store(false);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_hold_check_tmr) {
        tmr_cancel(m_hold_check_tmr);
    }
}

void sip_call_t::hold_check_timer_handler(void *arg)
{
    auto *call = static_cast<sip_call_t *>(arg);
    if (!call || call->is_closed() || !call->m_hold_reinvite_pending.load()) {
        return;
    }
    if (call->m_engine) {
        call->m_engine->evaluate_pending_hold_on_loop(call);
    }
    if (!call->is_closed() && call->m_hold_reinvite_pending.load()) {
        std::lock_guard<std::mutex> lock(call->m_mutex);
        if (call->m_hold_check_tmr) {
            tmr_start(call->m_hold_check_tmr, 60, hold_check_timer_handler, call);
        }
    }
}

uint32_t sip_call_t::get_duration() const
{
    if (m_closed.load()) {
        return m_duration.load();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_raw_call != nullptr) {
        uint32_t d = call_duration(m_raw_call);
        if (d > 0) {
            return d;
        }
    }
    if (m_is_established) {
        auto now = std::chrono::steady_clock::now();
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - m_established_at).count();
        return secs > 0 ? static_cast<uint32_t>(secs) : 0;
    }
    return m_duration.load();
}

void *sip_call_t::get_user_data() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_user_data;
}

void sip_call_t::set_user_data(void *data)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_user_data = data;
}

void sip_call_t::set_on_hold(bool hold)
{
    m_on_hold.store(hold);
}

void sip_call_t::set_muted(bool muted)
{
    m_is_muted.store(muted);
}

void sip_call_t::set_established()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_is_established = true;
    m_established_at = std::chrono::steady_clock::now();
}

void sip_call_t::set_closed(uint32_t dur)
{
    m_closed.store(true);
    stop_hold_check_timer();
    if (dur > 0) {
        m_duration.store(dur);
    } else {
        m_duration.store(get_duration());
    }
}

void sip_call_t::attach_raw_call(struct call *c)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_raw_call = c;
    if (c != nullptr) {
        const char *cid = call_id(c);
        if (m_id.empty() && cid && std::strlen(cid) > 0) {
            m_id = cid;
        }
        const char *peer_name = call_peername(c);
        if (peer_name && std::strlen(peer_name) > 0) {
            m_peer_display_name = peer_name;
        }
        const char *peer_uri = call_peeruri(c);
        if (peer_uri && std::strlen(peer_uri) > 0) {
            m_remote_uri = peer_uri;
        }
    }
}

void sip_call_t::retain()
{
    m_ref_count.fetch_add(1, std::memory_order_relaxed);
}

void sip_call_t::release()
{
    if (m_ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete this;
    }
}

void sip_call_t::detach_raw_call()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_raw_call = nullptr;
}

int sip_call_t::hangup(int code, const char *reason)
{
    if (!m_engine) {
        return EINVAL;
    }
    return m_engine->hangup_call(this, code, reason);
}

int sip_call_t::answer(bool with_video)
{
    if (!m_engine) {
        return EINVAL;
    }
    return m_engine->answer_call(this, with_video);
}

int sip_call_t::set_hold(bool hold)
{
    if (!m_engine) {
        return EINVAL;
    }
    return m_engine->hold_call(this, hold);
}

int sip_call_t::set_call_muted(bool muted)
{
    if (!m_engine) {
        return EINVAL;
    }
    return m_engine->mute_call(this, muted);
}

int sip_call_t::send_dtmf(const char *digits)
{
    if (!m_engine) {
        return EINVAL;
    }
    return m_engine->send_dtmf(this, digits);
}

int sip_call_t::request_keyframe()
{
    if (!m_engine) {
        return EINVAL;
    }
    return m_engine->request_keyframe(this);
}

int sip_call_t::transfer(const char *target_uri)
{
    if (!m_engine) return EINVAL;
    return m_engine->transfer_call(this, target_uri);
}

int sip_call_t::attended_transfer(sip_call_t *consult_call)
{
    if (!m_engine) return EINVAL;
    return m_engine->attended_transfer_call(this, consult_call);
}

int sip_call_t::accept_transfer(bool accept)
{
    if (!m_engine) return EINVAL;
    return m_engine->accept_transfer(this, accept);
}

std::string sip_call_t::pending_transfer_uri() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending_transfer_uri;
}

void sip_call_t::set_pending_transfer_uri(const std::string &uri)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending_transfer_uri = uri;
}

void sip_call_t::set_consult_call(sip_call_t *consult_call)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (consult_call) {
        consult_call->retain();
    }
    if (m_consult_call) {
        m_consult_call->release();
    }
    m_consult_call = consult_call;
}

sip_call_t *sip_call_t::take_consult_call()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    sip_call_t *c = m_consult_call;
    m_consult_call = nullptr;
    return c;
}
