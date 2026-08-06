#include "engine.h"

#include <string.h>

b32 vs_valid_epoch_ms(i64 ms) { return ms >= VS_MS_MIN && ms <= VS_MS_MAX; }

b32 vs_status_loaded(const VsStatus *s) {
    return s->state == VS_PLAY_PLAYING || s->state == VS_PLAY_PAUSED;
}

const char *vs_play_state_name(VsPlayState s) {
    switch (s) {
        case VS_PLAY_PLAYING: return "playing";
        case VS_PLAY_PAUSED: return "paused";
        case VS_PLAY_STOPPED: return "stopped";
    }
    return "stopped";
}

const char *vs_cmd_name(VsCmdKind k) {
    switch (k) {
        case VS_CMD_PAUSE: return "pause";
        case VS_CMD_RESUME: return "resume";
        case VS_CMD_SEEK: return "seek";
        case VS_CMD_RATE: return "rate";
    }
    return "?";
}

const char *vs_action_name(VsAction a) {
    switch (a) {
        case VS_ACT_PLAY: return "play";
        case VS_ACT_PAUSE: return "pause";
        case VS_ACT_SEEK: return "seek";
    }
    return "?";
}

const char *vs_msg_name(VsMsgKind k) {
    switch (k) {
        case VS_MSG_PING: return "ping";
        case VS_MSG_SET_READY: return "setReady";
        case VS_MSG_SET_FILE: return "setFile";
        case VS_MSG_CONTROL: return "control";
        case VS_MSG_REPORT: return "report";
        case VS_MSG_CHAT: return "chat";
    }
    return "?";
}

void vs_output_reset(VsOutput *o) {
    o->cmd_count = 0;
    o->msg_count = 0;
    o->dropped = 0;
}

static void out_cmd(VsOutput *o, VsCmdKind kind, f64 value) {
    if (!o) return;
    if (o->cmd_count >= VS_MAX_CMDS) {
        o->dropped = 1;
        return;
    }
    VsCmd *c = &o->cmds[o->cmd_count++];
    c->kind = kind;
    c->value = value;
}

static VsMsg *out_msg(VsOutput *o, VsMsgKind kind) {
    static VsMsg sink;  // débordement : on écrit dans le vide plutôt que hors bornes
    if (!o) {
        memset(&sink, 0, sizeof(sink));
        return &sink;
    }
    if (o->msg_count >= VS_MAX_MSGS) {
        o->dropped = 1;
        memset(&sink, 0, sizeof(sink));
        return &sink;
    }
    VsMsg *m = &o->msgs[o->msg_count++];
    memset(m, 0, sizeof(*m));
    m->kind = kind;
    return m;
}

// ------------------------------------------------------------- utilitaires ---

f64 engine_clamp_position(f64 pos, f64 duration) {
    if (pos < 0) pos = 0;
    if (duration > 0 && pos > duration) pos = duration;
    return pos;
}

static f64 expect_predict(const VsExpectation *x, i64 now) {
    if (x->paused) return x->pos;
    f64 rate = x->rate;
    if (rate <= 0) rate = 1;
    f64 d = vs_ns_seconds(now - x->at);
    if (d < 0) d = 0;
    return x->pos + d * rate;
}

static f64 engine_duration(const VsEngine *e) {
    if (e->have_status && e->status.length_sec > 0) return e->status.length_sec;
    if (e->have_file && e->file_duration_sec > 0) return e->file_duration_sec;
    return 0;
}

f64 engine_room_rate(const VsEngine *e) {
    if (e->room_state.rate <= 0) return 1;
    return e->room_state.rate;
}

i64 engine_now_server_ms(const VsEngine *e, i64 now) {
    return vs_ns_to_unix_ms(now) + e->offset_ms;
}

f64 engine_expected_position(const VsEngine *e, i64 now) {
    if (!e->have_state) return 0;
    if (e->room_state.paused) {
        return e->room_state.position_sec > 0 ? e->room_state.position_sec : 0;
    }
    f64 elapsed = (f64)(engine_now_server_ms(e, now) - e->room_state.ref_server_ms) / 1000.0;
    f64 pos = e->room_state.position_sec + elapsed * engine_room_rate(e);
    if (pos < 0) pos = 0;
    return pos;
}

i64 engine_next_backoff(i64 current_ns) {
    i64 next = current_ns <= 0 ? VS_BACKOFF_MIN_NS : current_ns * 2;
    if (next > VS_BACKOFF_MAX_NS) next = VS_BACKOFF_MAX_NS;
    return next;
}

// ------------------------------------------------ détecteur de buffering ---

static void buf_reset(VsBufferDetect *b) {
    b->have = 0;
    b->buffering = 0;
    b->stall_from = VS_TIME_ZERO;
}

#define VS_BUF_WINDOW_NS (700 * 1000000LL)
#define VS_BUF_MIN_RATIO 0.25

static b32 buf_observe(VsBufferDetect *b, const VsStatus *st, i64 now) {
    if (st->state != VS_PLAY_PLAYING) {
        b->have = 1;
        b->last_pos = st->position_sec;
        b->last_at = now;
        b->stall_from = VS_TIME_ZERO;
        b->buffering = 0;
        return 0;
    }
    if (!b->have) {
        b->have = 1;
        b->last_pos = st->position_sec;
        b->last_at = now;
        return 0;
    }
    f64 elapsed = vs_ns_seconds(now - b->last_at);
    if (elapsed <= 0) return b->buffering;
    f64 progressed = st->position_sec - b->last_pos;
    b->last_pos = st->position_sec;
    b->last_at = now;
    if (progressed >= elapsed * VS_BUF_MIN_RATIO) {
        b->stall_from = VS_TIME_ZERO;
        b->buffering = 0;
        return 0;
    }
    if (b->stall_from == VS_TIME_ZERO) {
        b->stall_from = now;
        return b->buffering;
    }
    if (now - b->stall_from >= VS_BUF_WINDOW_NS) b->buffering = 1;
    return b->buffering;
}

// ------------------------------------------------------------ cycle de vie ---

void engine_init(VsEngine *e) {
    memset(e, 0, sizeof(*e));
    e->phase = VS_PHASE_IDLE;
    e->applied_rate = 1;
    e->room_state.paused = 1;
    e->room_state.rate = 1;
    e->grace_until = VS_TIME_ZERO;
    e->hold_until = VS_TIME_ZERO;
    e->user_hold_until = VS_TIME_ZERO;
    e->last_ping = VS_TIME_ZERO;
    e->last_report = VS_TIME_ZERO;
    buf_reset(&e->buf);
    e->buf.stall_from = VS_TIME_ZERO;
}

// invalidate_reference oublie tout ce qui sert à corriger : hors état
// connecté, l'état de référence est invalidé jusqu'au welcome suivant
// (docs/protocol.md §Conditions de correction).
static void invalidate_reference(VsEngine *e) {
    e->self_id.len = 0;
    e->self_id.data[0] = 0;
    e->have_state = 0;
    e->have_offset = 0;
    e->offset_count = 0;
    e->have_pending_rs = 0;
    e->user_hold_until = VS_TIME_ZERO;
    e->hold_until = VS_TIME_ZERO;
    e->nudging = 0;
    e->correcting = VS_CORRECT_NONE;
    e->drift = 0;
}

void engine_connecting(VsEngine *e) {
    e->phase = VS_PHASE_CONNECTING;
    invalidate_reference(e);
}

void engine_session_lost(VsEngine *e) {
    e->phase = VS_PHASE_CONNECTING;
    invalidate_reference(e);
}

void engine_disconnected(VsEngine *e) {
    e->phase = VS_PHASE_IDLE;
    invalidate_reference(e);
}

// ------------------------------------------------------------ état de salle ---

b32 engine_sanitize_roomstate(const VsRoomState *in, VsRoomState *out) {
    if (!f64_is_finite(in->position_sec) || in->position_sec < 0) return 0;
    // Position déraisonnable : au-delà d'un an de média, c'est du bruit.
    if (in->position_sec > 31536000.0) return 0;
    if (!f64_is_finite(in->rate) || in->rate < VS_MIN_RATE || in->rate > VS_MAX_RATE) return 0;
    if (!in->paused && in->ref_server_ms <= 0) return 0;
    // Horodatage hors bornes : refus (sinon débordement signé dans le calcul
    // de la position attendue).
    if (in->ref_server_ms != 0 && !vs_valid_epoch_ms(in->ref_server_ms)) return 0;
    *out = *in;
    return 1;
}

// adopt_room_state installe l'état de référence et arme la fenêtre de grâce.
static void adopt_room_state(VsEngine *e, i64 now, const VsRoomState *rs) {
    e->room_state = *rs;
    e->have_state = 1;
    e->grace_until = now + VS_GRACE_NS;
}

void engine_on_roomstate(VsEngine *e, i64 now, const VsRoomState *raw) {
    VsRoomState rs;
    if (!engine_sanitize_roomstate(raw, &rs)) return;  // §Assainissement
    if (e->user_hold_until != VS_TIME_ZERO && now < e->user_hold_until) {
        if (e->self_id.len > 0 && strbuf_eq(&rs.set_by, strbuf_str(&e->self_id))) {
            // Écho de notre propre control : le hold tombe.
            e->user_hold_until = VS_TIME_ZERO;
            e->have_pending_rs = 0;
            adopt_room_state(e, now, &rs);
            return;
        }
        // roomState d'autrui pendant le hold : mémorisé (le dernier gagne).
        e->pending_rs = rs;
        e->have_pending_rs = 1;
        return;
    }
    adopt_room_state(e, now, &rs);
}

void engine_on_pong(VsEngine *e, i64 now, VsPong p) {
    i64 now_ms = vs_ns_to_unix_ms(now);
    // Défense en profondeur : protocol.c valide déjà les bornes, mais le
    // moteur ne doit jamais pouvoir déborder sur une entrée aberrante.
    if (!vs_valid_epoch_ms(p.t) || !vs_valid_epoch_ms(p.server_ms) || !vs_valid_epoch_ms(now_ms)) return;
    i64 rtt = now_ms - p.t;
    if (rtt < 0) rtt = 0;
    i64 offset = p.server_ms + rtt / 2 - now_ms;
    if (e->offset_count < VS_OFFSET_SAMPLES) {
        e->offsets[e->offset_count++] = offset;
    } else {
        for (isize i = 1; i < VS_OFFSET_SAMPLES; i++) e->offsets[i - 1] = e->offsets[i];
        e->offsets[VS_OFFSET_SAMPLES - 1] = offset;
    }
    // Médiane des mesures conservées.
    i64 tmp[VS_OFFSET_SAMPLES];
    memcpy(tmp, e->offsets, sizeof(i64) * (size_t)e->offset_count);
    for (isize i = 1; i < e->offset_count; i++) {
        i64 v = tmp[i];
        isize j = i - 1;
        while (j >= 0 && tmp[j] > v) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = v;
    }
    e->offset_ms = e->offset_count > 0 ? tmp[e->offset_count / 2] : 0;
    e->latency_ms = rtt / 2;
    e->have_offset = 1;
}

void engine_on_self_ready(VsEngine *e, b32 ready) { e->ready = ready; }

void engine_on_welcome(VsEngine *e, i64 now, Str8 self_id, const VsRoomState *st,
                       const b32 *self_ready, VsOutput *out) {
    strbuf_set(&e->self_id, self_id);
    e->phase = VS_PHASE_CONNECTED;
    // Le welcome est la référence d'une session neuve : aucun hold ni
    // roomState en attente ne lui survit.
    e->user_hold_until = VS_TIME_ZERO;
    e->have_pending_rs = 0;
    e->hold_until = VS_TIME_ZERO;
    e->nudging = 0;
    engine_on_roomstate(e, now, st);
    if (self_ready) e->ready = *self_ready;
    // Re-déclarer notre état après un (re)join.
    if (e->have_file) {
        VsMsg *m = out_msg(out, VS_MSG_SET_FILE);
        m->name = e->file_name;
        m->duration_sec = e->file_duration_sec;
        m->size_bytes = e->file_size_bytes;
    }
    VsMsg *r = out_msg(out, VS_MSG_SET_READY);
    r->ready = e->ready;
    VsMsg *p = out_msg(out, VS_MSG_PING);
    p->t = vs_ns_to_unix_ms(now);
    e->last_ping = now;
}

// ------------------------------------------------------------------ lecteur ---

void engine_open_file(VsEngine *e, Str8 name, i64 size_bytes, VsOutput *out) {
    strbuf_set(&e->file_name, name);
    e->file_duration_sec = 0;
    e->file_size_bytes = size_bytes;
    e->have_file = 1;
    memset(&e->status, 0, sizeof(e->status));
    e->have_status = 0;
    memset(&e->expect, 0, sizeof(e->expect));
    buf_reset(&e->buf);
    e->buffering = 0;
    e->vlc_error = 0;
    e->applied_rate = 1;
    VsMsg *m = out_msg(out, VS_MSG_SET_FILE);
    m->name = e->file_name;
    m->duration_sec = 0;
    m->size_bytes = size_bytes;
}

// declare_file envoie setFile dès qu'un fichier (et sa durée) est connu.
static void declare_file(VsEngine *e, const VsStatus *st, VsOutput *out) {
    if (!vs_status_loaded(st) || st->length_sec <= 0) return;
    Str8 name = strbuf_str(&st->file_name);
    if (name.len == 0 && e->have_file) name = strbuf_str(&e->file_name);
    if (e->have_file && strbuf_eq(&e->file_name, name) &&
        f64_abs(e->file_duration_sec - st->length_sec) < 0.5) {
        return;
    }
    strbuf_set(&e->file_name, name);
    e->file_duration_sec = st->length_sec;
    e->have_file = 1;
    VsMsg *m = out_msg(out, VS_MSG_SET_FILE);
    m->name = e->file_name;
    m->duration_sec = st->length_sec;
    m->size_bytes = e->file_size_bytes;
}

// user_action remonte une action utilisateur et arme le hold de 2 s.
static void user_action(VsEngine *e, i64 now, VsAction act, f64 pos, VsOutput *out) {
    VsMsg *m = out_msg(out, VS_MSG_CONTROL);
    m->action = act;
    m->position_sec = engine_clamp_position(pos, engine_duration(e));
    e->grace_until = now + VS_GRACE_NS;
    e->user_hold_until = now + VS_USER_HOLD_NS;
    e->have_pending_rs = 0;
}

// detect_user_action compare l'observation à ce que le moteur attendait ;
// tout écart non provoqué par lui est une action de l'utilisateur dans VLC.
static void detect_user_action(VsEngine *e, i64 now, const VsStatus *st, VsOutput *out) {
    if (!e->expect.valid || e->phase != VS_PHASE_CONNECTED) return;
    if (!vs_status_loaded(st) || !vs_status_loaded(&e->status)) return;
    b32 now_paused = st->state != VS_PLAY_PLAYING;
    if (now_paused != e->expect.paused) {
        user_action(e, now, now_paused ? VS_ACT_PAUSE : VS_ACT_PLAY, st->position_sec, out);
        return;
    }
    if (f64_abs(st->position_sec - expect_predict(&e->expect, now)) > VS_USER_SEEK_SEC) {
        user_action(e, now, VS_ACT_SEEK, st->position_sec, out);
    }
}

void engine_on_vlc_error(VsEngine *e) {
    e->vlc_error = 1;
    e->have_status = 0;
    e->expect.valid = 0;
}

void engine_on_vlc_status(VsEngine *e, i64 now, const VsStatus *st, VsOutput *out) {
    e->vlc_error = 0;
    e->buffering = buf_observe(&e->buf, st, now);
    // Pendant la fenêtre de grâce, l'observation ne fait pas autorité : on ne
    // remplace pas l'attendu, sinon une action utilisateur survenue pendant la
    // grâce serait absorbée définitivement.
    b32 in_grace = now < e->grace_until;
    if (e->have_status && !in_grace) detect_user_action(e, now, st, out);
    e->status = *st;
    e->have_status = 1;
    // … sauf s'il n'y a encore aucune attente : il faut bien une référence de
    // départ, y compris si la première observation tombe pendant une grâce.
    if (!in_grace || !e->expect.valid) {
        e->expect.valid = 1;
        e->expect.paused = st->state != VS_PLAY_PLAYING;
        e->expect.pos = st->position_sec;
        e->expect.at = now;
        e->expect.rate = st->rate;
    }
    declare_file(e, st, out);
}

// ---------------------------------------------------------------- décisions ---

// arm met à jour ce que le moteur attend de VLC après ces commandes et arme
// les fenêtres anti-boucle. Les commandes sont déjà dans `out`.
static void arm(VsEngine *e, i64 now, const VsCmd *cmds, isize count) {
    if (count == 0) return;
    b32 hold = 0;
    for (isize i = 0; i < count; i++) {
        switch (cmds[i].kind) {
            case VS_CMD_PAUSE:
                e->expect.pos = expect_predict(&e->expect, now);
                e->expect.paused = 1;
                e->expect.at = now;
                hold = 1;
                break;
            case VS_CMD_RESUME:
                e->expect.pos = expect_predict(&e->expect, now);
                e->expect.paused = 0;
                e->expect.at = now;
                hold = 1;
                break;
            case VS_CMD_SEEK:
                e->expect.pos = f64_round(cmds[i].value);  // VLC arrondit à la seconde
                e->expect.at = now;
                buf_reset(&e->buf);
                hold = 1;
                break;
            case VS_CMD_RATE:
                e->expect.pos = expect_predict(&e->expect, now);
                e->expect.at = now;
                e->expect.rate = cmds[i].value;
                e->applied_rate = cmds[i].value;
                break;
        }
    }
    e->grace_until = now + VS_GRACE_NS;
    if (hold) e->hold_until = now + VS_GRACE_NS;
}

// plan décide des corrections à appliquer à VLC.
//
// Conditions de correction (docs/protocol.md) : état connecté, état de salle
// de référence valide, et au moins une mesure d'offset d'horloge.
static void plan(VsEngine *e, i64 now, VsOutput *out) {
    e->correcting = VS_CORRECT_NONE;
    if (!e->have_status || !vs_status_loaded(&e->status)) {
        e->drift = 0;
        return;
    }
    if (e->phase != VS_PHASE_CONNECTED || !e->have_state) {
        e->drift = 0;
        e->nudging = 0;
        return;
    }
    f64 base = engine_room_rate(e);
    f64 expected = engine_clamp_position(engine_expected_position(e, now), e->status.length_sec);
    f64 drift = e->status.position_sec - expected;
    e->drift = drift;
    if (!e->have_offset) return;              // pas encore d'horloge serveur fiable
    if (now < e->user_hold_until) return;     // hold post-action : on attend l'écho
    if (now < e->hold_until) return;          // une correction est déjà en vol

    VsCmd acts[VS_MAX_CMDS];
    isize n = 0;

    f64 abs_drift = f64_abs(drift);
    if (e->room_state.paused) {
        // En pause : jamais de nudge, seek uniquement au-delà du seuil (le seek
        // HTTP est arrondi à la seconde, il rapproche toujours sous ce seuil).
        e->nudging = 0;
        if (e->status.state == VS_PLAY_PLAYING) {
            acts[n].kind = VS_CMD_PAUSE;
            acts[n].value = 0;
            n++;
        }
        if (f64_abs(e->applied_rate - base) > 1e-3 || f64_abs(e->status.rate - base) > 1e-3) {
            acts[n].kind = VS_CMD_RATE;
            acts[n].value = base;
            n++;
        }
        if (abs_drift >= VS_PAUSED_SEEK_SEC) {
            acts[n].kind = VS_CMD_SEEK;
            acts[n].value = expected;
            n++;
            e->correcting = VS_CORRECT_SEEK;
        }
    } else if (e->status.state == VS_PLAY_PAUSED) {
        // Départ / reprise de lecture (docs/protocol.md §Départ et reprise) :
        // on cale d'abord VLC sur la position de référence, PUIS on joue. Le
        // nudge (5 %/s) mettrait 10 s à résorber 0,5 s d'écart de départ.
        if (abs_drift >= VS_START_SEEK_SEC) {
            acts[n].kind = VS_CMD_SEEK;
            acts[n].value = expected;
            n++;
            e->correcting = VS_CORRECT_SEEK;
            e->nudging = 0;
        }
        acts[n].kind = VS_CMD_RESUME;
        acts[n].value = 0;
        n++;
        if (f64_abs(e->applied_rate - base) > 1e-3 || f64_abs(e->status.rate - base) > 1e-3) {
            acts[n].kind = VS_CMD_RATE;
            acts[n].value = base;
            n++;
        }
    } else {
        f64 target = base;
        b32 want_rate = 0;
        if (abs_drift >= VS_HARD_SEEK_SEC) {
            e->correcting = VS_CORRECT_SEEK;
            e->nudging = 0;
            acts[n].kind = VS_CMD_SEEK;
            acts[n].value = expected;
            n++;
            want_rate = 1;
        } else if ((e->nudging && abs_drift >= VS_NUDGE_EXIT_SEC) ||
                   (!e->nudging && abs_drift > VS_DEAD_ZONE_SEC)) {
            // Hystérésis : on engage au-delà de 0,1 s, on ne lâche qu'en
            // dessous de 0,03 s — sinon le rate oscillerait autour du seuil.
            e->nudging = 1;
            e->correcting = VS_CORRECT_NUDGE;
            target = drift > 0 ? base * VS_NUDGE_SLOW   // en avance : ralentir
                               : base * VS_NUDGE_FAST;  // en retard : accélérer
            want_rate = 1;
        } else {
            e->nudging = 0;
            want_rate = 1;
        }
        if (want_rate &&
            (f64_abs(e->applied_rate - target) > 1e-3 || f64_abs(e->status.rate - target) > 1e-3)) {
            acts[n].kind = VS_CMD_RATE;
            acts[n].value = target;
            n++;
        }
    }

    if (n == 0) return;
    for (isize i = 0; i < n; i++) out_cmd(out, acts[i].kind, acts[i].value);
    arm(e, now, acts, n);
}

// expire_user_hold lève le hold post-action à échéance. Faute d'écho du
// serveur, le dernier roomState reçu pendant le hold (celui d'autrui, qui
// précédait forcément notre control) est appliqué à ce moment-là.
static void expire_user_hold(VsEngine *e, i64 now) {
    if (e->user_hold_until == VS_TIME_ZERO || now < e->user_hold_until) return;
    e->user_hold_until = VS_TIME_ZERO;
    if (!e->have_pending_rs) return;
    VsRoomState rs = e->pending_rs;
    e->have_pending_rs = 0;
    adopt_room_state(e, now, &rs);
}

// periodic programme ping (2 s) et report (1 s).
static void periodic(VsEngine *e, i64 now, VsOutput *out) {
    if (e->phase != VS_PHASE_CONNECTED) return;
    if (e->last_ping == VS_TIME_ZERO || now - e->last_ping >= VS_PING_EVERY_NS) {
        e->last_ping = now;
        VsMsg *m = out_msg(out, VS_MSG_PING);
        m->t = vs_ns_to_unix_ms(now);
    }
    if (e->have_status && (e->last_report == VS_TIME_ZERO || now - e->last_report >= VS_REPORT_EVERY_NS)) {
        e->last_report = now;
        VsMsg *m = out_msg(out, VS_MSG_REPORT);
        m->position_sec = e->status.position_sec;
        m->paused = e->status.state != VS_PLAY_PLAYING;
        m->buffering = e->buffering;
    }
}

void engine_on_tick(VsEngine *e, i64 now, VsOutput *out) {
    expire_user_hold(e, now);
    plan(e, now, out);
    periodic(e, now, out);
}

// -------------------------------------------------------- actions de l'UI ---

void engine_user_control(VsEngine *e, i64 now, VsAction action, f64 position_sec, b32 use_pos, VsOutput *out) {
    f64 pos = position_sec;
    if (!use_pos) {
        pos = (e->have_status && vs_status_loaded(&e->status)) ? e->status.position_sec
                                                              : engine_expected_position(e, now);
    }
    if (!f64_is_finite(pos)) return;  // §Assainissement
    pos = engine_clamp_position(pos, engine_duration(e));
    VsMsg *m = out_msg(out, VS_MSG_CONTROL);
    m->action = action;
    m->position_sec = pos;
    // Hold post-action : corrections suspendues jusqu'à l'écho ou l'expiration.
    e->user_hold_until = now + VS_USER_HOLD_NS;
    e->have_pending_rs = 0;
}

void engine_set_ready(VsEngine *e, b32 ready, VsOutput *out) {
    e->ready = ready;
    VsMsg *m = out_msg(out, VS_MSG_SET_READY);
    m->ready = ready;
}

void engine_chat(VsEngine *e, Str8 text, VsOutput *out) {
    VS_UNUSED(e);
    if (text.len == 0) return;
    VsMsg *m = out_msg(out, VS_MSG_CHAT);
    strbuf_set(&m->text, text);
}
