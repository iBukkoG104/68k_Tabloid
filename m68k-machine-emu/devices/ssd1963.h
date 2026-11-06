/*
 * ssd1963.h - modello del controller display SSD1963 del PDA M68K.
 *
 * Pannello: TFT 7" 800x480, RGB565, interfaccia host 8080 parallela a 16 bit.
 * Il chip e' cablato come periferica a 16 bit (D0-D15 CPU <-> D0-D15 SSD1963),
 * con A1 = D/C#. Comandi e parametri viaggiano su D7-D0 (come sul ferro vero:
 * in modo 16 bit l'SSD1963 prende comando/parametro dal byte basso), i dati
 * pixel usano tutte e 16 le linee quando il formato e' 16 bit 565.
 *
 * Il modulo non dipende da SDL: tiene solo il frame buffer e lo stato del
 * chip. La finestra la disegna panel.c leggendo ssd1963_compose().
 */
#ifndef SSD1963_H
#define SSD1963_H

#include <stdint.h>

#define SSD_W_DEFAULT  800u
#define SSD_H_DEFAULT  480u
#define SSD_W_MAX      864u                /* limiti del chip reale */
#define SSD_H_MAX      640u
#define SSD_FB_PIXELS  (SSD_W_MAX * SSD_H_MAX)

/* Formati di set_pixel_data_interface (0xF0) */
enum {
    SSD_PIX_8      = 0x00,                 /* 8 bit:  3 trasferimenti = 1 px (888) */
    SSD_PIX_12     = 0x01,                 /* 12 bit: 444 impaccato              */
    SSD_PIX_16PACK = 0x02,                 /* 16 bit impaccato: 3 word = 2 px    */
    SSD_PIX_16_565 = 0x03,                 /* 16 bit 565: 1 word = 1 px (default)*/
    SSD_PIX_18     = 0x04,
    SSD_PIX_24     = 0x05,
    SSD_PIX_9      = 0x06
};

/* Stato visibile dall'esterno (sola lettura). */
typedef struct {
    uint16_t w, h;              /* dimensione pannello attiva (HDP+1, VDP+1) */
    unsigned frame_seq;         /* cambia quando l'immagine visibile cambia  */
    int   display_on;
    int   sleep, idle, partial, invert, tear_on;
    int   pix_fmt;
    uint8_t addr_mode;
    unsigned backlight;         /* 0..255, da set_pwm_conf (0xBE)            */
    uint16_t col_start, col_end, page_start, page_end;
    uint16_t cx, cy;            /* posizione corrente nella finestra         */
    uint32_t px_written;        /* pixel scritti in totale (statistica)      */
    unsigned long long cmds;    /* comandi ricevuti                          */
} ssd1963_state_t;

void ssd1963_init(void);
void ssd1963_reset(void);                  /* come il pin RESET#            */

/* Accessi dal bus. width e' 8 o 16 (bit effettivamente pilotati). */
void     ssd1963_write_cmd (uint32_t v, int width);
void     ssd1963_write_data(uint32_t v, int width);
uint32_t ssd1963_read_data (int width);
uint32_t ssd1963_read_cmd  (int width);    /* non esiste sul ferro: ritorna 0 */

const ssd1963_state_t *ssd1963_state(void);
void ssd1963_set_log(int level);           /* 0 = muto, 1 = comandi, 2 = +dati */

/* Compone l'immagine visibile (scroll hw, invert, idle, display off) in
 * dst, w*h pixel RGB565, stride in pixel. */
void ssd1963_compose(uint16_t *dst, int stride);

/* Salva l'immagine visibile come PPM (P6). 0 = ok. */
int  ssd1963_save_ppm(const char *path);

#endif
