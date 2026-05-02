#include "robot_face.h"
#include "lvgl.h"
#include <stdlib.h>

/* Display: 320x240 landscape (90-degree rotation) */
#define SCREEN_W    320
#define SCREEN_H    240

/* ── geometry ───────────────────────────────────── */

/* Eyes – large warm-white circles */
#define EYE_W       64
#define EYE_H       64
#define EYE_R       32
#define EYE_L_X     60
#define EYE_R_X     196
#define EYE_Y       74
#define EYE_CX_L    (EYE_L_X + EYE_W / 2)   /* 92  */
#define EYE_CX_R    (EYE_R_X + EYE_W / 2)   /* 228 */
#define EYE_CY      (EYE_Y   + EYE_H / 2)   /* 106 */

/* Pupils – bright blue */
#define PUPIL_W     28
#define PUPIL_H     28
#define PUPIL_R     14

/* Sparkle highlight dot inside each pupil */
#define HI_W        9
#define HI_H        9
#define HI_R        4
#define HI_OFS_X    6    /* offset from pupil centre rightward  */
#define HI_OFS_Y   -10  /* offset from pupil centre upward     */

/* Eyebrows – 3-point arched lines above eyes */
#define BROW_LINE_W 4

/* Cheeks – rosy pink ovals below/beside eyes */
#define CHEEK_W     44
#define CHEEK_H     28
#define CHEEK_R     14
#define CHEEK_L_X   (EYE_CX_L - 52)                 /* 40  */
#define CHEEK_R_X   (EYE_CX_R + 52 - CHEEK_W)       /* 236 */
#define CHEEK_Y     148

/* Smile line (idle) – 9-point parabola */
/* Mouth rect (speaking) – animated oval */
#define MOUTH_W             82
#define MOUTH_H_SPEAK_MIN   10
#define MOUTH_H_SPEAK_MAX   36
#define MOUTH_R             12
#define MOUTH_CX            160
#define MOUTH_CY            195

/* ── timing ─────────────────────────────────────── */
#define ANIM_PERIOD_MS      50
#define BLINK_INTERVAL      60   /* ~3 s between blinks  */
#define BLINK_CLOSED_TICKS  3    /* 150 ms closed        */
#define PUPIL_MOVE_INTERVAL 20   /* ~1 s between wanders */
#define MOUTH_PHASE_TICKS   4    /* 200 ms per open/close */

/* ── LVGL objects ───────────────────────────────── */
static lv_obj_t *s_scr        = NULL;
static lv_obj_t *s_lbl_text   = NULL;
static lv_obj_t *s_eye_l      = NULL;
static lv_obj_t *s_eye_r      = NULL;
static lv_obj_t *s_pupil_l    = NULL;
static lv_obj_t *s_pupil_r    = NULL;
static lv_obj_t *s_hi_l       = NULL;   /* sparkle highlight left  */
static lv_obj_t *s_hi_r       = NULL;   /* sparkle highlight right */
static lv_obj_t *s_brow_l     = NULL;
static lv_obj_t *s_brow_r     = NULL;
static lv_obj_t *s_cheek_l    = NULL;
static lv_obj_t *s_cheek_r    = NULL;
static lv_obj_t *s_smile      = NULL;   /* idle smile polyline      */
static lv_obj_t *s_mouth_rect = NULL;   /* speaking animated mouth  */
static lv_timer_t *s_timer    = NULL;

/* lv_line keeps a POINTER to the points array – must be static/global */
static const lv_point_precise_t s_brow_l_pts[] = {
    {58, 66}, {92, 58}, {122, 64}
};
static const lv_point_precise_t s_brow_r_pts[] = {
    {198, 64}, {228, 58}, {262, 66}
};
/* Parabola: y = -0.006*(x-160)^2 + 201, through (110,185)..(160,201)..(210,185) */
static const lv_point_precise_t s_smile_pts[] = {
    {110, 185}, {122, 192}, {136, 197}, {150, 200},
    {160, 201}, {170, 200}, {184, 197}, {198, 192}, {210, 185}
};

/* ── animation state ────────────────────────────── */
static bool s_speaking      = false;
static bool s_sleeping      = false;

static int  s_blink_counter = 0;
static bool s_blinking      = false;
static int  s_blink_closed  = 0;

static int  s_pupil_counter = 0;
static int  s_px_l = 0, s_py_l = 0;
static int  s_px_r = 0, s_py_r = 0;

static int  s_mouth_counter = 0;
static bool s_mouth_opening = true;
static int  s_mouth_h       = MOUTH_H_SPEAK_MIN;

/* ── helpers ────────────────────────────────────── */

static lv_obj_t *make_filled(lv_obj_t *parent,
                              int x, int y, int w, int h, int radius,
                              lv_color_t color, lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_bg_color(obj, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, opa, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    return obj;
}

static lv_obj_t *make_line(lv_obj_t *parent,
                            const lv_point_precise_t *pts, uint32_t cnt,
                            lv_color_t color, int width)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_obj_set_pos(line, 0, 0);   /* points are in screen coords */
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
    lv_line_set_points(line, pts, cnt);
    lv_obj_set_style_line_color(line, color, LV_PART_MAIN);
    lv_obj_set_style_line_width(line, width, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
    return line;
}

/* ── animation timer ────────────────────────────── */

static void face_anim_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_scr) return;

    /* ---- sleep mode: half-closed, no blink ---- */
    if (s_sleeping) {
        lv_obj_set_height(s_eye_l, EYE_H / 2);
        lv_obj_set_y(s_eye_l, EYE_CY);
        lv_obj_set_height(s_eye_r, EYE_H / 2);
        lv_obj_set_y(s_eye_r, EYE_CY);
        lv_obj_add_flag(s_pupil_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pupil_r, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_hi_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_hi_r, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* ---- blink ---- */
    s_blink_counter++;
    if (s_blink_counter >= BLINK_INTERVAL && !s_blinking) {
        s_blinking      = true;
        s_blink_closed  = 0;
        s_blink_counter = 0;
    }

    if (s_blinking) {
        lv_obj_set_height(s_eye_l, 4);
        lv_obj_set_y(s_eye_l, EYE_CY - 2);
        lv_obj_set_height(s_eye_r, 4);
        lv_obj_set_y(s_eye_r, EYE_CY - 2);
        lv_obj_add_flag(s_pupil_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pupil_r, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_hi_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_hi_r, LV_OBJ_FLAG_HIDDEN);

        s_blink_closed++;
        if (s_blink_closed >= BLINK_CLOSED_TICKS) {
            s_blinking = false;
            lv_obj_set_height(s_eye_l, EYE_H);
            lv_obj_set_y(s_eye_l, EYE_Y);
            lv_obj_set_height(s_eye_r, EYE_H);
            lv_obj_set_y(s_eye_r, EYE_Y);
            lv_obj_clear_flag(s_pupil_l, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_pupil_r, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_hi_l, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_hi_r, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_pupil_l,
                EYE_CX_L - PUPIL_W / 2 + s_px_l,
                EYE_CY   - PUPIL_H / 2 + s_py_l);
            lv_obj_set_pos(s_pupil_r,
                EYE_CX_R - PUPIL_W / 2 + s_px_r,
                EYE_CY   - PUPIL_H / 2 + s_py_r);
            lv_obj_set_pos(s_hi_l,
                EYE_CX_L + HI_OFS_X + s_px_l,
                EYE_CY   + HI_OFS_Y + s_py_l);
            lv_obj_set_pos(s_hi_r,
                EYE_CX_R + HI_OFS_X + s_px_r,
                EYE_CY   + HI_OFS_Y + s_py_r);
        }
        return;
    }

    /* ---- pupil wander ---- */
    s_pupil_counter++;
    if (s_pupil_counter >= PUPIL_MOVE_INTERVAL) {
        s_pupil_counter = 0;
        s_px_l = (rand() % 15) - 7;
        s_py_l = (rand() % 9)  - 4;
        s_px_r = (rand() % 15) - 7;
        s_py_r = (rand() % 9)  - 4;
        lv_obj_set_pos(s_pupil_l,
            EYE_CX_L - PUPIL_W / 2 + s_px_l,
            EYE_CY   - PUPIL_H / 2 + s_py_l);
        lv_obj_set_pos(s_pupil_r,
            EYE_CX_R - PUPIL_W / 2 + s_px_r,
            EYE_CY   - PUPIL_H / 2 + s_py_r);
        /* sparkle follows the pupil */
        lv_obj_set_pos(s_hi_l,
            EYE_CX_L + HI_OFS_X + s_px_l,
            EYE_CY   + HI_OFS_Y + s_py_l);
        lv_obj_set_pos(s_hi_r,
            EYE_CX_R + HI_OFS_X + s_px_r,
            EYE_CY   + HI_OFS_Y + s_py_r);
    }

    /* ---- speaking mouth animation ---- */
    if (s_speaking) {
        s_mouth_counter++;
        if (s_mouth_counter >= MOUTH_PHASE_TICKS) {
            s_mouth_counter = 0;
            if (s_mouth_opening) {
                s_mouth_h += 8;
                if (s_mouth_h >= MOUTH_H_SPEAK_MAX) {
                    s_mouth_h       = MOUTH_H_SPEAK_MAX;
                    s_mouth_opening = false;
                }
            } else {
                s_mouth_h -= 8;
                if (s_mouth_h <= MOUTH_H_SPEAK_MIN) {
                    s_mouth_h       = MOUTH_H_SPEAK_MIN;
                    s_mouth_opening = true;
                }
            }
            lv_obj_set_height(s_mouth_rect, s_mouth_h);
            lv_obj_set_y(s_mouth_rect, MOUTH_CY - s_mouth_h / 2);
        }
    }
}

/* ── public API ─────────────────────────────────── */

void robot_face_create(void)
{
    s_scr = lv_screen_active();

    /* Soft dark-blue background */
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0a0a14), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- text label (soft blue-white) ---- */
    s_lbl_text = lv_label_create(s_scr);
    lv_label_set_text(s_lbl_text, "...");
    lv_obj_set_style_text_color(s_lbl_text, lv_color_hex(0xDDE8FF), LV_PART_MAIN);
    lv_obj_align(s_lbl_text, LV_ALIGN_TOP_MID, 0, 10);

    /* ---- eyebrows: gentle arched white lines ---- */
    s_brow_l = make_line(s_scr, s_brow_l_pts, 3, lv_color_white(), BROW_LINE_W);
    s_brow_r = make_line(s_scr, s_brow_r_pts, 3, lv_color_white(), BROW_LINE_W);

    /* ---- eyes: large warm-white circles ---- */
    s_eye_l = make_filled(s_scr,
        EYE_L_X, EYE_Y, EYE_W, EYE_H, EYE_R,
        lv_color_hex(0xF5F5FF), LV_OPA_COVER);

    s_eye_r = make_filled(s_scr,
        EYE_R_X, EYE_Y, EYE_W, EYE_H, EYE_R,
        lv_color_hex(0xF5F5FF), LV_OPA_COVER);

    /* ---- pupils: bright friendly blue ---- */
    s_pupil_l = make_filled(s_scr,
        EYE_CX_L - PUPIL_W / 2, EYE_CY - PUPIL_H / 2,
        PUPIL_W, PUPIL_H, PUPIL_R,
        lv_color_hex(0x1A8AFF), LV_OPA_COVER);

    s_pupil_r = make_filled(s_scr,
        EYE_CX_R - PUPIL_W / 2, EYE_CY - PUPIL_H / 2,
        PUPIL_W, PUPIL_H, PUPIL_R,
        lv_color_hex(0x1A8AFF), LV_OPA_COVER);

    /* ---- sparkle highlights inside pupils ---- */
    s_hi_l = make_filled(s_scr,
        EYE_CX_L + HI_OFS_X, EYE_CY + HI_OFS_Y, HI_W, HI_H, HI_R,
        lv_color_white(), LV_OPA_COVER);

    s_hi_r = make_filled(s_scr,
        EYE_CX_R + HI_OFS_X, EYE_CY + HI_OFS_Y, HI_W, HI_H, HI_R,
        lv_color_white(), LV_OPA_COVER);

    /* ---- cheeks: rosy pink, semi-transparent ---- */
    s_cheek_l = make_filled(s_scr,
        CHEEK_L_X, CHEEK_Y, CHEEK_W, CHEEK_H, CHEEK_R,
        lv_color_hex(0xFF6B9E), LV_OPA_50);

    s_cheek_r = make_filled(s_scr,
        CHEEK_R_X, CHEEK_Y, CHEEK_W, CHEEK_H, CHEEK_R,
        lv_color_hex(0xFF6B9E), LV_OPA_50);

    /* ---- idle smile: curved polyline ---- */
    s_smile = make_line(s_scr, s_smile_pts, 9, lv_color_white(), 4);

    /* ---- speaking mouth: animated oval (hidden at start) ---- */
    s_mouth_rect = lv_obj_create(s_scr);
    lv_obj_set_size(s_mouth_rect, MOUTH_W, MOUTH_H_SPEAK_MIN);
    lv_obj_set_pos(s_mouth_rect,
        MOUTH_CX - MOUTH_W / 2,
        MOUTH_CY - MOUTH_H_SPEAK_MIN / 2);
    lv_obj_set_style_bg_color(s_mouth_rect, lv_color_hex(0x1a0010), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_mouth_rect, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_mouth_rect, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_mouth_rect, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(s_mouth_rect, MOUTH_R, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_mouth_rect, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_mouth_rect, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_mouth_rect, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_mouth_rect, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_mouth_rect, LV_OBJ_FLAG_HIDDEN);

    /* ---- start animation timer ---- */
    s_timer = lv_timer_create(face_anim_cb, ANIM_PERIOD_MS, NULL);
}

void robot_face_set_text(const char *text)
{
    if (s_lbl_text && text) {
        lv_label_set_text(s_lbl_text, text);
        lv_obj_align(s_lbl_text, LV_ALIGN_TOP_MID, 0, 10);
    }
}

void robot_face_set_speaking(bool speaking)
{
    s_speaking = speaking;
    if (speaking) {
        if (s_smile)      lv_obj_add_flag(s_smile, LV_OBJ_FLAG_HIDDEN);
        if (s_mouth_rect) lv_obj_clear_flag(s_mouth_rect, LV_OBJ_FLAG_HIDDEN);
        s_mouth_h       = MOUTH_H_SPEAK_MIN;
        s_mouth_opening = true;
        s_mouth_counter = 0;
    } else {
        if (s_mouth_rect) lv_obj_add_flag(s_mouth_rect, LV_OBJ_FLAG_HIDDEN);
        if (s_smile)      lv_obj_clear_flag(s_smile, LV_OBJ_FLAG_HIDDEN);
    }
}

void robot_face_set_sleep(bool sleep_mode)
{
    s_sleeping = sleep_mode;
    if (!sleep_mode && s_eye_l && s_eye_r) {
        lv_obj_set_height(s_eye_l, EYE_H);
        lv_obj_set_y(s_eye_l, EYE_Y);
        lv_obj_set_height(s_eye_r, EYE_H);
        lv_obj_set_y(s_eye_r, EYE_Y);
        lv_obj_clear_flag(s_pupil_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_pupil_r, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_hi_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_hi_r, LV_OBJ_FLAG_HIDDEN);
    }
}
