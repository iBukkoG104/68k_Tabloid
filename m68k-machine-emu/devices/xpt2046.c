/*
 * xpt2046.c - emulazione dell'XPT2046 a livello di bit SPI.
 */
#include "xpt2046.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    xpt2046_state_t pub;

    /* ricezione del byte di controllo */
    int      in_ctrl;
    uint8_t  ctrl;
    int      ctrl_bits;

    /* coda dei bit in uscita: il prossimo bit e' quello a (outn-1) */
    uint64_t outq;
    int      outn;

    int      converting;        /* dal primo bit del controllo alla fine dati */
    int      clk_mode;
    uint8_t  latch;             /* ultimo byte ricevuto (modo WONLY)          */

    /* calibrazione */
    int      xmin, xmax, ymin, ymax, swap_xy, inv_x, inv_y;
    int      noise;
    uint32_t rnd;

    int      log;
} xpt_t;

static xpt_t X;

static unsigned nextrnd(void)
{
    X.rnd ^= X.rnd << 13; X.rnd ^= X.rnd >> 17; X.rnd ^= X.rnd << 5;
    return X.rnd;
}

static int jitter(void)
{
    if (X.noise <= 0) return 0;
    return (int)(nextrnd() % (unsigned)(2 * X.noise + 1)) - X.noise;
}

static unsigned clamp12(int v)
{
    if (v < 0) return 0;
    if (v > 4095) return 4095;
    return (unsigned)v;
}

/* ------------------------------------------------------------------ */
/* Conversioni                                                         */
/* ------------------------------------------------------------------ */
/* Canali in modo differenziale (SER/DFR#=0): A2..A0
 *   001 = Y, 011 = Z1, 100 = Z2, 101 = X
 * In modo single-ended (SER/DFR#=1):
 *   000/110 = temperatura, 010 = VBAT, 100 = AUX
 */
static unsigned convert(uint8_t ctrl)
{
    int chan = (ctrl >> 4) & 7;
    int ser  = (ctrl >> 2) & 1;
    int down = X.pub.pen_down;

    if (ser) {
        switch (chan) {
        case 0: return 1500;                    /* TEMP0 (~25 C)            */
        case 6: return 1560;                    /* TEMP1                    */
        case 2: return clamp12(2400 + jitter()); /* VBAT: ~3.9 V / 4        */
        case 4: return clamp12(2048 + jitter()); /* AUX                     */
        default: break;
        }
    }
    switch (chan) {
    case 1: return down ? clamp12((int)X.pub.y_adc + jitter()) : 0;   /* Y  */
    case 5: return down ? clamp12((int)X.pub.x_adc + jitter()) : 0;   /* X  */
    case 3: return down ? clamp12((int)X.pub.z1 + jitter()) : 0;      /* Z1 */
    case 4: return down ? clamp12((int)X.pub.z2 + jitter()) : 4095;   /* Z2 */
    default: return 0;
    }
}

static void enqueue(uint16_t frame16)
{
    if (X.outn > 48) {              /* non dovrebbe mai servire */
        X.outq = 0; X.outn = 0;
    }
    X.outq = (X.outq << 16) | frame16;
    X.outn += 16;
}

static void start_conversion(uint8_t ctrl)
{
    unsigned v = convert(ctrl);
    int eight = (ctrl >> 3) & 1;    /* MODE: 1 = 8 bit, 0 = 12 bit */
    uint16_t frame;

    X.pub.last_ctrl = ctrl;
    /* PENIRQ abilitato solo con PD1=PD0=0 */
    X.pub.penirq_en = ((ctrl & 0x03) == 0);

    if (eight) frame = (uint16_t)(((v >> 4) & 0xFF) << 7);   /* BUSY + 8 bit  */
    else       frame = (uint16_t)((v & 0x0FFF) << 3);        /* BUSY + 12 bit */
    enqueue(frame);

    if (X.log >= 2)
        fprintf(stderr, "[XPT2046] ctrl=0x%02X -> %u (%s)\n", ctrl, v,
                eight ? "8 bit" : "12 bit");
}

/* Un bit di clock SPI: in mosi, out miso. */
static int spi_bit(int mosi)
{
    int miso = 0;
    if (X.outn > 0) {
        miso = (int)((X.outq >> (X.outn - 1)) & 1u);
        X.outn--;
        if (X.outn == 0 && !X.in_ctrl) X.converting = 0;
    }

    if (!X.in_ctrl) {
        if (mosi) {                         /* bit di start */
            X.in_ctrl = 1;
            X.ctrl = 1;
            X.ctrl_bits = 1;
            X.converting = 1;
        }
    } else {
        X.ctrl = (uint8_t)((X.ctrl << 1) | (mosi & 1));
        if (++X.ctrl_bits == 8) {
            X.in_ctrl = 0;
            start_conversion(X.ctrl);
        }
    }
    return miso;
}

uint8_t xpt2046_xfer(uint8_t mosi)
{
    uint8_t miso = 0;
    for (int i = 7; i >= 0; i--)
        miso = (uint8_t)((miso << 1) | spi_bit((mosi >> i) & 1));
    X.pub.xfers++;
    X.latch = miso;
    return miso;
}

/* ------------------------------------------------------------------ */
/* Interfaccia bus                                                     */
/* ------------------------------------------------------------------ */
void xpt2046_set_clock_mode(int mode) { X.clk_mode = mode; }

uint8_t xpt2046_bus_read(void)
{
    if (X.clk_mode == XPT_CLK_WONLY) return X.latch;
    /* MOSI a 0 durante la lettura: nessun bit di start spurio */
    return xpt2046_xfer(0x00);
}

void xpt2046_bus_write(uint8_t v)
{
    if (X.log >= 1 && (v & 0x80))
        fprintf(stderr, "[XPT2046] SPI out 0x%02X\n", v);
    xpt2046_xfer(v);
}

/* ------------------------------------------------------------------ */
/* Pannello resistivo                                                  */
/* ------------------------------------------------------------------ */
void xpt2046_set_touch(int down, int px, int py, unsigned w, unsigned h)
{
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if ((unsigned)px >= w) px = (int)w - 1;
    if ((unsigned)py >= h) py = (int)h - 1;

    int wasdown = X.pub.pen_down;
    X.pub.pen_down = down ? 1 : 0;
    X.pub.px = (unsigned)px;
    X.pub.py = (unsigned)py;

    int ax = px, ay = py;
    if (X.swap_xy) { int t = ax; ax = ay; ay = t; }
    int span_x = (int)(X.swap_xy ? h : w) - 1;
    int span_y = (int)(X.swap_xy ? w : h) - 1;
    if (X.inv_x) ax = span_x - ax;
    if (X.inv_y) ay = span_y - ay;

    X.pub.x_adc = clamp12(X.xmin + (X.xmax - X.xmin) * ax / (span_x ? span_x : 1));
    X.pub.y_adc = clamp12(X.ymin + (X.ymax - X.ymin) * ay / (span_y ? span_y : 1));

    if (down) {
        /* pressione: Z1 cresce e Z2 cala quando si preme. Valori tipici di
         * un pannello 7" con Rx-plate ~ 400 ohm. */
        X.pub.z1 = clamp12(420 + jitter() * 4);
        X.pub.z2 = clamp12(3550 + jitter() * 4);
    } else {
        X.pub.z1 = 0;
        X.pub.z2 = 4095;
    }

    if (down && !wasdown && X.log >= 1)
        fprintf(stderr, "[XPT2046] pen down @ %d,%d -> ADC x=%u y=%u\n",
                px, py, X.pub.x_adc, X.pub.y_adc);
}

int xpt2046_penirq(void)
{
    X.pub.penirq = (X.pub.pen_down && X.pub.penirq_en && !X.converting) ? 1 : 0;
    return X.pub.penirq;
}

void xpt2046_set_cal(int xmin, int xmax, int ymin, int ymax,
                     int swap_xy, int inv_x, int inv_y)
{
    X.xmin = xmin; X.xmax = xmax; X.ymin = ymin; X.ymax = ymax;
    X.swap_xy = swap_xy; X.inv_x = inv_x; X.inv_y = inv_y;
}

void xpt2046_set_noise(int lsb) { X.noise = lsb; }
void xpt2046_set_log(int level) { X.log = level; }
const xpt2046_state_t *xpt2046_state(void) { return &X.pub; }

void xpt2046_reset(void)
{
    X.in_ctrl = 0; X.ctrl = 0; X.ctrl_bits = 0;
    X.outq = 0; X.outn = 0; X.converting = 0; X.latch = 0;
    X.pub.penirq_en = 1;
    X.pub.z2 = 4095;
}

void xpt2046_init(void)
{
    int log = X.log, clk = X.clk_mode;
    memset(&X, 0, sizeof X);
    X.log = log; X.clk_mode = clk;
    X.rnd = 0x13579BDFu;
    X.noise = 3;
    /* pannello "ideale": entrambi gli assi crescono con le coordinate del
     * display. Se il tuo pannello e' montato ruotato o specchiato, usa le
     * opzioni di calibrazione dell'harness. */
    xpt2046_set_cal(200, 3900, 200, 3900, 0, 0, 0);
    xpt2046_reset();
}
