/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* SEQ > AUTOMATION (Felucca's, without its CHANCE, RATCH, NUDGE and QUANTIZE rows; included by ui.c): the part's
 * automation as a list. The rows:
 *   PLAY     the first: the automation ON / OFF (KNOB 4; OFF keeps it) and its counts; OCT+ CLEAR asks
 *            "CLEAR Tn AUTOMATION?" (the locks and events go, the steps' VEL stays)
 *   LOCK / AUTO  a record (motion.c): its step, its parameter, its value. WaveStorm's per-step X / Y are locks of
 *            STORM's X and Y (EDIT 1)
 *   VEL      (Felucca 1.5, #173) a step's velocity (step_t.vel), the level its notes play at (seq.c seq_step; a step
 *            that never had one, 0, plays 96; live recording stores the played one: the panel's keys 100). Not listed
 *            until + ADD VEL's OCT+ (SHOW) lists every note step inside LEN, until HIDE or the page is left (a recorded
 *            pattern would fill the list otherwise). An ACC step plays 127 whatever its VEL (DIM)
 *   + ADD    the last
 * in the order of the steps (a step's VEL, then its records in the order they were added).
 *   KNOB 1 (or the PRESETS knob) the row
 *   KNOB 2 the step: a record moves to the next step where its parameter has no record (1 .. LEN); VEL none
 *   KNOB 3 a record's parameter: any one motion can record (motion.c motion_param) that the sound has, by name; it
 *          takes the next one with no record on that step, its value the sound's own (a lock that changes nothing yet);
 *          on + ADD, VEL comes first
 *   KNOB 4 the value (PLAY: ON / OFF; VEL 1..127)
 *   OCT+   on + ADD: a lock on the step and parameter KNOB 2 / 3 set there (the SEQ cursor's step and the first
 *          parameter of the sound page shown last, to begin with), at the sound's own value; VEL: SHOW / HIDE the VEL
 *          rows; on a record: its kind LOCK <-> AUTO (an automation event holds its value until the next one, a lock
 *          sounds on its step only); on PLAY: CLEAR (the dialog)
 *   EDIT   deletes the record, puts a VEL back to 96; OCT- goes HOME (as on the other action pages)
 * Every edit is one undo (SAVE held: ui.c motion_undo_take, step_undo_take; a knob turned on is one; PLAY is a setting).
 * AUTOMATION FULL when the 128 records are used */
static int32_t accel(uint32_t role, int32_t s, int32_t range);
static void confirm_open(uint32_t kind, uint32_t trk);

#define EV_VEL 0xFEu                                    /* + ADD's one before the parameters (ui.ev_id): a VEL (0xFF: none) */
enum { EVK_TOP, EVK_REC, EVK_ADD, EVK_VEL };
#define EVC(k, a) ((uint16_t)((uint32_t)(k) << 8 | (a)))   /* a row: its kind, its record (EVK_REC) or its step */
#define EV_ROWS (2u + MOTION_MAX + NSTEP)
static int ev_sid(uint32_t id) { return id == EV_VEL; }   /* a step's own field */
static int ev_skd(uint32_t k) { return k == EVK_VEL; }

/* id has a row's name on track t: motion records it, and the sound has it (the engine's own E1..E8 that it labels);
 * VEL always */
static int ev_id_ok(const track_t *t, uint32_t id)
{
    const param_desc_t *d;
    if (ev_sid(id))
        return 1;
    if (id >= P_COUNT || !motion_param(id))
        return 0;
    d = track_desc(t, id);
    return d && d->label && d->label[0] && d->label[0] != '-' && d->max > d->min;
}
/* its name in the list and on the card (b holds 12): the page's word before the card's label where the label alone
 * would say two things (ENV ATK, ENV FLT: ENV DEST's, LFO FLT: LFO DEST's) */
static void ev_name(const track_t *t, uint32_t id, char *b)
{
    const char *pre = id >= P_ATK && id <= P_ED_SHP ? "ENV " : id >= P_LRATE && id <= P_LD_AMP ? "LFO " : "";
    if (ev_sid(id)) {
        str_cpy(b, "VEL", 12);
        return;
    }
    str_cpy(b, pre, 12);
    str_cpy(b + str_len(b), id == P_LEVEL ? "LEVEL" : id == P_GLIDE ? "GLIDE" : track_desc(t, id)->label, 12 - str_len(b));
}

/* a step's VEL (kind EVK_VEL): its value, its default (96), at it, set (VEL 96 is stored as 0, as a step that never
 * had one) */
static int32_t ev_sval(const step_t *s, uint32_t k) { (void)k; return s->vel ? s->vel : 96; }
static int32_t ev_sdefv(uint32_t k) { (void)k; return 96; }
static int ev_sdef(const step_t *s, uint32_t k) { return ev_sval(s, k) == ev_sdefv(k); }
static void ev_sput(step_t *s, uint32_t k, int32_t v) { (void)k; s->vel = (uint8_t)(v == 96 ? 0 : v); }
static uint32_t ev_skind(uint32_t id) { (void)id; return EVK_VEL; }
/* a VEL row (shown): a note step; it plays: not an ACC step (127) */
static int ev_vel_row(const step_t *s) { return s->time == ST_NOTE && s->n; }
static int ev_vel_plays(const step_t *s) { return ev_vel_row(s) && !(s->flags & SF_ACCENT); }

/* id free on track t's step (no record of it there; VEL: at the default); every one past the steps */
static int ev_id_free(const track_t *t, uint32_t step, uint32_t id)
{
    if (step >= NSTEP)
        return 1;
    if (ev_sid(id))
        return ev_sdef(&t->step[step], ev_skind(id));
    return motion_find(t, step, id) < 0;
}
/* + ADD's order: VEL, then the parameters by id; (uint32_t)-1 before the first */
static int32_t ev_id_pos(uint32_t id) { return id == EV_VEL ? 0 : id < P_COUNT ? (int32_t)id + 1 : -1; }
static uint32_t ev_id_at(int32_t v) { return v == 0 ? EV_VEL : (uint32_t)(v - 1); }
/* the next one after id in direction dir (+1 / -1) that is free on track t's step; id itself when there is none */
static uint32_t ev_id_step(const track_t *t, uint32_t step, uint32_t id, int32_t dir)
{
    int32_t i = ev_id_pos(id);
    for (;;) {
        i += dir;
        if (i < 0 || i >= (int32_t)P_COUNT + 1)
            return id;
        if (ev_id_ok(t, ev_id_at(i)) && ev_id_free(t, step, ev_id_at(i)))
            return ev_id_at(i);
    }
}
static uint32_t ev_len(const track_t *t) { return (uint32_t)clamp(t->p[P_SLEN], 1, NSTEP); }

/* the rows of the selected track (rw[EV_ROWS]): their count, PLAY and + ADD included */
static uint32_t ev_list(uint16_t *rw)
{
    const track_t *t = TSEL;
    uint8_t idx[MOTION_MAX];
    uint32_t m = motion_rows(song.sel, idx), len = ev_len(t), n = 0, j = 0, s;
    rw[n++] = EVC(EVK_TOP, 0);
    for (s = 0; s < NSTEP; s++) {
        if (s < len && ((ui.ev_vel && ev_vel_row(&t->step[s])) || ui.ev_keep == EVC(EVK_VEL, s)))
            rw[n++] = EVC(EVK_VEL, s);
        for (; j < m && (motion.event[idx[j]].place & 63u) == s; j++)
            rw[n++] = EVC(EVK_REC, idx[j]);
    }
    rw[n++] = EVC(EVK_ADD, 0);
    return n;
}
static uint32_t ev_row(uint32_t n) { return ui.ev_row < n ? ui.ev_row : n - 1u; }
/* the row selected: its code (*np: the count) */
static uint32_t ev_cur(uint16_t *rw, uint32_t *np)
{
    uint32_t n = ev_list(rw);
    if (np)
        *np = n;
    return rw[ev_row(n)];
}
/* the step a row is on (+ ADD: where it adds) */
static uint32_t ev_step_of(uint32_t c)
{
    uint32_t k = c >> 8, a = c & 0xFFu;
    return k == EVK_REC ? motion.event[a].place & 63u : k == EVK_ADD ? ui.ev_step : a;
}

/* the page opened, or the track or its sound changed: + ADD's step and parameter inside what the track has */
static void ev_fix(void)
{
    const track_t *t = TSEL;
    if (ui.ev_step >= ev_len(t))
        ui.ev_step = (uint8_t)(ev_len(t) - 1u);
    if (!ev_id_ok(t, ui.ev_id)) {
        uint32_t id = lock_id(0);
        ui.ev_id = (uint8_t)(id != 0xFFu && ev_id_ok(t, id) ? id : ev_id_step(t, NSTEP, (uint32_t)-1, 1));
    }
}
static void ev_enter(void)
{
    ui.ev_step = (uint8_t)ui.cursor;
    ui.ev_id = 0xFFu;
    ui.ev_keep = 0;
    ui.ev_vel = 0;
    ev_fix();
}

/* the row showing c (after a move it may sit elsewhere in the order) */
static void ev_follow(uint32_t c)
{
    uint16_t rw[EV_ROWS];
    uint32_t n = ev_list(rw), r;
    for (r = 0; r < n && rw[r] != c; r++)
        ;
    if (r < n)
        ui.ev_row = (uint16_t)r;
}

/* what the list shows (graph_signature): the rows, their values, the one selected, PLAY, + ADD's place */
static uint32_t ev_sig(void)
{
    const track_t *t = TSEL;
    uint16_t rw[EV_ROWS];
    uint32_t n = ev_list(rw), h = 2166136261u, i, c;
    for (i = 0; i < n; i++) {
        c = rw[i];
        if (c >> 8 == EVK_REC)
            c ^= (uint32_t)motion.event[c & 0xFFu].place << 24 | (uint32_t)motion.event[c & 0xFFu].param << 16 |
                 (uint32_t)(uint16_t)motion.event[c & 0xFFu].value << 8;
        else if (ev_skd(c >> 8))
            c ^= (uint32_t)ev_sval(&t->step[c & 0xFFu], c >> 8) << 16;
        h = (h ^ c) * 16777619u;
    }
    return (h ^ ev_row(n) * 2654435761u ^ ((uint32_t)ui.ev_step << 8 | ui.ev_id) * 104729u ^
            (uint32_t)motion_enabled(t) * 7919u ^ ui.ev_vel * 15485863u) *
           16777619u;
}

static const char *ev_act_name(void)
{
    uint16_t rw[EV_ROWS];
    uint32_t c = ev_cur(rw, 0), k = c >> 8;
    if (k == EVK_TOP)
        return "CLEAR";
    if (k == EVK_ADD)
        return ui.ev_id != EV_VEL ? "ADD" : ui.ev_vel ? "HIDE" : "SHOW";
    if (k != EVK_REC)
        return "--";
    return (motion.event[c & 0xFFu].param & MOTION_LOCK) ? "AUTO" : "LOCK";   /* what OCT+ turns it into */
}
static int ev_act_ready(void)
{
    uint16_t rw[EV_ROWS];
    uint32_t c = ev_cur(rw, 0), k = c >> 8;
    if (k == EVK_TOP)
        return motion_count(TSEL) != 0u;
    if (k == EVK_ADD)
        return ev_sid(ui.ev_id) || (motion.count < MOTION_MAX && ev_id_ok(TSEL, ui.ev_id));
    return k == EVK_REC;
}

/* a VEL row (code c, kind k, step a) turned s on KNOB slot (4 the value; VEL: every note step has its row, KNOB 2
 * moves nothing) */
static void ev_step_knob(track_t *t, uint32_t slot, int32_t s, uint32_t c)
{
    uint32_t k = c >> 8, a = c & 0xFFu;
    int32_t v = ev_sval(&t->step[a], k);
    if (slot == 3u) {                                   /* the value: VEL 1..127 */
        v = clamp(v + accel(EN_K1 + slot, s, 126), 1, 127);
        if (v == ev_sval(&t->step[a], k))
            return;
        step_undo_take(t, 0x400u | slot | k << 4);
        ev_sput(&t->step[a], k, v);
        ui.ev_keep = (uint16_t)c;                      /* (back at its default it stays listed while edited) */
    } else {
        return;
    }
    motion_undo_done(t);
    ev_follow(c);
}

/* KNOB slot turned s on AUTOMATION */
static void ev_knob(uint32_t slot, int32_t s)
{
    track_t *t = TSEL;
    uint16_t rw[EV_ROWS];
    uint32_t n, c = ev_cur(rw, &n), r = ev_row(n), k = c >> 8, i, step, id;
    const motion_event_t *e;
    const param_desc_t *d;
    if (slot == 0u) {
        uint32_t to = (uint32_t)clamp((int32_t)r + s, 0, (int32_t)n - 1);
        if (to != r) {                                  /* another row: the one edited may leave the list now */
            ui.ev_keep = 0;
            ui.ev_row = (uint16_t)to;
            ev_follow(rw[to]);
        }
        return;
    }
    if (k == EVK_TOP) {                                 /* PLAY: KNOB 4 right ON, left OFF */
        if (slot == 3u)
            motion_set_enabled(t, s > 0);
        return;
    }
    if (k == EVK_ADD) {                                 /* + ADD: where it goes */
        ev_fix();
        if (slot == 1u)
            ui.ev_step = (uint8_t)clamp((int32_t)ui.ev_step + s, 0, (int32_t)ev_len(t) - 1);
        else if (slot == 2u)
            for (i = (uint32_t)(s > 0 ? s : -s); i; i--)
                ui.ev_id = (uint8_t)ev_id_step(t, NSTEP, ui.ev_id, s > 0 ? 1 : -1);
        return;
    }
    if (k != EVK_REC) {
        ev_step_knob(t, slot, s, c);
        return;
    }
    i = c & 0xFFu;
    e = &motion.event[i];
    step = e->place & 63u;
    id = MOTION_ID(e);
    if (slot == 1u) {                                   /* the next step where its parameter is free */
        uint32_t top = ev_len(t) > step ? ev_len(t) : step + 1u, m = (uint32_t)(s > 0 ? s : -s);
        int32_t to = (int32_t)step, at = (int32_t)step;
        while (m--) {                                   /* a detent each: the next free step */
            do
                at += s > 0 ? 1 : -1;
            while (at >= 0 && at < (int32_t)top && motion_find(t, (uint32_t)at, id) >= 0);
            if (at < 0 || at >= (int32_t)top)
                break;
            to = at;
        }
        if (to == (int32_t)step)
            return;
        motion_undo_take(t, 0x200u | slot | i << 4);   /* (a knob turned on, this record) */
        (void)motion_move(t, i, (uint32_t)to, id, e->value);
    } else if (slot == 2u) {                            /* another parameter, from the sound's own value */
        uint32_t to = id, m = (uint32_t)(s > 0 ? s : -s), nx;
        while (m--) {                                   /* a detent each: the next free parameter (no step field) */
            nx = ev_id_step(t, step, to, s > 0 ? 1 : -1);
            if (nx >= P_COUNT)
                break;
            to = nx;
        }
        if (to == id)
            return;
        motion_undo_take(t, 0x200u | slot | i << 4);   /* (a knob turned on, this record) */
        (void)motion_move(t, i, step, to, motion_base_value(t, to));
        ui.ev_id = (uint8_t)to;
    } else {                                            /* the value */
        d = track_desc(t, id);
        s = accel(EN_K1 + slot, s, d->fmt == F_ENUM ? 0 : d->max - d->min);
        motion_undo_take(t, 0x200u | slot | i << 4);   /* (a knob turned on, this record) */
        (void)motion_move(t, i, step, id, (int16_t)param_turn(d, e->value, s));
    }
    motion_undo_done(t);
    ev_follow(c);
}

/* OCT+: on + ADD what KNOB 2 / 3 set; a record's kind LOCK <-> AUTO; on PLAY, CLEAR (the dialog); a step's VEL:
 * nothing (its value is KNOB 4's) */
static void ev_oct(void)
{
    track_t *t = TSEL;
    uint16_t rw[EV_ROWS];
    uint32_t c = ev_cur(rw, 0), k = c >> 8;
    int32_t at;
    if (k == EVK_TOP) {
        if (!motion_count(t))
            ui_message("NOTHING TO CLEAR");
        else
            confirm_open(CF_CLEAR_MOTION, song.sel);
        return;
    }
    if (k == EVK_ADD) {
        ev_fix();
        if (!ev_id_ok(t, ui.ev_id))
            return;
        if (ui.ev_id == EV_VEL) {                       /* VEL: its rows shown / hidden (on the step, if a note) */
            ui.ev_vel ^= 1u;
            ui.ev_keep = 0;
            ev_follow(EVC(EVK_VEL, ui.ev_step));
            ui_message(ui.ev_vel ? "VEL SHOWN" : "VEL HIDDEN");
            return;
        }
        if ((at = motion_find(t, ui.ev_step, ui.ev_id)) >= 0) {   /* one is there already: show it */
            ev_follow(EVC(EVK_REC, at));
            ui_message("ALREADY LISTED");
            return;
        }
        if (motion.count >= MOTION_MAX) {
            ui_message("AUTOMATION FULL");
            return;
        }
        motion_undo_take(t, 0);
        (void)motion_set_lock(t, ui.ev_step, ui.ev_id, motion_base_value(t, ui.ev_id));
        motion_undo_done(t);
        if ((at = motion_find(t, ui.ev_step, ui.ev_id)) >= 0)
            ev_follow(EVC(EVK_REC, at));
        ui_message("LOCK ADDED");
        return;
    }
    if (k == EVK_REC) {
        const motion_event_t e = motion.event[c & 0xFFu];
        uint32_t lock = !(e.param & MOTION_LOCK);
        motion_undo_take(t, 0);
        (void)motion_put(t, e.place & 63u, MOTION_ID(&e), e.value, lock ? MOTION_LOCK : 0u);
        motion_undo_done(t);
        ui_message(lock ? "NOW A LOCK" : "NOW AUTOMATION");
    }
}

/* EDIT: the record goes, a VEL back to 96 (the row below moves up) */
static void ev_delete(void)
{
    track_t *t = TSEL;
    uint16_t rw[EV_ROWS];
    uint32_t n, c = ev_cur(rw, &n), r = ev_row(n), k = c >> 8;
    const motion_event_t *e;
    if (k == EVK_TOP || k == EVK_ADD) {
        ui_message("NOTHING TO DELETE");
        return;
    }
    if (k == EVK_REC) {
        e = &motion.event[c & 0xFFu];
        motion_undo_take(t, 0);
        motion_delete_event(t, e->place & 63u, MOTION_ID(e));
    } else {
        step_undo_take(t, 0);
        ev_sput(&t->step[c & 0xFFu], k, ev_sdefv(k));
        if (ui.ev_keep == c)
            ui.ev_keep = 0;
    }
    motion_undo_done(t);
    ui.ev_row = (uint16_t)r;                             /* (ev_row clamps it to the new count) */
    ui_message(k == EVK_VEL ? "VEL 96" : "DELETED");
}
