/* Motion decision logic: see md_logic.h and docs/motion.md.
 * Everything per frame is O(cells) (768 at most) plus O(tracks x blobs)
 * (8 x 16); no allocation, no recursion. */
#include "md_logic.h"
#include <string.h>

#define NB_SW (MDL_COLS * MDL_ROWS_SW)
#define TIME_MAX ((int64_t)1 << 52)     /* clamp: differences can never overflow */

/* stock table @0x410480: minimum region size (cells) per sensitivity */
static const uint8_t k_min_cells[101] = {
    50,49,48,47,46,45,44,43,42,41,40,39,38,37,36,35,34,33,32,31,30,29,28,27,27,26,25,24,24,23,22,21,21,20,19,18,18,17,16,15,15,14,
    14,13,13,12,12,12,12,12,12,12,12,11,11,11,11,11,11,11,11,10,10,10,10,9,9,9,9,9,9,9,9,8,8,8,8,8,8,8,8,7,7,7,7,6,6,6,6,6,6,6,6,
    5,5,5,5,4,4,4,4 };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void mdl_init(mdl_t *st)
{
    memset(st, 0, sizeof *st);
    st->base50 = -1;
    st->base90 = -1;
    st->grid_rows = -1;
    st->raw.sens = -1;                  /* forces the first configuration */
}

static void tracks_clear(mdl_t *st)
{
    for (int i = 0; i < MDL_MAX_TRACKS; i++) st->trk[i].id = 0;
}

void mdl_disabled(mdl_t *st)
{
    st->have_bg = 0;
    st->motion = 0;
    st->motion_any = 0;
    st->line_on = 0;
    st->loiter_on = 0;
    tracks_clear(st);
}

void mdl_night_switch(mdl_t *st, int64_t now_ms, int hw)
{
    if (now_ms < 0) now_ms = 0;
    if (now_ms > TIME_MAX) now_ms = TIME_MAX;
    st->have_bg = 0;
    st->quiet_until = now_ms + (hw ? 10000 : 5000);    /* IR switching: stock pauses 5 s; the 3D-NR statistic settles in ~8 s */
    tracks_clear(st);
}

/* ---- settings ---------------------------------------------------------------- */
static void cfg_update(mdl_t *st, const mdl_cfg_t *in, int rows)
{
    if (!memcmp(in, &st->raw, sizeof *in) && st->grid_rows == rows) return;
    st->raw = *in;
    mdl_cfg_t c = *in;
    c.sens = clampi(c.sens, 1, 100);
    c.hold_s = clampi(c.hold_s, 2, 300);
    c.nzones = clampi(c.nzones, 0, MDL_MAX_ZONES);
    for (int k = 0; k < c.nzones; k++) {
        mdl_zone_t *z = &c.zone[k];
        int a = clampi(z->c0, 0, MDL_COLS - 1), b = clampi(z->c1, 0, MDL_COLS - 1);
        z->c0 = a < b ? a : b; z->c1 = a < b ? b : a;
        a = clampi(z->r0, 0, MDL_ROWS_SW - 1); b = clampi(z->r1, 0, MDL_ROWS_SW - 1);
        z->r0 = a < b ? a : b; z->r1 = a < b ? b : a;
        z->sens = clampi(z->sens, 1, 100);
    }
    c.min_size = clampi(c.min_size, 0, 100);
    c.persist_ms = clampi(c.persist_ms, 0, 60000);
    c.light_pct = c.light_pct <= 0 ? 0 : clampi(c.light_pct, 10, 90);
    for (int i = 0; i < 4; i++) c.line[i] = clampi(c.line[i], 0, 1000);
    {
        int dx = c.line[2] - c.line[0], dy = c.line[3] - c.line[1];
        if (dx * dx + dy * dy < 400) c.line_on = 0;
    }
    c.line_on = c.line_on != 0;
    if (c.line_dir < 0 || c.line_dir > 2) c.line_dir = 0;
    c.loiter_s = clampi(c.loiter_s, 0, 3600);
    c.loiter_zone = clampi(c.loiter_zone, 0, MDL_MAX_ZONES);
    if (c.loiter_zone > c.nzones) c.loiter_zone = 0;
    st->cfg = c;
    st->grid_rows = rows;
    memset(st->zmap, 0, sizeof st->zmap);
    for (int r = 0; r < rows; r++) {
        int ur = r * MDL_ROWS_SW / rows;                 /* same mapping as the watch mask */
        for (int col = 0; col < MDL_COLS; col++)
            for (int k = 0; k < c.nzones; k++)
                if (col >= c.zone[k].c0 && col <= c.zone[k].c1 && ur >= c.zone[k].r0 && ur <= c.zone[k].r1)
                    st->zmap[r * MDL_COLS + col] = (uint8_t)(k + 1);
    }
}

static int64_t clamp_time(int64_t t) { return t < 0 ? 0 : t > TIME_MAX ? TIME_MAX : t; }

/* ---- connected components ------------------------------------------------------ */
typedef struct { int n, sc, sr; } blob_t;

/* Components of m (nonzero cells), 4- or 8-connected. With filter > 0 the cells of
 * components smaller than filter are cleared in m. Blobs of >= min_keep cells go
 * to bl (the largest maxb). Returns the number stored. */
static int components(mdl_t *st, uint8_t *m, int rows, int conn, int filter, int min_keep, blob_t *bl, int maxb)
{
    int ncell = rows * MDL_COLS, nb = 0;
    memset(st->seen, 0, (size_t)ncell);
    for (int i = 0; i < ncell; i++) {
        if (!m[i] || st->seen[i]) continue;
        int sp = 0, n = 0, sc = 0, sr = 0;
        st->stack[sp++] = (uint16_t)i; st->seen[i] = 1;
        while (sp) {
            int k = st->stack[--sp], r = k / MDL_COLS, c = k % MDL_COLS;
            st->cells[n++] = (uint16_t)k;
            sc += c; sr += r;
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++) {
                    if ((!dr && !dc) || (conn == 4 && dr && dc)) continue;
                    int rr = r + dr, cc = c + dc;
                    if (rr < 0 || rr >= rows || cc < 0 || cc >= MDL_COLS) continue;
                    int kk = rr * MDL_COLS + cc;
                    if (m[kk] && !st->seen[kk]) { st->seen[kk] = 1; st->stack[sp++] = (uint16_t)kk; }
                }
        }
        if (filter > 0 && n < filter)
            for (int j = 0; j < n; j++) m[st->cells[j]] = 0;
        if (bl && maxb > 0 && n >= min_keep) {              /* keep the largest, sorted */
            int pos;
            if (nb < maxb) pos = nb++;
            else if (bl[maxb - 1].n >= n) continue;
            else pos = maxb - 1;
            while (pos > 0 && bl[pos - 1].n < n) { bl[pos] = bl[pos - 1]; pos--; }
            bl[pos].n = n; bl[pos].sc = sc; bl[pos].sr = sr;
        }
    }
    return nb;
}

/* ---- tracking, line crossing, loitering ------------------------------------------ */
static int line_side(const int *L, int x, int y)
{
    int64_t dx = L[2] - L[0], dy = L[3] - L[1];
    int64_t cr = dx * (y - L[1]) - dy * (x - L[0]);
    if (cr * cr <= 15 * 15 * (dx * dx + dy * dy)) return 0;     /* within 1.5 % of the line: undecided */
    return cr < 0 ? 1 : 2;                                       /* 1: left of A->B (y grows downwards) */
}
static int sgn64(int64_t v) { return (v > 0) - (v < 0); }
static int orient(int ax, int ay, int bx, int by, int cx, int cy)
{
    return sgn64((int64_t)(bx - ax) * (cy - ay) - (int64_t)(by - ay) * (cx - ax));
}
static int seg_cross(const int *L, int px, int py, int qx, int qy)
{
    int o1 = orient(L[0], L[1], L[2], L[3], px, py), o2 = orient(L[0], L[1], L[2], L[3], qx, qy);
    int o3 = orient(px, py, qx, qy, L[0], L[1]), o4 = orient(px, py, qx, qy, L[2], L[3]);
    return o1 * o2 <= 0 && o3 * o4 <= 0;
}

static int in_loiter_area(const mdl_t *st, const mdl_track_t *t, const uint8_t *watch, int rows)
{
    int col = clampi(t->x * MDL_COLS / 1000, 0, MDL_COLS - 1);
    if (st->cfg.loiter_zone > 0) {
        const mdl_zone_t *z = &st->cfg.zone[st->cfg.loiter_zone - 1];
        int ur = clampi(t->y * MDL_ROWS_SW / 1000, 0, MDL_ROWS_SW - 1);
        return col >= z->c0 && col <= z->c1 && ur >= z->r0 && ur <= z->r1;
    }
    int r = clampi(t->y * rows / 1000, 0, rows - 1);
    return watch[r * MDL_COLS + col] != 0;
}

/* m: cells that changed (whole picture minus the overlay); conn: 4 or 8.
 * feed = 0 (quiet / light change): no matching, tracks are dropped. */
static void rules(mdl_t *st, uint8_t *m, int rows, int conn, int feed, const mdl_in_t *in, int64_t now, mdl_out_t *out)
{
    const mdl_cfg_t *c = &st->cfg;
    out->ncross = 0;
    int want = c->line_on || c->loiter_s > 0;
    if (!want || !feed) tracks_clear(st);
    if (want && feed) {
        blob_t bl[MDL_MAX_BLOBS];
        int nb = components(st, m, rows, conn, 0, c->min_size > MDL_TRACK_MIN_CELLS ? c->min_size : MDL_TRACK_MIN_CELLS, bl, MDL_MAX_BLOBS);
        int bx[MDL_MAX_BLOBS], by[MDL_MAX_BLOBS], bused[MDL_MAX_BLOBS] = { 0 }, tused[MDL_MAX_TRACKS] = { 0 };
        for (int b = 0; b < nb; b++) {
            bx[b] = (2 * bl[b].sc + bl[b].n) * 1000 / (2 * bl[b].n * MDL_COLS);
            by[b] = (2 * bl[b].sr + bl[b].n) * 1000 / (2 * bl[b].n * rows);
        }
        /* greedy nearest match inside the gate */
        const int64_t gate2 = (int64_t)MDL_GATE_CELLS * 1000 * MDL_GATE_CELLS * 1000;
        for (;;) {
            int bt = -1, bb = -1;
            int64_t best = gate2 + 1;
            for (int t = 0; t < MDL_MAX_TRACKS; t++) {
                if (!st->trk[t].id || tused[t]) continue;
                for (int b = 0; b < nb; b++) {
                    if (bused[b]) continue;
                    int64_t dx = (int64_t)(bx[b] - st->trk[t].x) * MDL_COLS, dy = (int64_t)(by[b] - st->trk[t].y) * rows;
                    int64_t d2 = dx * dx + dy * dy;
                    if (d2 < best) { best = d2; bt = t; bb = b; }
                }
            }
            if (bt < 0) break;
            tused[bt] = 1; bused[bb] = 1;
            mdl_track_t *t = &st->trk[bt];
            t->x = bx[bb]; t->y = by[bb]; t->last_ms = now;
            if (t->nseen < 1000000) t->nseen++;
        }
        /* unmatched tracks age out; unmatched blobs start tracks */
        for (int t = 0; t < MDL_MAX_TRACKS; t++)
            if (st->trk[t].id && !tused[t] && (now - st->trk[t].last_ms > MDL_TRACK_TTL_MS || now < st->trk[t].last_ms))
                st->trk[t].id = 0;
        for (int b = 0; b < nb; b++) {
            if (bused[b]) continue;
            for (int t = 0; t < MDL_MAX_TRACKS; t++) {
                if (st->trk[t].id) continue;
                mdl_track_t *n = &st->trk[t];
                memset(n, 0, sizeof *n);
                st->next_id = st->next_id >= 65535 ? 1 : st->next_id + 1;
                n->id = st->next_id;
                n->x = n->rx = bx[b]; n->y = n->ry = by[b];
                n->side = c->line_on ? line_side(c->line, n->x, n->y) : 0;
                n->nseen = 1; n->first_ms = n->last_ms = now;
                tused[t] = 1;                    /* seen this frame */
                break;
            }
        }
        /* rules on tracks seen this frame */
        for (int ti = 0; ti < MDL_MAX_TRACKS; ti++) {
            mdl_track_t *t = &st->trk[ti];
            if (!t->id || !tused[ti]) continue;
            int mature = t->nseen >= MDL_TRACK_MIN_SEEN && now - t->first_ms >= c->persist_ms;
            if (c->line_on) {
                int s = line_side(c->line, t->x, t->y);
                if (!t->side) { t->side = s; t->rx = t->x; t->ry = t->y; }
                else if (mature && s && s != t->side) {
                    if (seg_cross(c->line, t->rx, t->ry, t->x, t->y) && (!c->line_dir || c->line_dir == s)) {
                        out->cross[out->ncross].id = t->id;
                        out->cross[out->ncross].dir = s;
                        out->ncross++;
                        st->line_on = 1;
                        st->last_cross = now;
                    }
                    t->side = s; t->rx = t->x; t->ry = t->y;
                }
            }
            if (c->loiter_s > 0) {
                if (in_loiter_area(st, t, in->watch, rows)) {
                    if (!t->in_zone) { t->in_zone = 1; t->in_since = now; }
                    if (mature && now - t->in_since >= (int64_t)c->loiter_s * 1000) {
                        if (!st->loiter_on) st->loiter_id = t->id;
                        st->loiter_on = 1;
                        st->last_loiter = now;
                    }
                } else t->in_zone = 0;
            }
        }
    }
    int64_t hold = (int64_t)c->hold_s * 1000;
    if (st->line_on && (now - st->last_cross > hold || now < st->last_cross)) st->line_on = 0;
    if (st->loiter_on && (now - st->last_loiter > hold || now < st->last_loiter || !c->loiter_s)) st->loiter_on = 0;
    out->ntracks = 0;
    for (int t = 0; t < MDL_MAX_TRACKS; t++)
        if (st->trk[t].id) {
            out->track[out->ntracks].id = st->trk[t].id;
            out->track[out->ntracks].x = st->trk[t].x;
            out->track[out->ntracks].y = st->trk[t].y;
            out->ntracks++;
        }
}

static void out_states(const mdl_t *st, mdl_out_t *out)
{
    out->motion = st->motion;
    out->motion_any = st->motion_any;
    out->line = st->line_on;
    out->loiter = st->loiter_on;
    out->loiter_id = st->loiter_on ? st->loiter_id : 0;
}

/* ---- software source --------------------------------------------------------------- */
static int need_cells(int watched, int sens)            /* 6% .. 0.4% of the watched cells */
{
    int need = watched * (60 - (sens * 56) / 100) / 1000;
    return need < 1 ? 1 : need;
}

int mdl_sw_frame(mdl_t *st, const mdl_cfg_t *cfgin, const float *cur, const mdl_in_t *in, mdl_out_t *out)
{
    cfg_update(st, cfgin, MDL_ROWS_SW);
    const mdl_cfg_t *c = &st->cfg;
    int64_t now = clamp_time(in->now_ms);
    if (!st->have_bg) { for (int i = 0; i < NB_SW; i++) st->bg[i] = cur[i]; st->have_bg = 1; return 1; }

    /* sensitivity 1..100 -> per-block luma threshold 40..5, area 6%..0.4% of watched blocks */
    float thr = 40.0f - 0.35f * (float)c->sens, thrz[MDL_MAX_ZONES + 1];
    thrz[0] = thr;
    for (int k = 0; k < c->nzones; k++) thrz[k + 1] = 40.0f - 0.35f * (float)c->zone[k].sens;
    int changed_all = 0, npos = 0, nneg = 0;
    float sum_d = 0;
    for (int i = 0; i < NB_SW; i++) {
        float df = cur[i] - st->bg[i], d = df < 0 ? -df : df;
        int hit = d > thr;
        st->hit[i] = (uint8_t)hit;
        st->hitz[i] = (uint8_t)(c->nzones && st->zmap[i] ? d > thrz[st->zmap[i]] : hit);
        changed_all += hit;
        if (c->light_pct) { sum_d += df; npos += df >= 2.0f; nneg += df <= -2.0f; }
        st->bg[i] += (cur[i] - st->bg[i]) * (hit ? 0.02f : 0.08f);       /* adapt slowly where things move */
    }
    if (c->min_size) {
        components(st, st->hit, MDL_ROWS_SW, 4, c->min_size, 0, NULL, 0);
        if (c->nzones) components(st, st->hitz, MDL_ROWS_SW, 4, c->min_size, 0, NULL, 0);
    }
    const uint8_t *hz = c->nzones ? st->hitz : st->hit;
    int wg[MDL_MAX_ZONES + 1] = { 0 }, cg[MDL_MAX_ZONES + 1] = { 0 }, watched_any = 0, changed_any = 0, score = 0;
    for (int i = 0; i < NB_SW; i++) {
        int w = in->watch[i] != 0;
        if (in->watch_all[i]) { watched_any++; changed_any += st->hit[i]; }
        if (w) { int g = c->nzones ? st->zmap[i] : 0; wg[g]++; cg[g] += hz[i]; score += hz[i]; }
        out->active[i] = (uint8_t)(hz[i] && w);
    }
    out->score = score;
    out->hwmax = out->hwp90 = 0;
    out->light = 0;

    int light;
    if (!c->light_pct) light = changed_all > NB_SW * 6 / 10;               /* light switched: not motion */
    else {
        float mean = sum_d / (float)NB_SW;
        light = changed_all * 100 > c->light_pct * NB_SW ||
                ((mean >= 6.0f || mean <= -6.0f) && (mean > 0 ? npos : nneg) * 100 >= 75 * NB_SW);
    }
    if (light) {
        for (int i = 0; i < NB_SW; i++) st->bg[i] = cur[i];
        st->quiet_until = now + (c->light_pct ? MDL_LIGHT_QUIET_MS : 2000);
        st->run = 0;
        if (c->light_pct) st->run_any = 0;
        out->light = 1;
        rules(st, st->hit, MDL_ROWS_SW, 4, 0, in, now, out);
        out_states(st, out);
        return 0;
    }
    int cand = 0;
    if (now >= st->quiet_until)
        for (int g = 0; g <= c->nzones; g++)
            if (wg[g] && cg[g] >= need_cells(wg[g], g ? c->zone[g - 1].sens : c->sens)) cand = 1;
    if (cand) {
        if (st->run < 1000000) st->run++;
        if (st->run == 1) st->run_t0 = now;
        if (st->run >= 2 && (!c->persist_ms || now - st->run_t0 >= c->persist_ms)) { st->last_hit = now; st->motion = 1; }
    } else st->run = 0;
    if (st->motion && now - st->last_hit > (int64_t)c->hold_s * 1000) st->motion = 0;
    /* whole picture, same rules (only wakes the frame rate) */
    if (now >= st->quiet_until && changed_any >= need_cells(watched_any, c->sens)) {
        if (st->run_any < 1000000) st->run_any++;
        if (st->run_any == 1) st->run_any_t0 = now;
        if (st->run_any >= 2 && (!c->persist_ms || now - st->run_any_t0 >= c->persist_ms)) { st->last_any = now; st->motion_any = 1; }
    } else st->run_any = 0;
    if (st->motion_any && now - st->last_any > (int64_t)c->hold_s * 1000) st->motion_any = 0;

    if (c->line_on || c->loiter_s) {
        for (int i = 0; i < NB_SW; i++) st->act_all[i] = (uint8_t)(st->hit[i] && in->watch_all[i]);
        rules(st, st->act_all, MDL_ROWS_SW, 4, now >= st->quiet_until, in, now, out);
    } else rules(st, st->act_all, MDL_ROWS_SW, 4, 0, in, now, out);
    out_states(st, out);
    return 0;
}

/* ---- hardware source ------------------------------------------------------------------
 * ISP 3D-NR statistic: u16[24][32] per-block motion values. Cells >= hi seed regions
 * grown over cells >= lo (8-connected); a region counts when its bounding box is
 * between min_cells(sensitivity) and 300 cells and at least min_cells of its cells
 * are in the watched zones (per zone with zone sensitivities). Stock uses fixed
 * 10000/5000; measured on this camera the still-scene floor is p90 ~500 (day) /
 * ~200 (night IR) with single cells up to ~1700, and a person gives 10-20 cells
 * >= 1000, peaks 3000-11000. So the thresholds follow the noise:
 * hi = max(1800, 3.5 p90), lo = hi / 2. */
static int hw_detect(mdl_t *st, const uint16_t *g, const uint8_t *watch, int zones, uint8_t *act, int hi, int lo)
{
    const mdl_cfg_t *c = &st->cfg;
    int ng = zones ? c->nzones : 0, minc[MDL_MAX_ZONES + 1];
    minc[0] = k_min_cells[c->sens];
    for (int k = 0; k < ng; k++) minc[k + 1] = k_min_cells[c->zone[k].sens];
    memset(st->seen, 0, MDL_MAX_CELLS);
    memset(act, 0, MDL_MAX_CELLS);
    int found = 0;
    for (int i = 0; i < MDL_MAX_CELLS; i++) {
        if (st->seen[i] || g[i] < hi) continue;
        int sp = 0, r0 = 99, r1 = -1, c0 = 99, c1 = -1, n = 0, wg[MDL_MAX_ZONES + 1] = { 0 };
        st->stack[sp++] = (uint16_t)i; st->seen[i] = 1;
        while (sp) {
            int k = st->stack[--sp], r = k / MDL_COLS, cc = k % MDL_COLS;
            st->cells[n++] = (uint16_t)k;
            if (r < r0) r0 = r;
            if (r > r1) r1 = r;
            if (cc < c0) c0 = cc;
            if (cc > c1) c1 = cc;
            if (watch[k]) wg[ng ? st->zmap[k] : 0]++;
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++) {
                    int rr = r + dr, c2 = cc + dc;
                    if (rr < 0 || rr >= MDL_ROWS_HW || c2 < 0 || c2 >= MDL_COLS) continue;
                    int kk = rr * MDL_COLS + c2;
                    if (!st->seen[kk] && g[kk] >= lo) { st->seen[kk] = 1; st->stack[sp++] = (uint16_t)kk; }
                }
        }
        int area = (r1 - r0 + 1) * (c1 - c0 + 1), ok = 0;
        if (area <= 300 && (!c->min_size || n >= c->min_size))
            for (int k = 0; k <= ng; k++)
                if (area >= minc[k] && wg[k] >= minc[k]) ok = 1;
        if (ok) {
            found = 1;
            for (int j = 0; j < n; j++) act[st->cells[j]] = 1;
        }
    }
    return found;
}

void mdl_hw_frame(mdl_t *st, const mdl_cfg_t *cfgin, const uint16_t *g, const mdl_in_t *in, mdl_out_t *out)
{
    cfg_update(st, cfgin, MDL_ROWS_HW);
    const mdl_cfg_t *c = &st->cfg;
    int64_t t = clamp_time(in->now_ms);
    int mx = 0;
    for (int i = 0; i < MDL_MAX_CELLS; i++) if (g[i] > mx) mx = g[i];
    out->hwmax = mx;
    /* noise floor: 90th percentile (counting sort on a coarse scale) */
    uint16_t hcnt[256];
    memset(hcnt, 0, sizeof hcnt);
    for (int i = 0; i < MDL_MAX_CELLS; i++) hcnt[g[i] >= 65280 ? 255 : g[i] >> 8]++;
    int acc = 0, p50 = -1, p90 = 0;
    for (int b = 0; b < 256; b++) {
        acc += hcnt[b];
        if (p50 < 0 && acc >= MDL_MAX_CELLS / 2) p50 = b * 256 + 128;
        if (acc >= MDL_MAX_CELLS * 9 / 10) { p90 = b * 256 + 128; break; }
    }
    int hi = p90 * 7 / 2;
    if (hi < 1800) hi = 1800;
    int lo = hi / 2;
    out->hwp90 = p90;
    /* most of the picture changed at once (lights, IR switch, AE jump): not
     * motion. The median follows the light level (~100 bright IR, ~300 lit
     * room, ~1000 dark room), so compare it with a slow baseline; a jump
     * that lasts over 5 s becomes the new baseline. */
    if (st->base50 < 0) st->base50 = p50;
    if (st->base90 < 0) st->base90 = p90;
    int global = p50 > st->base50 * 3 + 400;
    if (c->light_pct && !global) {          /* option: many cells above the quiet-scene threshold */
        int th = st->base90 * 7 / 2, n = 0;
        if (th < 1800) th = 1800;
        for (int i = 0; i < MDL_MAX_CELLS; i++) n += g[i] >= th;
        global = n * 100 >= c->light_pct * MDL_MAX_CELLS;
    }
    if (global) { if (++st->glob_n > 50) { st->base50 = p50; st->base90 = p90; st->glob_n = 0; } }
    else { st->glob_n = 0; st->base50 += (p50 - st->base50) / 32; st->base90 += (p90 - st->base90) / 32; }
    out->light = global;
    if (global && c->light_pct) {
        int64_t q = t + MDL_LIGHT_QUIET_MS;
        if (q > st->quiet_until) st->quiet_until = q;
    }

    int hit = 0;
    if (global) memset(out->active, 0, MDL_MAX_CELLS);
    else hit = hw_detect(st, g, in->watch, 1, out->active, hi, lo);
    int score = 0;
    for (int i = 0; i < MDL_MAX_CELLS; i++) score += out->active[i];
    out->score = score;
    st->hist = ((st->hist << 1) | hit) & 7;
    int ok = in->night ? __builtin_popcount((unsigned)st->hist) >= 2 : hit;
    if (ok && !st->ok_prev) st->ok_t0 = t;
    st->ok_prev = ok;
    if (ok && c->persist_ms && t - st->ok_t0 < c->persist_ms) ok = 0;
    if (t >= st->quiet_until && ok) { st->last_hit = t; st->motion = 1; }
    if (st->motion && t - st->last_hit > (int64_t)c->hold_s * 1000) st->motion = 0;
    /* whole picture, same rules (only wakes the frame rate) */
    int hit_all = 0;
    if (global) memset(st->act_all, 0, MDL_MAX_CELLS);
    else hit_all = hw_detect(st, g, in->watch_all, 0, st->act_all, hi, lo);
    st->hist_all = ((st->hist_all << 1) | hit_all) & 7;
    int ok_all = in->night ? __builtin_popcount((unsigned)st->hist_all) >= 2 : hit_all;
    if (ok_all && !st->ok_any_prev) st->ok_any_t0 = t;
    st->ok_any_prev = ok_all;
    if (ok_all && c->persist_ms && t - st->ok_any_t0 < c->persist_ms) ok_all = 0;
    if (t >= st->quiet_until && ok_all) { st->last_any = t; st->motion_any = 1; }
    if (st->motion_any && t - st->last_any > (int64_t)c->hold_s * 1000) st->motion_any = 0;

    rules(st, st->act_all, MDL_ROWS_HW, 8, !global && t >= st->quiet_until, in, t, out);
    out_states(st, out);
}

/* ---- settings text ------------------------------------------------------------------------ */
static int read_num(const char **p, int lo, int hi, int *v)
{
    const char *s = *p;
    int x = 0, n = 0;
    while (*s >= '0' && *s <= '9') { if (++n > 4) return -1; x = x * 10 + (*s++ - '0'); }
    if (!n || x < lo || x > hi) return -1;
    *v = x; *p = s;
    return 0;
}

int mdl_parse_zones(const char *v, mdl_zone_t *z, int max)
{
    int n = 0;
    if (!*v) return 0;
    for (;;) {
        mdl_zone_t t;
        if (n >= max) return -1;
        if (read_num(&v, 0, MDL_COLS - 1, &t.c0) || *v++ != ',' || read_num(&v, 0, MDL_ROWS_SW - 1, &t.r0) || *v++ != ',' ||
            read_num(&v, 0, MDL_COLS - 1, &t.c1) || *v++ != ',' || read_num(&v, 0, MDL_ROWS_SW - 1, &t.r1) || *v++ != ',' ||
            read_num(&v, 1, 100, &t.sens))
            return -1;
        if (t.c0 > t.c1 || t.r0 > t.r1) return -1;
        if (z) z[n] = t;
        n++;
        if (!*v) return n;
        if (*v++ != ';') return -1;
    }
}

int mdl_parse_line(const char *v, int line[4])
{
    int l[4];
    if (!*v) return 0;
    for (int i = 0; i < 4; i++) {
        if (i && *v++ != ',') return -1;
        if (read_num(&v, 0, 1000, &l[i])) return -1;
    }
    if (*v) return -1;
    int dx = l[2] - l[0], dy = l[3] - l[1];
    if (dx * dx + dy * dy < 400) return -1;
    if (line) memcpy(line, l, sizeof l);
    return 1;
}
