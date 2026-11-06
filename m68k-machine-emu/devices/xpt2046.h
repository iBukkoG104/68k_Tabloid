/*
 * xpt2046.h - modello del controller touch resistivo XPT2046 (compatibile
 * ADS7846/TSC2046) del PDA M68K.
 *
 * Il chip e' SPI; sul bus del 68010 e' raggiunto da una porta a 8 bit
 * ($E00300-$E003FF, A0=1 => $E00301) dietro a un master SPI/shift register.
 * Il modello e' a livello di bit, quindi funziona con tutte le sequenze
 * usate dai driver reali:
 *
 *   write ctrl; read; read                     (3 cicli di bus)
 *   write ctrl; write 0; write 0 + latch       (modo "latch")
 *   write ctrl; xfer16(ctrl successivo)        (pipeline a 16 clock)
 *
 * Il byte di controllo e' S A2 A1 A0 MODE SER/DFR# PD1 PD0; dopo gli 8 bit
 * il chip accoda in uscita 1 bit di BUSY + 12 bit di dato (MSB first) + 3
 * bit a zero, cioe' esattamente i 16 bit che i driver leggono e poi
 * scalano di 3.
 */
#ifndef XPT2046_H
#define XPT2046_H

#include <stdint.h>

/* Modo di clocking della porta (come e' fatto il glue logic sulla scheda) */
enum {
    XPT_CLK_RW    = 0,   /* default: ogni accesso (R o W) = 1 trasferimento  */
    XPT_CLK_WONLY = 1    /* solo le scritture clockano; la lettura ritorna
                            il byte ricevuto durante l'ultima scrittura      */
};

typedef struct {
    int      pen_down;
    unsigned px, py;         /* ultima posizione in pixel del pannello      */
    unsigned x_adc, y_adc;   /* valori ADC corrispondenti                   */
    unsigned z1, z2;
    int      penirq;         /* 1 = /PENIRQ asserito                        */
    int      penirq_en;      /* abilitato dai bit PD del byte di controllo   */
    unsigned long long xfers;
    uint8_t  last_ctrl;
} xpt2046_state_t;

void xpt2046_init(void);
void xpt2046_reset(void);

/* Un trasferimento SPI full duplex a 8 bit: ritorna il byte su MISO. */
uint8_t xpt2046_xfer(uint8_t mosi);

/* Accessi dal bus, con il modo di clocking scelto. */
uint8_t xpt2046_bus_read(void);
void    xpt2046_bus_write(uint8_t v);
void    xpt2046_set_clock_mode(int mode);

/* Stato del touch: down=0/1, posizione in pixel sul pannello w x h. */
void xpt2046_set_touch(int down, int px, int py, unsigned w, unsigned h);

int  xpt2046_penirq(void);

/* Calibrazione del pannello resistivo simulato. */
void xpt2046_set_cal(int xmin, int xmax, int ymin, int ymax,
                     int swap_xy, int inv_x, int inv_y);
void xpt2046_set_noise(int lsb);
void xpt2046_set_log(int level);

const xpt2046_state_t *xpt2046_state(void);

#endif
