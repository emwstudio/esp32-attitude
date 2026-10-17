#include "ui_ahrs.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "bsp/esp-bsp.h"

#define SCR_W 480
#define SCR_H 480
#define SCR_CX 240
#define SCR_CY 240

/* Rotating horizon card */
#define CARD_W 960
#define CARD_H 960
#define CARD_CX 480
#define CARD_CY 480
#define PX_PER_DEG 5.0f

#define COL_SKY    0x1FA9D8
#define COL_GROUND 0xBF5A06  /* 大地橙（暗一档，不炸眼） */
#define COL_LINE   0x39FF14   /* 地平线/俯仰梯统一荧光绿 */
#define COL_TAPE   0x39FF14   /* 参考同款荧光翠绿 */

static const char *TAG = "ui";

static lv_obj_t *s_horizon_img;
static lv_image_dsc_t s_horizon_dsc;
static lv_image_dsc_t dst_dsc;
static uint8_t *s_horizon_dst;   /* 480x480 RGB565 自研旋转渲染目标 */
const uint8_t *ui_card_ptr;      /* 地平线卡片源（供 render_horizon） */

/* tapes: pre-rendered strip images, scrolled via offset */
static lv_obj_t *s_img_left, *s_img_right;
static lv_image_dsc_t s_dsc_left, s_dsc_right;
static int s_strip_h_left, s_strip_h_right;
#define TAPE_W 120
#define TAPE_WIN_H 240
#define TAPE_WIN_Y 120
#define SPD_MAX 160.0f     /* kt（节） */
#define SPD_PX  4.0f
#define ALT_MAX 4000.0f    /* ft（英尺）；演示高度范围内够用，省 PSRAM */
#define ALT_PX  0.8f

/* heading ring */
static lv_obj_t *s_ring_img;
static lv_image_dsc_t s_ring_dsc;
#define RING_SIZE 480
#define RING_CX 240
#define RING_CY 240
#define RING_TICK_OUT 207
#define RING_POS_Y 360   /* 环中心在屏幕 (240, 600)，底部露出倒扣半月 */

static lv_obj_t *s_lbl_demo, *s_heartbeat, *s_voice_dot;
static lv_obj_t *s_hdg_lbls[12];
static lv_image_dsc_t s_hdg_dscs[12];
#define HDG_LBL_BOX_W 62
#define HDG_LBL_BOX_H 30
static int seg_draw_text(lv_layer_t *layer, const char *text, int x, int y, int dw, int dh, int t,
                         lv_color_t color);
static int seg_text_width(const char *text, int dw);
static const char *s_cur_seg_text;
#define SEG_TAPE_W 10
#define SEG_TAPE_H 20
#define SEG_TAPE_T 3

static void draw_hdg_seg_label(lv_layer_t *layer)
{
    int tw = seg_text_width(s_cur_seg_text, SEG_TAPE_W);
    seg_draw_text(layer, s_cur_seg_text, (HDG_LBL_BOX_W - tw) / 2, (HDG_LBL_BOX_H - SEG_TAPE_H) / 2,
                  SEG_TAPE_W, SEG_TAPE_H, SEG_TAPE_T, lv_color_hex(COL_TAPE));
}
#define HDG_LBL_R 175.0f
static const int s_hdg_degs[12] = {0, 30, 60, 90, 120, 150, 180, 210, 240, 270, 300, 330};
static const char *const s_hdg_texts[12] = {"N","3","6","E","12","15","S","21","24","W","30","33"};

static int s_q_roll = 9999, s_q_pitch = 9999;
static int s_q_speed = -1, s_q_alt = -1, s_q_hdg = -1;

/* LVGL 异步绘制只记录字符串指针，必须用存活得足够久的 buffer（环形池） */
#define TXT_POOL_SIZE 128
static char s_txt_pool[TXT_POOL_SIZE][12];
static int s_txt_idx;

static char *pool_txt(void)
{
    return s_txt_pool[s_txt_idx++ % TXT_POOL_SIZE];
}

static void draw_line(lv_layer_t *layer, int x1, int y1, int x2, int y2,
                      lv_color_t color, int width)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.round_end = 1;
    dsc.p1.x = x1; dsc.p1.y = y1;
    dsc.p2.x = x2; dsc.p2.y = y2;
    lv_draw_line(layer, &dsc);
}

/* 初始化期一次性画布绘制（此时任务未启动，派发安全） */
static void canvas_draw_once(uint8_t *buf, int w, int h, lv_color_format_t cf,
                             void (*draw_fn)(lv_layer_t *))
{
    lv_obj_t *tmp = lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(tmp, buf, w, h, cf);
    lv_layer_t layer;
    lv_canvas_init_layer(tmp, &layer);
    draw_fn(&layer);
    lv_canvas_finish_layer(tmp, &layer);
    lv_obj_delete(tmp);
}

/* ---------------- 自研地平线旋转渲染（绕开 LVGL 大图 transform 的 90° bug） ---------------- */

#define HORIZON_VIEW 480

static void render_horizon(const uint8_t *card, float roll_deg, int pitch_px)
{
    const uint16_t *src_img = (const uint16_t *)card;
    uint16_t *dst = (uint16_t *)s_horizon_dst;

    float rad = roll_deg * (float)M_PI / 180.0f;
    int32_t cs = (int32_t)(cosf(rad) * 65536.0f);
    int32_t sn = (int32_t)(sinf(rad) * 65536.0f);

    for (int y = 0; y < HORIZON_VIEW; y++) {
        int dy = y - 240;
        int32_t base_x = (-240) * cs - dy * sn;
        int32_t base_y = (-240) * sn + dy * cs;
        uint16_t *row = dst + y * HORIZON_VIEW;
        for (int x = 0; x < HORIZON_VIEW; x++) {
            /* +32768 四舍五入：>>16 是向下取整，在旋转中心（采样坐标过零点）会产生 1px 台阶 */
            int sx = 480 + (int)((base_x + x * cs + 32768) >> 16);
            int sy = 480 + pitch_px + (int)((base_y + x * sn + 32768) >> 16);
            /* 横滚 45° + 极限俯仰时采样角点会超出卡片，钳制防越界读 */
            if (sx < 0) sx = 0; else if (sx >= CARD_W) sx = CARD_W - 1;
            if (sy < 0) sy = 0; else if (sy >= CARD_H) sy = CARD_H - 1;
            row[x] = src_img[sy * CARD_W + sx];
        }
    }
}

/* ---------------- horizon card ---------------- */

static void draw_horizon(lv_layer_t *layer)
{
    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.border_width = 0;

    fill.bg_color = lv_color_hex(COL_SKY);
    fill.bg_opa = LV_OPA_COVER;
    lv_area_t sky = {0, 0, CARD_W - 1, CARD_CY};
    lv_draw_rect(layer, &fill, &sky);

    fill.bg_color = lv_color_hex(COL_GROUND);
    lv_area_t gnd = {0, CARD_CY, CARD_W - 1, CARD_H - 1};
    lv_draw_rect(layer, &fill, &gnd);

    draw_line(layer, 0, CARD_CY, CARD_W, CARD_CY, lv_color_hex(COL_LINE), 3);

    /* 俯仰梯（真机标准：直横线；地平线上=实线、下=虚线；±10/±20 两端标数字，其余短刻度；
       所有线中央断开 ±48px 避开飞机符号——波音 PFD 同款缺口） */
    for (int p = 5; p <= 20; p += 5) {
        bool labeled = (p == 10 || p == 20);
        int half = labeled ? 76 : 62;
        int gap = 48;
        for (int sign = 1; sign >= -1; sign -= 2) {
            int y = CARD_CY - sign * (int)(p * PX_PER_DEG);
            int segs[2][2] = {{CARD_CX - half, CARD_CX - gap}, {CARD_CX + gap, CARD_CX + half}};
            for (int s = 0; s < 2; s++) {
                int x1 = segs[s][0], x2 = segs[s][1];
                if (x1 >= x2) continue;
                if (sign > 0) {
                    draw_line(layer, x1, y, x2, y, lv_color_hex(COL_LINE), 2);
                } else {
                    /* 地平线下：虚线（8px 实 + 6px 空） */
                    for (int x = x1; x < x2; x += 14) {
                        int xe = x + 8 < x2 ? x + 8 : x2;
                        draw_line(layer, x, y, xe, y, lv_color_hex(COL_LINE), 2);
                    }
                }
            }
            if (labeled) {
                char *t = pool_txt();
                snprintf(t, 12, "%d", p);
                int tw = seg_text_width(t, 10);
                seg_draw_text(layer, t, CARD_CX - half - 6 - tw, y - 10, 10, 20, 3, lv_color_hex(COL_LINE));
                seg_draw_text(layer, t, CARD_CX + half + 6, y - 10, 10, 20, 3, lv_color_hex(COL_LINE));
            }
        }
    }

    /* 横滚刻度已拆为独立固定层（见 draw_roll_scale），不再画在卡片上 */
}

/* ---------------- tape strips ---------------- */

typedef struct {
    float min_val, max_val, px_per_unit, minor, major;
    int height;
    bool labels_left;
} strip_cfg_t;

static strip_cfg_t s_cur_strip;

static int strip_y(float v)
{
    return s_cur_strip.height - TAPE_WIN_H / 2 - (int)((v - s_cur_strip.min_val) * s_cur_strip.px_per_unit);
}

static void draw_strip(lv_layer_t *layer)
{
    const strip_cfg_t *c = &s_cur_strip;

    for (int v = (int)c->min_val; v <= (int)c->max_val; v += (int)c->minor) {
        int y = strip_y(v);
        bool major = (v % (int)c->major) == 0;
        int tick_len = major ? 17 : 11;
        int tx1 = c->labels_left ? TAPE_W - 25 - tick_len : 25;
        draw_line(layer, tx1, y, tx1 + tick_len, y, lv_color_hex(COL_TAPE), major ? 6 : 3);
        if (major) {
            char t[8];
            snprintf(t, sizeof(t), "%d", v);
            int tw = seg_text_width(t, SEG_TAPE_W);
            int ty = y - SEG_TAPE_H / 2;
            if (c->labels_left) {
                seg_draw_text(layer, t, tx1 - 8 - tw, ty, SEG_TAPE_W, SEG_TAPE_H, SEG_TAPE_T,
                              lv_color_hex(COL_TAPE));
            } else {
                seg_draw_text(layer, t, tx1 + tick_len + 8, ty, SEG_TAPE_W, SEG_TAPE_H, SEG_TAPE_T,
                              lv_color_hex(COL_TAPE));
            }
        }
    }
}

/* 单位标签与罗盘字母：B612（空客驾驶舱字体，Bold 自带粗体） */
LV_FONT_DECLARE(ui_font_b612_24);
LV_FONT_DECLARE(ui_font_b612_28);

static esp_err_t create_strip(lv_obj_t *parent, int x, float min_val, float max_val, float px_per_unit,
                              float minor, float major, bool labels_left,
                              lv_image_dsc_t *dsc, lv_obj_t **out_img, int *out_h,
                              const char *unit)
{
    int h = (int)((max_val - min_val) * px_per_unit) + TAPE_WIN_H;
    uint8_t *buf = heap_caps_malloc(TAPE_W * h * 4, MALLOC_CAP_SPIRAM);
    if (!buf) return ESP_ERR_NO_MEM;
    memset(buf, 0, TAPE_W * h * 4);

    s_cur_strip = (strip_cfg_t){min_val, max_val, px_per_unit, minor, major, h, labels_left};
    canvas_draw_once(buf, TAPE_W, h, LV_COLOR_FORMAT_ARGB8888, draw_strip);

    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc->header.w = TAPE_W;
    dsc->header.h = h;
    dsc->header.stride = TAPE_W * 4;
    dsc->data = buf;
    dsc->data_size = TAPE_W * h * 4;

    /* 裁剪窗口 */
    lv_obj_t *clip = lv_obj_create(parent);
    lv_obj_remove_flag(clip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(clip, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_scrollbar_mode(clip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(clip, x, TAPE_WIN_Y);
    lv_obj_set_size(clip, TAPE_W, TAPE_WIN_H);
    lv_obj_set_style_bg_opa(clip, LV_OPA_0, 0);
    lv_obj_set_style_border_width(clip, 0, 0);
    lv_obj_set_style_pad_all(clip, 0, 0);

    lv_obj_t *img = lv_image_create(clip);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(img, dsc);

    /* 单位标签在窗口底部（DSEG14 数码管字体直渲，居中） */
    lv_obj_t *ul = lv_label_create(parent);
    lv_obj_remove_flag(ul, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(ul, &ui_font_b612_24, 0);
    lv_obj_set_style_text_color(ul, lv_color_hex(COL_TAPE), 0);
    lv_label_set_text(ul, unit);
    lv_obj_set_pos(ul, x, TAPE_WIN_Y + TAPE_WIN_H + 2);
    lv_obj_set_width(ul, TAPE_W);
    lv_obj_set_style_text_align(ul, LV_TEXT_ALIGN_CENTER, 0);

    *out_img = img;
    *out_h = h;
    return ESP_OK;
}

/* ---------------- heading ring ---------------- */

static void draw_ring(lv_layer_t *layer)
{
    for (int deg = 0; deg < 360; deg += 5) {
        if (deg % 90 == 45) continue;   /* 45° 位置由小方块定位点替代刻度 */
        float rad = deg * (float)M_PI / 180.0f;
        bool major = (deg % 30) == 0;
        int r_in = major ? 190 : 196;           /* 主刻度略长，副刻度短（参考样式） */
        int r_out = RING_TICK_OUT;
        int x1 = RING_CX + (int)(r_in * sinf(rad));
        int y1 = RING_CY - (int)(r_in * cosf(rad));
        int x2 = RING_CX + (int)(r_out * sinf(rad));
        int y2 = RING_CY - (int)(r_out * cosf(rad));
        draw_line(layer, x1, y1, x2, y2, lv_color_hex(COL_TAPE), major ? 6 : 3);
        (void)major;
    }

    /* 45° 方向小方块定位点（NE/SE/SW/NW，真机罗盘标记） */
    lv_draw_rect_dsc_t sq;
    lv_draw_rect_dsc_init(&sq);
    sq.bg_color = lv_color_hex(COL_TAPE);
    sq.bg_opa = LV_OPA_COVER;
    sq.border_width = 0;
    sq.radius = 1;
    for (int deg = 45; deg < 360; deg += 90) {
        float rad = deg * (float)M_PI / 180.0f;
        int cx = RING_CX + (int)(201.5f * sinf(rad));   /* 对齐副刻度中心 (196+207)/2 */
        int cy = RING_CY - (int)(201.5f * cosf(rad));
        lv_area_t a = {cx - 6, cy - 6, cx + 6, cy + 6};
        lv_draw_rect(layer, &sq, &a);
    }
}

/* ---------------- 横滚刻度层（固定） + 横滚指针（随 roll 旋转） ---------------- */
/* 真机标准（波音 FCOM）：刻度弧固定不动，指针随坡度转动。
   刻度弧圆心在屏幕 (240,230)（上移10px），半径 140-158 */
#define ROLL_SCALE_CX 240
#define ROLL_SCALE_CY 190   /* 刻度画布局部坐标（画布位于屏幕 y=30，圆心屏幕 (240,220)） */
#define ROLL_PTR_W 60
#define ROLL_PTR_H 200
#define ROLL_PTR_PIVOT_X 30
#define ROLL_PTR_PIVOT_Y 190

static lv_obj_t *s_roll_ptr_img;
static lv_image_dsc_t s_roll_ptr_dsc;
static lv_image_dsc_t s_roll_scale_dsc;

static void draw_roll_scale(lv_layer_t *layer)
{
    static const int roll_marks[] = {-30, -20, -10, 0, 10, 20, 30};  /* 只留7个 */
    for (unsigned i = 0; i < sizeof(roll_marks) / sizeof(roll_marks[0]); i++) {
        int deg = roll_marks[i];
        bool major = (deg == 0 || deg == 30 || deg == -30);
        float rad = deg * (float)M_PI / 180.0f;
        int r1 = 158 - (major ? 17 : 11);
        int x1 = ROLL_SCALE_CX + (int)(r1 * sinf(rad)), y1 = ROLL_SCALE_CY - (int)(r1 * cosf(rad));
        int x2 = ROLL_SCALE_CX + (int)(158 * sinf(rad)), y2 = ROLL_SCALE_CY - (int)(158 * cosf(rad));
        draw_line(layer, x1, y1, x2, y2, lv_color_hex(COL_TAPE), major ? 6 : 3);
    }
}

static void draw_roll_pointer(lv_layer_t *layer)
{
    /* 画布局部：倒三角顶点 (30,32) 指向刻度弧（即指向 pivot 方向），底边 y=12 */
    for (int yy = 12; yy <= 32; yy += 2) {
        int hw = 13 - (yy - 12) * 13 / 20;
        draw_line(layer, ROLL_PTR_PIVOT_X - hw, yy, ROLL_PTR_PIVOT_X + hw, yy,
                  lv_color_hex(COL_TAPE), 3);
    }
}

/* ---------------- static objects ---------------- */

static lv_obj_t *make_line(lv_obj_t *parent, const lv_point_precise_t *pts, uint32_t n,
                           lv_color_t color, int width)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_line_set_points(l, pts, n);
    lv_obj_set_style_line_color(l, color, 0);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    return l;
}

static void on_screen_click(lv_event_t *e)
{
    (void)e;
    attitude_recalibrate();
}


/* ---------------- 七段数码管数值窗（复古 LED 风） ---------------- */

#define SEG_BOX_W 84
#define SEG_BOX_H 36
#define SEG_DIGIT_W 13
#define SEG_DIGIT_H 24
#define SEG_T 3

typedef struct {
    lv_obj_t *img;
    lv_image_dsc_t dsc;
    uint8_t *buf;
    char text[8];
    int16_t w;      /* 画布宽（侧框 96 对齐刻度列，顶/底框 84） */
    int8_t align;   /* 0=居中, 1=右对齐(内缩7), -1=左对齐(内缩7) */
} seg_box_t;

static seg_box_t s_seg_boxes[4];
static seg_box_t *s_cur_seg_box;

static const uint8_t s_seg_map[12] = {
    0x3F, /* 0: abcdef */
    0x06, /* 1: bc */
    0x5B, /* 2: abdeg */
    0x4F, /* 3: abcdg */
    0x66, /* 4: bcfg */
    0x6D, /* 5: acdfg */
    0x7D, /* 6: acdefg */
    0x07, /* 7: abc */
    0x7F, /* 8: abcdefg */
    0x6F, /* 9: abcdfg */
    0x40, /* 10: '-' (g) */
    0x40, /* 11: '+' (g+竖，单独处理) */
};

/* 段索引: 0=a 1=b 2=c 3=d 4=e 5=f 6=g */
static void seg_draw_one(lv_layer_t *layer, int seg, int x, int y, int dw, int dh, int t,
                         lv_color_t color, lv_opa_t opa)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = color;
    r.bg_opa = opa;
    r.border_width = 0;
    r.radius = 1;
    int hh = dh / 2;
    lv_area_t a;
    switch (seg) {
    case 0: a = (lv_area_t){x, y, x + dw, y + t}; break;
    case 1: a = (lv_area_t){x + dw - t, y, x + dw, y + hh}; break;
    case 2: a = (lv_area_t){x + dw - t, y + hh, x + dw, y + dh}; break;
    case 3: a = (lv_area_t){x, y + dh - t, x + dw, y + dh}; break;
    case 4: a = (lv_area_t){x, y + hh, x + t, y + dh}; break;
    case 5: a = (lv_area_t){x, y, x + t, y + hh}; break;
    default: a = (lv_area_t){x, y + hh - t / 2, x + dw, y + hh + t / 2 + 1}; break;
    }
    lv_draw_rect(layer, &r, &a);
}

static void seg_draw_digit(lv_layer_t *layer, char ch, int x, int y, int dw, int dh, int t,
                           lv_color_t color)
{
    int idx;
    if (ch >= '0' && ch <= '9') idx = ch - '0';
    else if (ch == '-') idx = 10;
    else if (ch == '+') idx = 11;
    else return;
    uint8_t m = s_seg_map[idx];
    for (int s = 0; s < 7; s++) {
        seg_draw_one(layer, s, x, y, dw, dh, t, color,
                     (m & (1 << s)) ? LV_OPA_COVER : LV_OPA_0);  /* 灭段不画 */
    }
    if (idx == 11) {
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_color = color;
        r.bg_opa = LV_OPA_COVER;
        r.border_width = 0;
        r.radius = 1;
        lv_area_t a = {x + dw / 2 - 1, y + 4, x + dw / 2 + 1, y + dh - 4};
        lv_draw_rect(layer, &r, &a);
    }
}

/* 绘制一串七段数字，返回总宽 */
static int seg_draw_text(lv_layer_t *layer, const char *text, int x, int y, int dw, int dh, int t,
                         lv_color_t color)
{
    int n = (int)strlen(text);
    for (int i = 0; i < n; i++) {
        seg_draw_digit(layer, text[i], x, y, dw, dh, t, color);
        x += dw + 3;
    }
    return n * (dw + 3) - 3;
}

static int seg_text_width(const char *text, int dw)
{
    return (int)strlen(text) * (dw + 3) - 3;
}

static void seg_box_redraw(lv_layer_t *layer)
{
    seg_box_t *b = s_cur_seg_box;

    /* 白线顶到画布边缘，中间纯黑底，无透明无毛刺 */
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_white();
    bg.bg_opa = LV_OPA_COVER;
    bg.border_width = 0;
    bg.radius = 0;
    lv_area_t top = {0, 0, b->w - 1, 2};
    lv_draw_rect(layer, &bg, &top);
    lv_area_t bottom = {0, SEG_BOX_H - 3, b->w - 1, SEG_BOX_H - 1};
    lv_draw_rect(layer, &bg, &bottom);

    bg.bg_color = lv_color_black();
    lv_area_t mid = {0, 3, b->w - 1, SEG_BOX_H - 4};
    lv_draw_rect(layer, &bg, &mid);

    /* 数字对齐：侧框与刻度表数字同列（右框左对齐/左框右对齐，内缩7），顶/底框居中 */
    int n = (int)strlen(b->text);
    int total = n * (SEG_DIGIT_W + 3) - 3;
    int x;
    if (b->align > 0) x = b->w - 7 - total;
    else if (b->align < 0) x = 7;
    else x = (b->w - total) / 2;
    int y = (SEG_BOX_H - SEG_DIGIT_H) / 2;
    seg_draw_text(layer, b->text, x, y, SEG_DIGIT_W, SEG_DIGIT_H, SEG_T, lv_color_hex(COL_TAPE));
}

static void seg_box_set(int idx, const char *text)
{
    seg_box_t *b = &s_seg_boxes[idx];
    if (strcmp(b->text, text) == 0) return;
    strncpy(b->text, text, sizeof(b->text) - 1);
    b->text[sizeof(b->text) - 1] = '\0';
    s_cur_seg_box = b;
    canvas_draw_once(b->buf, b->w, SEG_BOX_H, LV_COLOR_FORMAT_ARGB8888, seg_box_redraw);
    lv_image_cache_drop(&b->dsc);
    lv_obj_invalidate(b->img);
}

static lv_obj_t *make_seg_box(lv_obj_t *parent, int x, int y, int idx, int w, int align)
{
    seg_box_t *b = &s_seg_boxes[idx];
    b->w = w;
    b->align = align;
    b->buf = heap_caps_malloc(w * SEG_BOX_H * 4, MALLOC_CAP_SPIRAM);
    if (!b->buf) return NULL;
    b->text[0] = '\0';

    b->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    b->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    b->dsc.header.w = w;
    b->dsc.header.h = SEG_BOX_H;
    b->dsc.header.stride = w * 4;
    b->dsc.data = b->buf;
    b->dsc.data_size = w * SEG_BOX_H * 4;

    b->img = lv_image_create(parent);
    lv_obj_remove_flag(b->img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(b->img, &b->dsc);
    lv_obj_set_pos(b->img, x, y);
    return b->img;
}

/* ---------------- public API ---------------- */


esp_err_t ui_ahrs_init(void)
{
    uint8_t *card = heap_caps_malloc(CARD_W * CARD_H * 2, MALLOC_CAP_SPIRAM);
    if (!card) {
        ESP_LOGE(TAG, "no PSRAM for horizon card");
        return ESP_ERR_NO_MEM;
    }
    ui_card_ptr = card;
    uint8_t *ring = heap_caps_malloc(RING_SIZE * RING_SIZE * 4, MALLOC_CAP_SPIRAM);
    if (!ring) {
        ESP_LOGE(TAG, "no PSRAM for heading ring");
        return ESP_ERR_NO_MEM;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* 地平线卡片 */
    canvas_draw_once(card, CARD_W, CARD_H, LV_COLOR_FORMAT_RGB565, draw_horizon);
    s_horizon_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_horizon_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_horizon_dsc.header.w = CARD_W;
    s_horizon_dsc.header.h = CARD_H;
    s_horizon_dsc.header.stride = CARD_W * 2;
    s_horizon_dsc.data = card;
    s_horizon_dsc.data_size = CARD_W * CARD_H * 2;

    s_horizon_dst = heap_caps_malloc(HORIZON_VIEW * HORIZON_VIEW * 2, MALLOC_CAP_SPIRAM);
    if (!s_horizon_dst) {
        ESP_LOGE(TAG, "no PSRAM for horizon dst");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "card sky px=0x%04X ground px=0x%04X",
             ((const uint16_t *)card)[240 * CARD_W + 480],
             ((const uint16_t *)card)[720 * CARD_W + 480]);
    render_horizon(card, 0.0f, 0);
    ESP_LOGI(TAG, "dst sky px=0x%04X ground px=0x%04X",
             ((const uint16_t *)s_horizon_dst)[120 * HORIZON_VIEW + 240],
             ((const uint16_t *)s_horizon_dst)[360 * HORIZON_VIEW + 240]);

    s_horizon_img = lv_image_create(scr);
    lv_obj_remove_flag(s_horizon_img, LV_OBJ_FLAG_CLICKABLE);
    dst_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dst_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dst_dsc.header.w = HORIZON_VIEW;
    dst_dsc.header.h = HORIZON_VIEW;
    dst_dsc.header.stride = HORIZON_VIEW * 2;
    dst_dsc.data = s_horizon_dst;
    dst_dsc.data_size = HORIZON_VIEW * HORIZON_VIEW * 2;
    lv_image_set_src(s_horizon_img, &dst_dsc);
    lv_obj_set_pos(s_horizon_img, 0, 0);

    /* 航向罗盘环（透明底，只露底部倒扣半月） */
    memset(ring, 0, RING_SIZE * RING_SIZE * 4);
    canvas_draw_once(ring, RING_SIZE, RING_SIZE, LV_COLOR_FORMAT_ARGB8888, draw_ring);
    s_ring_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_ring_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    s_ring_dsc.header.w = RING_SIZE;
    s_ring_dsc.header.h = RING_SIZE;
    s_ring_dsc.header.stride = RING_SIZE * 4;
    s_ring_dsc.data = ring;
    s_ring_dsc.data_size = RING_SIZE * RING_SIZE * 4;

    s_ring_img = lv_image_create(scr);
    lv_obj_remove_flag(s_ring_img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(s_ring_img, &s_ring_dsc);
    lv_obj_set_pos(s_ring_img, 0, RING_POS_Y);
    lv_image_set_pivot(s_ring_img, RING_CX, RING_CY);

    /* 罗盘直立文字：数字用七段窗，N/E/S/W 字母用粗体 */
    for (int i = 0; i < 12; i++) {
        bool is_letter = (i == 0 || i == 3 || i == 6 || i == 9);
        if (is_letter) {
            lv_obj_t *l = lv_label_create(scr);
            lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_text_font(l, &ui_font_b612_28, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(COL_TAPE), 0);
            lv_obj_set_width(l, HDG_LBL_BOX_W);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(l, s_hdg_texts[i]);
            s_hdg_lbls[i] = l;
        } else {
            uint8_t *buf = heap_caps_malloc(HDG_LBL_BOX_W * HDG_LBL_BOX_H * 4, MALLOC_CAP_SPIRAM);
            lv_image_dsc_t *dsc = &s_hdg_dscs[i];
            dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
            dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
            dsc->header.w = HDG_LBL_BOX_W;
            dsc->header.h = HDG_LBL_BOX_H;
            dsc->header.stride = HDG_LBL_BOX_W * 4;
            dsc->data = buf;
            dsc->data_size = HDG_LBL_BOX_W * HDG_LBL_BOX_H * 4;
            memset(buf, 0, HDG_LBL_BOX_W * HDG_LBL_BOX_H * 4);

            s_cur_seg_text = s_hdg_texts[i];
            canvas_draw_once(buf, HDG_LBL_BOX_W, HDG_LBL_BOX_H, LV_COLOR_FORMAT_ARGB8888,
                             draw_hdg_seg_label);
            lv_image_cache_drop(dsc);

            lv_obj_t *img = lv_image_create(scr);
            lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
            lv_image_set_src(img, dsc);
            s_hdg_lbls[i] = img;
        }
    }

    /* 左右刻度带（预渲染长条 + 滚动窗口） */
    if (create_strip(scr, 0, 0.0f, SPD_MAX, SPD_PX, 5.0f, 10.0f, true,
                     &s_dsc_left, &s_img_left, &s_strip_h_left, "kt") != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    if (create_strip(scr, SCR_W - TAPE_W, -500.0f, ALT_MAX, ALT_PX, 20.0f, 100.0f, false,
                     &s_dsc_right, &s_img_right, &s_strip_h_right, "ft") != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    /* 点触屏幕重新校准姿态零点 */
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, on_screen_click, LV_EVENT_CLICKED, NULL);

    /* 航向指针：底部中央实心三角 ▽ 指向罗盘环（扫描线填充） */
    static lv_point_precise_t fill_pts[11][2];
    int fill_i = 0;
    for (int yy = 372; yy <= 392 && fill_i < 11; yy += 2, fill_i++) {
        int hw = 13 - (yy - 372) * 13 / 20;
        fill_pts[fill_i][0].x = SCR_CX - hw; fill_pts[fill_i][0].y = yy;
        fill_pts[fill_i][1].x = SCR_CX + hw; fill_pts[fill_i][1].y = yy;
        lv_obj_t *fl = lv_line_create(scr);
        lv_obj_remove_flag(fl, LV_OBJ_FLAG_CLICKABLE);
        lv_line_set_points(fl, fill_pts[fill_i], 2);
        lv_obj_set_style_line_color(fl, lv_color_hex(COL_TAPE), 0);
        lv_obj_set_style_line_width(fl, 3, 0);
    }

    /* 飞机符号：短机翼横杆 + 无缝中央 V 缺口 */
    lv_color_t ac = lv_color_hex(COL_TAPE);  /* 飞机符号荧光绿 */
    static const lv_point_precise_t w1[] = {{SCR_CX - 44, SCR_CY}, {SCR_CX - 13, SCR_CY}};
    static const lv_point_precise_t w2[] = {{SCR_CX + 13, SCR_CY}, {SCR_CX + 44, SCR_CY}};
    static const lv_point_precise_t v1[] = {{SCR_CX - 13, SCR_CY}, {SCR_CX, SCR_CY + 14}};
    static const lv_point_precise_t v2[] = {{SCR_CX, SCR_CY + 14}, {SCR_CX + 13, SCR_CY}};
    make_line(scr, w1, 2, ac, 5);
    make_line(scr, w2, 2, ac, 5);
    make_line(scr, v1, 2, ac, 5);
    make_line(scr, v2, 2, ac, 5);

    /* 横滚刻度弧：固定层（480x130 画布，屏幕 y=40 起，圆心屏幕 (240,230)） */
    uint8_t *rs_buf = heap_caps_malloc(480 * 130 * 4, MALLOC_CAP_SPIRAM);
    if (!rs_buf) return ESP_ERR_NO_MEM;
    memset(rs_buf, 0, 480 * 130 * 4);
    canvas_draw_once(rs_buf, 480, 130, LV_COLOR_FORMAT_ARGB8888, draw_roll_scale);
    memset(&s_roll_scale_dsc, 0, sizeof(s_roll_scale_dsc));
    s_roll_scale_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_roll_scale_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    s_roll_scale_dsc.header.w = 480;
    s_roll_scale_dsc.header.h = 130;
    s_roll_scale_dsc.header.stride = 480 * 4;
    s_roll_scale_dsc.data = rs_buf;
    s_roll_scale_dsc.data_size = 480 * 130 * 4;
    lv_obj_t *rs_img = lv_image_create(scr);
    lv_obj_remove_flag(rs_img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(rs_img, &s_roll_scale_dsc);
    lv_obj_set_pos(rs_img, 0, 31);

    /* 横滚指针：60x200 画布，pivot (30,190)=屏幕 (240,220)，随 roll 旋转 */
    uint8_t *rp_buf = heap_caps_malloc(ROLL_PTR_W * ROLL_PTR_H * 4, MALLOC_CAP_SPIRAM);
    if (!rp_buf) return ESP_ERR_NO_MEM;
    memset(rp_buf, 0, ROLL_PTR_W * ROLL_PTR_H * 4);
    canvas_draw_once(rp_buf, ROLL_PTR_W, ROLL_PTR_H, LV_COLOR_FORMAT_ARGB8888, draw_roll_pointer);
    memset(&s_roll_ptr_dsc, 0, sizeof(s_roll_ptr_dsc));
    s_roll_ptr_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_roll_ptr_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    s_roll_ptr_dsc.header.w = ROLL_PTR_W;
    s_roll_ptr_dsc.header.h = ROLL_PTR_H;
    s_roll_ptr_dsc.header.stride = ROLL_PTR_W * 4;
    s_roll_ptr_dsc.data = rp_buf;
    s_roll_ptr_dsc.data_size = ROLL_PTR_W * ROLL_PTR_H * 4;
    s_roll_ptr_img = lv_image_create(scr);
    lv_obj_remove_flag(s_roll_ptr_img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(s_roll_ptr_img, &s_roll_ptr_dsc);
    lv_obj_set_pos(s_roll_ptr_img, SCR_CX - ROLL_PTR_PIVOT_X, 30 + ROLL_SCALE_CY - ROLL_PTR_PIVOT_Y);
    lv_image_set_pivot(s_roll_ptr_img, ROLL_PTR_PIVOT_X, ROLL_PTR_PIVOT_Y);

    make_seg_box(scr, 0, SCR_CY - 18, 0, 77, 1);        /* 空速（贴左缘，数字右对齐同刻度列） */
    make_seg_box(scr, SCR_W - 77, SCR_CY - 18, 1, 77, -1); /* 海拔（贴右缘，数字左对齐同刻度列） */
    make_seg_box(scr, SCR_CX - SEG_BOX_W / 2, SCR_H - SEG_BOX_H - 2, 2, SEG_BOX_W, 0); /* 航向（距下缘2px防吞边） */
    make_seg_box(scr, SCR_CX - SEG_BOX_W / 2, 2, 3, SEG_BOX_W, 0);          /* 俯仰（距上缘2px防吞边） */

    s_lbl_demo = lv_label_create(scr);
    lv_obj_remove_flag(s_lbl_demo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(s_lbl_demo, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_demo, lv_color_hex(0xFF4040), 0);
    lv_label_set_text(s_lbl_demo, "IMU DEMO");
    lv_obj_center(s_lbl_demo);
    lv_obj_set_y(s_lbl_demo, 64);
    lv_obj_add_flag(s_lbl_demo, LV_OBJ_FLAG_HIDDEN);

    s_heartbeat = lv_obj_create(scr);
    lv_obj_remove_flag(s_heartbeat, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_heartbeat, 4, 4);
    lv_obj_set_size(s_heartbeat, 8, 8);
    lv_obj_set_style_bg_color(s_heartbeat, lv_color_hex(COL_TAPE), 0);
    lv_obj_set_style_border_width(s_heartbeat, 0, 0);
    lv_obj_set_style_radius(s_heartbeat, 2, 0);

    /* 对话状态点（顶部俯仰黑框右侧）：默认隐藏，录音红/处理黄/播报绿。
       先剥掉 LVGL 主题残留样式，纯实心圆，无任何装饰。 */
    s_voice_dot = lv_obj_create(scr);
    lv_obj_remove_style_all(s_voice_dot);
    lv_obj_remove_flag(s_voice_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_voice_dot, SCR_CX + SEG_BOX_W / 2 + 14, 6);
    lv_obj_set_size(s_voice_dot, 28, 28);
    lv_obj_set_style_radius(s_voice_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_voice_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_voice_dot, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "ahrs ui ready");
    return ESP_OK;
}

void ui_ahrs_set_voice_state(int state)
{
    if (!s_voice_dot) return;
    if (bsp_display_lock(1000) != 0) return;
    static const uint32_t colors[] = {0, 0xFF3030, 0xFFC000, 0x39FF14};
    if (state == 0) {
        lv_obj_add_flag(s_voice_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_voice_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_voice_dot, lv_color_hex(colors[state & 3]), 0);
    }
    bsp_display_unlock();
}

void ui_ahrs_update(const flight_state_t *st)
{
    /* 心跳点 */
    static int beat;
    if (++beat >= 15) {
        beat = 0;
        if (lv_obj_has_flag(s_heartbeat, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(s_heartbeat, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_heartbeat, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 地平线：旋转 + 俯仰平移 */
    int qr = (int)lroundf(st->roll * 3.3f);
    int qp = (int)lroundf(st->pitch * 3.3f);
    if (qr != s_q_roll || qp != s_q_pitch) {
        s_q_roll = qr;
        s_q_pitch = qp;
        int pitch_px = (int)lroundf(st->pitch * PX_PER_DEG);
        /* 钳制 ±176px（±35°）：保证 960 卡片在任意横滚角下四角不出界 */
        if (pitch_px > 176) pitch_px = 176;
        if (pitch_px < -176) pitch_px = -176;
        render_horizon(ui_card_ptr, st->roll, pitch_px);
        lv_image_set_rotation(s_roll_ptr_img, (int)lroundf(st->roll * 10.0f)); /* 横滚指针随坡度转 */
        lv_image_cache_drop(&dst_dsc);
        lv_obj_invalidate(s_horizon_img);

        char txt[16];
        if (fabsf(st->pitch) < 0.5f) {
            snprintf(txt, sizeof(txt), "0");
        } else {
            snprintf(txt, sizeof(txt), "%+.0f", st->pitch);
        }
        seg_box_set(3, txt);
    }

    /* 空速带滚动（显示节 kt，内部 km/h ÷1.852） */
    float spd_kt = st->airspeed_kmh * 0.539957f;
    int qs = (int)lroundf(spd_kt);
    if (qs != s_q_speed) {
        s_q_speed = qs;
        float v = spd_kt;
        if (v > SPD_MAX) v = SPD_MAX;
        lv_image_set_offset_y(s_img_left, (int)(TAPE_WIN_H - s_strip_h_left + v * SPD_PX));
        char txt[16];
        snprintf(txt, sizeof(txt), "%d", qs);
        seg_box_set(0, txt);
    }

    /* 海拔带滚动（显示英尺 ft，内部 m ×3.28084） */
    float alt_ft = st->altitude_m * 3.28084f;
    int qa = (int)lroundf(alt_ft);
    if (qa != s_q_alt) {
        s_q_alt = qa;
        float v = alt_ft;
        if (v < -500.0f) v = -500.0f;
        if (v > ALT_MAX) v = ALT_MAX;
        lv_image_set_offset_y(s_img_right, (int)(TAPE_WIN_H - s_strip_h_right + (v + 500.0f) * ALT_PX));
        char txt[16];
        snprintf(txt, sizeof(txt), "%d", qa);
        seg_box_set(1, txt);
    }

    /* 罗盘环旋转：航向增大 → 环逆时针转（底部刻度向右移动） */
    int qh = (int)lroundf(st->heading) % 360;
    if (qh < 0) qh += 360;
    if (qh != s_q_hdg) {
        s_q_hdg = qh;
        lv_image_set_rotation(s_ring_img, (int32_t)(-qh * 10));
        for (int i = 0; i < 12; i++) {
            float a = (s_hdg_degs[i] - st->heading) * (float)M_PI / 180.0f;
            int x = SCR_CX + (int)(HDG_LBL_R * sinf(a)) - 31;
            int y = (RING_POS_Y + RING_CY) - (int)(HDG_LBL_R * cosf(a)) - 14;
            lv_obj_set_pos(s_hdg_lbls[i], x, y);
        }
        char txt[16];
        snprintf(txt, sizeof(txt), "%03d", qh);
        seg_box_set(2, txt);
    }

    if (st->imu_ok) {
        lv_obj_add_flag(s_lbl_demo, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_lbl_demo, LV_OBJ_FLAG_HIDDEN);
    }
}
