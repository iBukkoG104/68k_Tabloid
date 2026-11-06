/*
 * ssd1963.c - emulazione del controller SSD1963 (800x480 RGB565, 8080 16 bit).
 *
 * Cosa e' modellato:
 *   - frame buffer interno RGB565 (il chip vero ne ha 1215 kB: 800*480*2 =
 *     768 kB ci stanno comodi);
 *   - set_column_address / set_page_address con wrap dentro la finestra;
 *   - write_memory_start / _continue e read_memory_start / _continue;
 *   - set_address_mode (0x36): scambio pagina/colonna, mirror X, mirror Y,
 *     ordine RGB/BGR;
 *   - set_pixel_data_interface (0xF0): 8 bit 888, 12 bit 444, 16 bit
 *     impaccato, 16 bit 565 (il formato del nostro pannello), 18/24 bit;
 *   - display on/off, sleep, idle (8 colori), invert;
 *   - scroll verticale hardware (set_scroll_area / set_scroll_start);
 *   - set_lcd_mode: ridimensiona il pannello (HDP+1 x VDP+1);
 *   - set_pwm_conf: duty del PWM di backlight -> luminosita' della finestra;
 *   - i comandi get_* restituiscono i valori impostati, read_ddb l'ID reale
 *     dell'SSD1963 (01 57 61 01 FF);
 *   - gli altri comandi (PLL, periodi di h/v sync, GPIO, DBC, post-process)
 *     sono accettati, memorizzati e loggati: non cambiano l'immagine ma non
 *     fanno sballare la macchina a stati dei parametri.
 */
#include "ssd1963.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPARAM_MAX 16
#define RDBUF_MAX  16

/* -1 = comando a flusso (write/read memory), altrimenti numero di parametri */
#define STREAM (-1)

typedef struct {
    ssd1963_state_t pub;

    uint16_t fb[SSD_FB_PIXELS];

    /* macchina a stati di comando/parametri */
    uint8_t  cmd;
    int      nparam;                /* attesi */
    int      pcount;                /* ricevuti */
    uint8_t  param[NPARAM_MAX];
    int      streaming;             /* 1 = write memory attivo */
    int      reading;               /* 1 = read memory attivo   */

    /* assemblatore dei formati pixel che richiedono piu' trasferimenti */
    uint8_t  pacc[4];
    int      pn;

    /* coda dei byte di risposta per i comandi get_* */
    uint8_t  rdbuf[RDBUF_MAX];
    int      rdlen, rdpos;

    /* registri memorizzati */
    uint8_t  lcd_mode[7];
    uint8_t  hori[8], vert[7];
    uint8_t  gpio_conf[2], gpio_val;
    uint8_t  post_proc[4];
    uint8_t  pwm_conf[6];
    uint8_t  dbc_conf, dbc_th[9];
    uint8_t  pll, pll_mn[3], lshift[3];
    uint8_t  gamma;
    uint8_t  tear_mode;
    uint16_t part_start, part_end;
    uint16_t sc_tfa, sc_vsa, sc_bfa, sc_vsp;

    int      log;
    int      warn_n;
} ssd_t;

static ssd_t S;

/* ------------------------------------------------------------------ */
static void logf_(int lvl, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
;

static void logf_(int lvl, const char *fmt, ...)
{
    if (S.log < lvl) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[SSD1963] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void warn_(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
;

static void warn_(const char *fmt, ...)
{
    if (S.warn_n++ >= 20) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[SSD1963][warn] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void touch_frame(void) { S.pub.frame_seq++; }

/* ------------------------------------------------------------------ */
/* Tabella dei comandi                                                 */
/* ------------------------------------------------------------------ */
typedef struct { uint8_t cmd; int8_t np; int8_t nr; const char *name; } cmdent_t;

/* np = parametri in scrittura, nr = byte restituiti in lettura */
static const cmdent_t CMDS[] = {
    { 0x00, 0, 0, "nop" },
    { 0x01, 0, 0, "soft_reset" },
    { 0x0A, 0, 1, "get_power_mode" },
    { 0x0B, 0, 1, "get_address_mode" },
    { 0x0D, 0, 1, "get_display_mode" },
    { 0x0E, 0, 1, "get_tear_effect_status" },
    { 0x10, 0, 0, "enter_sleep_mode" },
    { 0x11, 0, 0, "exit_sleep_mode" },
    { 0x12, 0, 0, "enter_partial_mode" },
    { 0x13, 0, 0, "enter_normal_mode" },
    { 0x20, 0, 0, "exit_invert_mode" },
    { 0x21, 0, 0, "enter_invert_mode" },
    { 0x26, 1, 0, "set_gamma_curve" },
    { 0x28, 0, 0, "set_display_off" },
    { 0x29, 0, 0, "set_display_on" },
    { 0x2A, 4, 0, "set_column_address" },
    { 0x2B, 4, 0, "set_page_address" },
    { 0x2C, STREAM, 0, "write_memory_start" },
    { 0x2E, 0, STREAM, "read_memory_start" },
    { 0x30, 4, 0, "set_partial_area" },
    { 0x33, 6, 0, "set_scroll_area" },
    { 0x34, 0, 0, "set_tear_off" },
    { 0x35, 1, 0, "set_tear_on" },
    { 0x36, 1, 0, "set_address_mode" },
    { 0x37, 2, 0, "set_scroll_start" },
    { 0x38, 0, 0, "exit_idle_mode" },
    { 0x39, 0, 0, "enter_idle_mode" },
    { 0x3C, STREAM, 0, "write_memory_continue" },
    { 0x3E, 0, STREAM, "read_memory_continue" },
    { 0xA1, 0, 5, "read_ddb" },
    { 0xB0, 7, 0, "set_lcd_mode" },
    { 0xB1, 0, 7, "get_lcd_mode" },
    { 0xB4, 8, 0, "set_hori_period" },
    { 0xB5, 0, 8, "get_hori_period" },
    { 0xB6, 7, 0, "set_vert_period" },
    { 0xB7, 0, 7, "get_vert_period" },
    { 0xB8, 2, 0, "set_gpio_conf" },
    { 0xB9, 0, 2, "get_gpio_conf" },
    { 0xBA, 1, 0, "set_gpio_value" },
    { 0xBB, 0, 1, "get_gpio_status" },
    { 0xBC, 4, 0, "set_post_proc" },
    { 0xBD, 0, 4, "get_post_proc" },
    { 0xBE, 6, 0, "set_pwm_conf" },
    { 0xBF, 0, 6, "get_pwm_conf" },
    { 0xC0, 6, 0, "set_lcd_gen0" }, { 0xC1, 0, 6, "get_lcd_gen0" },
    { 0xC2, 6, 0, "set_lcd_gen1" }, { 0xC3, 0, 6, "get_lcd_gen1" },
    { 0xC4, 6, 0, "set_lcd_gen2" }, { 0xC5, 0, 6, "get_lcd_gen2" },
    { 0xC6, 6, 0, "set_lcd_gen3" }, { 0xC7, 0, 6, "get_lcd_gen3" },
    { 0xC8, 2, 0, "set_gpio0_rop" }, { 0xC9, 0, 2, "get_gpio0_rop" },
    { 0xCA, 2, 0, "set_gpio1_rop" }, { 0xCB, 0, 2, "get_gpio1_rop" },
    { 0xCC, 2, 0, "set_gpio2_rop" }, { 0xCD, 0, 2, "get_gpio2_rop" },
    { 0xCE, 2, 0, "set_gpio3_rop" }, { 0xCF, 0, 2, "get_gpio3_rop" },
    { 0xD0, 1, 0, "set_dbc_conf" },  { 0xD1, 0, 1, "get_dbc_conf" },
    { 0xD4, 9, 0, "set_dbc_th" },    { 0xD5, 0, 9, "get_dbc_th" },
    { 0xE0, 1, 0, "set_pll" },
    { 0xE2, 3, 0, "set_pll_mn" },
    { 0xE4, 0, 3, "get_pll_mn" },
    { 0xE5, 0, 1, "get_pll_status" },
    { 0xE6, 3, 0, "set_lshift_freq" },
    { 0xE7, 0, 3, "get_lshift_freq" },
    { 0xF0, 1, 0, "set_pixel_data_interface" },
    { 0xF1, 0, 1, "get_pixel_data_interface" },
};

static const cmdent_t *cmd_find(uint8_t c)
{
    for (size_t i = 0; i < sizeof CMDS / sizeof CMDS[0]; i++)
        if (CMDS[i].cmd == c) return &CMDS[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Frame buffer                                                        */
/* ------------------------------------------------------------------ */
static void window_reset(void)
{
    S.pub.col_start = 0; S.pub.col_end  = (uint16_t)(S.pub.w - 1);
    S.pub.page_start = 0; S.pub.page_end = (uint16_t)(S.pub.h - 1);
    S.pub.cx = 0; S.pub.cy = 0;
}

/* Trasforma la posizione logica (cx,cy) in coordinate del pannello
 * applicando set_address_mode: bit5 scambio pagina/colonna, bit6 mirror
 * sulle colonne, bit7 mirror sulle pagine. */
static int map_xy(unsigned *px, unsigned *py)
{
    unsigned x = S.pub.cx, y = S.pub.cy;
    if (S.pub.addr_mode & 0x20) { unsigned t = x; x = y; y = t; }
    if (S.pub.addr_mode & 0x40) x = (unsigned)(S.pub.w - 1) - x;
    if (S.pub.addr_mode & 0x80) y = (unsigned)(S.pub.h - 1) - y;
    *px = x; *py = y;
    return x < S.pub.w && y < S.pub.h;
}

static void advance(void)
{
    if (S.pub.cx >= S.pub.col_end) {
        S.pub.cx = S.pub.col_start;
        if (S.pub.cy >= S.pub.page_end) S.pub.cy = S.pub.page_start;
        else                            S.pub.cy++;
    } else {
        S.pub.cx++;
    }
}

static uint16_t bgr_fix(uint16_t c)
{
    if (!(S.pub.addr_mode & 0x08)) return c;
    /* RGB <-> BGR: scambia i 5 bit alti con i 5 bassi */
    return (uint16_t)(((c & 0x001F) << 11) | (c & 0x07E0) | ((c >> 11) & 0x001F));
}

static void put_pixel(uint16_t rgb565)
{
    unsigned x, y;
    if (map_xy(&x, &y)) {
        uint16_t c = bgr_fix(rgb565);
        uint16_t *p = &S.fb[y * SSD_W_MAX + x];
        if (*p != c) { *p = c; touch_frame(); }
    }
    S.pub.px_written++;
    advance();
}

static uint16_t get_pixel(void)
{
    unsigned x, y;
    uint16_t c = 0;
    if (map_xy(&x, &y)) c = bgr_fix(S.fb[y * SSD_W_MAX + x]);
    advance();
    return c;
}

static uint16_t rgb888_to_565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* ------------------------------------------------------------------ */
/* Flusso dei dati pixel                                               */
/* ------------------------------------------------------------------ */
static void feed_byte(uint8_t b)
{
    switch (S.pub.pix_fmt) {
    case SSD_PIX_16_565:
        /* bus a 8 bit su formato 565: byte alto poi byte basso */
        S.pacc[S.pn++] = b;
        if (S.pn == 2) { put_pixel((uint16_t)((S.pacc[0] << 8) | S.pacc[1])); S.pn = 0; }
        break;
    case SSD_PIX_12:
        /* 444 impaccato: 3 byte = 2 pixel (R0G0 B0R1 G1B1) */
        S.pacc[S.pn++] = b;
        if (S.pn == 3) {
            unsigned r0 = (S.pacc[0] >> 4) * 17, g0 = (S.pacc[0] & 15) * 17;
            unsigned b0 = (S.pacc[1] >> 4) * 17, r1 = (S.pacc[1] & 15) * 17;
            unsigned g1 = (S.pacc[2] >> 4) * 17, b1 = (S.pacc[2] & 15) * 17;
            put_pixel(rgb888_to_565(r0, g0, b0));
            put_pixel(rgb888_to_565(r1, g1, b1));
            S.pn = 0;
        }
        break;
    default:
        /* 8 bit / 16 bit impaccato / 18 / 24 bit: 3 byte = 1 pixel (888) */
        S.pacc[S.pn++] = b;
        if (S.pn == 3) {
            put_pixel(rgb888_to_565(S.pacc[0], S.pacc[1], S.pacc[2]));
            S.pn = 0;
        }
        break;
    }
}

static void feed_word(uint16_t w)
{
    switch (S.pub.pix_fmt) {
    case SSD_PIX_16_565:
        put_pixel(w);                       /* il percorso del nostro pannello */
        break;
    case SSD_PIX_8:
        warn_("formato 8 bit con accesso .W: uso solo D7-D0");
        feed_byte((uint8_t)(w & 0xFF));
        break;
    case SSD_PIX_18:
    case SSD_PIX_9:
        warn_("formato %d non trasportabile su bus 16 bit: trattato come 565",
              S.pub.pix_fmt);
        put_pixel(w);
        break;
    default:
        /* 12 bit, 16 bit impaccato, 24 bit: flusso di byte, alto poi basso */
        feed_byte((uint8_t)(w >> 8));
        feed_byte((uint8_t)(w & 0xFF));
        break;
    }
}

/* Un byte del flusso di lettura nei formati a 888 (3 byte per pixel). */
static uint8_t drain_byte888(void)
{
    if (S.pn == 0) {
        uint16_t c = get_pixel();
        S.pacc[0] = (uint8_t)(((c >> 11) & 0x1F) << 3);
        S.pacc[1] = (uint8_t)(((c >> 5)  & 0x3F) << 2);
        S.pacc[2] = (uint8_t)(( c        & 0x1F) << 3);
        S.pn = 3;
    }
    return S.pacc[3 - S.pn--];
}

static uint32_t drain_pixel(int width)
{
    if (S.pub.pix_fmt == SSD_PIX_16_565) {
        if (width == 16) return get_pixel();
        /* bus a 8 bit: byte alto e poi byte basso dello stesso pixel */
        if (S.pn == 0) {
            uint16_t c = get_pixel();
            S.pacc[0] = (uint8_t)(c >> 8);
            S.pacc[1] = (uint8_t)c;
            S.pn = 1;
            return S.pacc[0];
        }
        S.pn = 0;
        return S.pacc[1];
    }
    /* altri formati: flusso di byte 888, due per ogni ciclo a 16 bit */
    if (width == 16) {
        uint32_t hi = drain_byte888();
        return (hi << 8) | drain_byte888();
    }
    return drain_byte888();
}

/* ------------------------------------------------------------------ */
/* Esecuzione dei comandi                                              */
/* ------------------------------------------------------------------ */
static uint16_t p16(int i) { return (uint16_t)((S.param[i] << 8) | S.param[i + 1]); }

static void rd_set(const uint8_t *p, int n)
{
    if (n > RDBUF_MAX) n = RDBUF_MAX;
    memcpy(S.rdbuf, p, (size_t)n);
    S.rdlen = n; S.rdpos = 0;
}

static void rd_set1(uint8_t v) { rd_set(&v, 1); }

static void resize(unsigned w, unsigned h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > SSD_W_MAX || h > SSD_H_MAX) {
        warn_("pannello %ux%u fuori dai limiti del modello (%ux%u): troncato",
              w, h, SSD_W_MAX, SSD_H_MAX);
        if (w > SSD_W_MAX) w = SSD_W_MAX;
        if (h > SSD_H_MAX) h = SSD_H_MAX;
    }
    if (w == S.pub.w && h == S.pub.h) return;
    S.pub.w = (uint16_t)w; S.pub.h = (uint16_t)h;
    window_reset();
    S.sc_tfa = 0; S.sc_vsa = (uint16_t)h; S.sc_bfa = 0; S.sc_vsp = 0;
    S.part_start = 0; S.part_end = (uint16_t)(h - 1);
    logf_(1, "pannello ridimensionato a %ux%u", w, h);
    touch_frame();
}

static void exec_cmd(void)
{
    uint8_t c = S.cmd;
    S.pub.cmds++;

    switch (c) {
    case 0x00: break;                                  /* nop */
    case 0x01: ssd1963_reset(); logf_(1, "soft_reset"); break;

    case 0x0A: {                                       /* get_power_mode */
        uint8_t v = 0x80;                              /* booster on */
        if (S.pub.idle)       v |= 0x40;
        if (S.pub.partial)    v |= 0x20;
        if (!S.pub.sleep)     v |= 0x10;               /* sleep out */
        if (!S.pub.partial)   v |= 0x08;               /* normal mode */
        if (S.pub.display_on) v |= 0x04;
        rd_set1(v);
        break;
    }
    case 0x0B: rd_set1(S.pub.addr_mode); break;        /* get_address_mode */
    case 0x0D: {                                       /* get_display_mode */
        uint8_t v = (uint8_t)(S.gamma & 0x07);
        if (S.sc_vsa != S.pub.h || S.sc_vsp) v |= 0x80; /* scroll attivo */
        if (S.pub.invert)                    v |= 0x20;
        if (S.pub.idle)                      v |= 0x40;
        rd_set1(v);
        break;
    }
    case 0x0E: rd_set1((uint8_t)(S.pub.tear_on ? 0x80 : 0x00)); break;

    case 0x10: S.pub.sleep = 1; touch_frame(); break;
    case 0x11: S.pub.sleep = 0; touch_frame(); break;
    case 0x12: S.pub.partial = 1; touch_frame(); break;
    case 0x13: S.pub.partial = 0; touch_frame(); break;
    case 0x20: S.pub.invert = 0; touch_frame(); break;
    case 0x21: S.pub.invert = 1; touch_frame(); break;
    case 0x26: S.gamma = S.param[0]; break;
    case 0x28: S.pub.display_on = 0; touch_frame(); logf_(1, "display OFF"); break;
    case 0x29: S.pub.display_on = 1; touch_frame(); logf_(1, "display ON"); break;

    case 0x2A: {                                       /* set_column_address */
        uint16_t sc = p16(0), ec = p16(2);
        if (sc > ec) { warn_("set_column_address con SC>EC (%u>%u)", sc, ec); }
        S.pub.col_start = sc; S.pub.col_end = ec;
        S.pub.cx = sc;
        logf_(1, "set_column_address %u..%u", sc, ec);
        break;
    }
    case 0x2B: {                                       /* set_page_address */
        uint16_t sp = p16(0), ep = p16(2);
        if (sp > ep) { warn_("set_page_address con SP>EP (%u>%u)", sp, ep); }
        S.pub.page_start = sp; S.pub.page_end = ep;
        S.pub.cy = sp;
        logf_(1, "set_page_address %u..%u", sp, ep);
        break;
    }
    case 0x2C:                                         /* write_memory_start */
        S.pub.cx = S.pub.col_start; S.pub.cy = S.pub.page_start;
        S.pn = 0; S.streaming = 1; S.reading = 0;
        logf_(1, "write_memory_start (finestra %u..%u x %u..%u)",
              S.pub.col_start, S.pub.col_end, S.pub.page_start, S.pub.page_end);
        break;
    case 0x3C:                                         /* write_memory_continue */
        S.pn = 0; S.streaming = 1; S.reading = 0;
        break;
    case 0x2E:                                         /* read_memory_start */
        S.pub.cx = S.pub.col_start; S.pub.cy = S.pub.page_start;
        S.pn = 0; S.reading = 1; S.streaming = 0; S.rdlen = S.rdpos = 0;
        break;
    case 0x3E:                                         /* read_memory_continue */
        S.pn = 0; S.reading = 1; S.streaming = 0; S.rdlen = S.rdpos = 0;
        break;

    case 0x30: S.part_start = p16(0); S.part_end = p16(2); touch_frame(); break;
    case 0x33:                                         /* set_scroll_area */
        S.sc_tfa = p16(0); S.sc_vsa = p16(2); S.sc_bfa = p16(4);
        if ((unsigned)S.sc_tfa + S.sc_vsa + S.sc_bfa != S.pub.h)
            warn_("set_scroll_area: TFA+VSA+BFA=%u invece di %u",
                  (unsigned)S.sc_tfa + S.sc_vsa + S.sc_bfa, S.pub.h);
        touch_frame();
        break;
    case 0x34: S.pub.tear_on = 0; break;
    case 0x35: S.pub.tear_on = 1; S.tear_mode = S.param[0]; break;
    case 0x36:
        S.pub.addr_mode = S.param[0];
        logf_(1, "set_address_mode 0x%02X%s%s%s%s", S.param[0],
              (S.param[0] & 0x80) ? " flipY" : "", (S.param[0] & 0x40) ? " flipX" : "",
              (S.param[0] & 0x20) ? " swapXY" : "", (S.param[0] & 0x08) ? " BGR" : "");
        touch_frame();
        break;
    case 0x37: S.sc_vsp = p16(0); touch_frame(); break;
    case 0x38: S.pub.idle = 0; touch_frame(); break;
    case 0x39: S.pub.idle = 1; touch_frame(); break;

    case 0xA1: {                                       /* read_ddb */
        static const uint8_t ddb[5] = { 0x01, 0x57, 0x61, 0x01, 0xFF };
        rd_set(ddb, 5);
        break;
    }
    case 0xB0:                                         /* set_lcd_mode */
        memcpy(S.lcd_mode, S.param, 7);
        resize((unsigned)(((S.param[2] & 0x07) << 8) | S.param[3]) + 1u,
               (unsigned)(((S.param[4] & 0x07) << 8) | S.param[5]) + 1u);
        break;
    case 0xB1: rd_set(S.lcd_mode, 7); break;
    case 0xB4: memcpy(S.hori, S.param, 8); break;
    case 0xB5: rd_set(S.hori, 8); break;
    case 0xB6: memcpy(S.vert, S.param, 7); break;
    case 0xB7: rd_set(S.vert, 7); break;
    case 0xB8: memcpy(S.gpio_conf, S.param, 2); break;
    case 0xB9: rd_set(S.gpio_conf, 2); break;
    case 0xBA: S.gpio_val = S.param[0]; break;
    case 0xBB: rd_set1(S.gpio_val); break;
    case 0xBC: memcpy(S.post_proc, S.param, 4); touch_frame(); break;
    case 0xBD: rd_set(S.post_proc, 4); break;
    case 0xBE:                                         /* set_pwm_conf */
        memcpy(S.pwm_conf, S.param, 6);
        /* param[1] = duty del PWM (0..255), param[2] bit0 = PWM abilitato */
        S.pub.backlight = (S.pwm_conf[2] & 0x01) ? S.pwm_conf[1] : 0u;
        logf_(1, "set_pwm_conf: backlight %u/255 (%s)", S.pub.backlight,
              (S.pwm_conf[2] & 0x01) ? "on" : "off");
        touch_frame();
        break;
    case 0xBF: rd_set(S.pwm_conf, 6); break;
    case 0xD0: S.dbc_conf = S.param[0]; break;
    case 0xD1: rd_set1(S.dbc_conf); break;
    case 0xD4: memcpy(S.dbc_th, S.param, 9); break;
    case 0xD5: rd_set(S.dbc_th, 9); break;
    case 0xE0: S.pll = S.param[0]; break;
    case 0xE2: memcpy(S.pll_mn, S.param, 3); break;
    case 0xE4: rd_set(S.pll_mn, 3); break;
    case 0xE5: rd_set1((uint8_t)((S.pll & 0x01) ? 0x04 : 0x00)); break; /* PLL locked */
    case 0xE6: memcpy(S.lshift, S.param, 3); break;
    case 0xE7: rd_set(S.lshift, 3); break;
    case 0xF0:                                         /* set_pixel_data_interface */
        S.pub.pix_fmt = S.param[0] & 0x07;
        S.pn = 0;
        logf_(1, "set_pixel_data_interface %d%s", S.pub.pix_fmt,
              S.pub.pix_fmt == SSD_PIX_16_565 ? " (16 bit 565)" : "");
        break;
    case 0xF1: rd_set1((uint8_t)S.pub.pix_fmt); break;

    default:
        if (c >= 0xC0 && c <= 0xCF) break;             /* lcd_gen / gpio_rop */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Interfaccia bus                                                     */
/* ------------------------------------------------------------------ */
void ssd1963_write_cmd(uint32_t v, int width)
{
    uint8_t c = (uint8_t)(v & 0xFF);
    if (width == 16 && (v & 0xFF00))
        warn_("comando 0x%04X: in modo 16 bit l'SSD1963 guarda solo D7-D0", v);

    const cmdent_t *e = cmd_find(c);
    if (!e) {
        warn_("comando 0x%02X sconosciuto (ignorato)", c);
        S.cmd = 0x00; S.nparam = 0; S.pcount = 0;
        S.streaming = S.reading = 0;
        return;
    }
    logf_(2, "cmd 0x%02X %s", c, e->name);

    S.cmd = c;
    S.pcount = 0;
    S.streaming = 0;
    S.reading = 0;
    S.rdlen = S.rdpos = 0;
    S.pn = 0;
    S.nparam = (e->np == STREAM) ? STREAM : e->np;

    if (e->np == STREAM) {                /* write_memory_*: dati subito dopo */
        exec_cmd();
    } else if (e->np == 0) {
        exec_cmd();                       /* nessun parametro: esegui ora     */
    }
}

void ssd1963_write_data(uint32_t v, int width)
{
    if (S.streaming) {
        if (width == 16) feed_word((uint16_t)v);
        else             feed_byte((uint8_t)v);
        return;
    }
    if (S.nparam == STREAM) return;       /* non dovrebbe capitare */

    if (S.pcount >= S.nparam) {
        warn_("parametro in eccesso per il comando 0x%02X (ne vuole %d)",
              S.cmd, S.nparam);
        return;
    }
    if (width == 16 && (v & 0xFF00))
        warn_("parametro 0x%04X del comando 0x%02X: valgono solo D7-D0",
              v, S.cmd);
    S.param[S.pcount++] = (uint8_t)(v & 0xFF);
    if (S.pcount == S.nparam) exec_cmd();
}

uint32_t ssd1963_read_data(int width)
{
    if (S.reading) return drain_pixel(width);
    if (S.rdpos < S.rdlen) return S.rdbuf[S.rdpos++];
    /* lettura oltre i byte previsti: il chip pilota 0 */
    return 0;
}

uint32_t ssd1963_read_cmd(int width)
{
    (void)width;
    warn_("lettura con D/C#=0: sull'8080 non esiste, ritorno 0");
    return 0;
}

const ssd1963_state_t *ssd1963_state(void) { return &S.pub; }
void ssd1963_set_log(int level) { S.log = level; }

/* ------------------------------------------------------------------ */
/* Composizione dell'immagine visibile                                 */
/* ------------------------------------------------------------------ */
static unsigned scroll_src_line(unsigned l)
{
    unsigned tfa = S.sc_tfa, vsa = S.sc_vsa, bfa = S.sc_bfa;
    if (vsa == 0 || (unsigned)tfa + vsa + bfa != S.pub.h) return l;  /* scroll non valido */
    if (l < tfa || l >= tfa + vsa) return l;                          /* aree fisse */
    unsigned vsp = S.sc_vsp;
    unsigned off = (vsp >= tfa) ? (vsp - tfa) : 0;
    return tfa + ((l - tfa) + off) % vsa;
}

void ssd1963_compose(uint16_t *dst, int stride)
{
    unsigned w = S.pub.w, h = S.pub.h;
    int blank = !S.pub.display_on || S.pub.sleep;

    for (unsigned y = 0; y < h; y++) {
        uint16_t *d = dst + (size_t)y * (size_t)stride;
        if (blank) { memset(d, 0, w * sizeof *d); continue; }
        const uint16_t *s = &S.fb[(size_t)scroll_src_line(y) * SSD_W_MAX];
        if (!S.pub.invert && !S.pub.idle) {
            memcpy(d, s, w * sizeof *d);
            continue;
        }
        for (unsigned x = 0; x < w; x++) {
            uint16_t c = s[x];
            if (S.pub.idle) {
                /* idle mode: 8 colori, 1 bit per canale */
                c = (uint16_t)(((c & 0x8000) ? 0xF800 : 0) |
                               ((c & 0x0400) ? 0x07E0 : 0) |
                               ((c & 0x0010) ? 0x001F : 0));
            }
            if (S.pub.invert) c = (uint16_t)~c;
            d[x] = c;
        }
    }
}

int ssd1963_save_ppm(const char *path)
{
    unsigned w = S.pub.w, h = S.pub.h;
    uint16_t *img = malloc((size_t)w * h * sizeof *img);
    if (!img) return -1;
    ssd1963_compose(img, (int)w);

    FILE *f = fopen(path, "wb");
    if (!f) { free(img); return -1; }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint16_t c = img[i];
        unsigned r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
        uint8_t px[3] = { (uint8_t)((r * 255 + 15) / 31),
                          (uint8_t)((g * 255 + 31) / 63),
                          (uint8_t)((b * 255 + 15) / 31) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    free(img);
    return 0;
}

/* ------------------------------------------------------------------ */
void ssd1963_reset(void)
{
    int log = S.log;
    uint16_t w = S.pub.w ? S.pub.w : SSD_W_DEFAULT;
    uint16_t h = S.pub.h ? S.pub.h : SSD_H_DEFAULT;
    unsigned seq = S.pub.frame_seq;
    /* contatori: statistiche dell'harness, non stato del chip */
    unsigned long long cmds = S.pub.cmds;
    uint32_t px = S.pub.px_written;

    /* il frame buffer non viene azzerato da un soft reset: sul ferro resta
     * quello che c'era (contenuto indefinito all'accensione). */
    memset(&S.pub, 0, sizeof S.pub);
    S.cmd = 0; S.nparam = 0; S.pcount = 0; S.streaming = 0; S.reading = 0;
    S.pn = 0; S.rdlen = 0; S.rdpos = 0;
    S.log = log;

    S.pub.w = w; S.pub.h = h;
    S.pub.frame_seq = seq + 1;
    S.pub.cmds = cmds;
    S.pub.px_written = px;
    S.pub.display_on = 0;            /* dopo il reset il display e' spento   */
    S.pub.pix_fmt = SSD_PIX_16_565;  /* 16 bit 565: il nostro pannello       */
    S.pub.addr_mode = 0x00;
    S.pub.backlight = 255;           /* PWM non ancora programmato: retro on */
    window_reset();

    S.sc_tfa = 0; S.sc_vsa = h; S.sc_bfa = 0; S.sc_vsp = 0;
    S.part_start = 0; S.part_end = (uint16_t)(h - 1);
    memset(S.lcd_mode, 0, sizeof S.lcd_mode);
    memset(S.pwm_conf, 0, sizeof S.pwm_conf);
    S.pwm_conf[1] = 255;
    S.post_proc[0] = 0x40; S.post_proc[1] = 0x80; S.post_proc[2] = 0x40;
    S.post_proc[3] = 0x01;
}

void ssd1963_init(void)
{
    memset(&S, 0, sizeof S);
    S.pub.w = SSD_W_DEFAULT;
    S.pub.h = SSD_H_DEFAULT;
    ssd1963_reset();
    /* contenuto del frame buffer all'accensione: rumore, come la SRAM.
     * Cosi' si vede subito se il firmware non pulisce lo schermo. */
    uint32_t x = 0x2468ACEu;
    for (size_t i = 0; i < SSD_FB_PIXELS; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        S.fb[i] = (uint16_t)x;
    }
}
