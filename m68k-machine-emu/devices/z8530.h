/*
 * z8530.h - modello dello Zilog Z8530 SCC del PDA M68K.
 *
 * Canale A: console (RS232 generica).
 * Canale B: ESP32 con ZiModem.
 *
 * Sul bus del 68010 occupa la pagina $E00000-$E000FF con A2 = A/B# e
 * A1 = D/C# (A0 non conta), cioe':
 *
 *   $E00001  canale B, controllo      $E00005  canale A, controllo
 *   $E00003  canale B, dati           $E00007  canale A, dati
 *
 * Cosa c'e' di vero rispetto allo stub precedente:
 *   - tempi di trasmissione e ricezione al baud rate programmato
 *     (WR4 clock mode, WR11 sorgente del clock, WR12/13 time constant,
 *     WR14 abilitazione del baud rate generator);
 *   - Tx Buffer Empty (RR0 D2) che si azzera davvero quando il buffer e'
 *     pieno, e All Sent (RR1 D0);
 *   - FIFO di ricezione da 3 byte come il chip vero, con Rx Overrun;
 *   - interrupt Rx / Tx / External-Status con latch IP e IUS, catena di
 *     priorita' interna, vettore modificato dallo stato (WR9 VIS) e
 *     comandi Reset Tx Int Pending / Reset Highest IUS / Error Reset;
 *   - RR2 con lo stato sul canale B, RR3 solo sul canale A.
 */
#ifndef Z8530_H
#define Z8530_H

#include <stdint.h>

enum { SCC_B = 0, SCC_A = 1 };

typedef struct {
    uint32_t baud;              /* 0 = clock non programmato */
    uint32_t char_tenths;       /* bit per carattere, in decimi */
    int      rx_avail;          /* byte nella FIFO del chip */
    int      tx_ready;          /* RR0 D2 */
    int      all_sent;          /* RR1 D0 */
    int      dcd, cts;
    unsigned long long rx_bytes, tx_bytes, overruns, lost_tx;
} z8530_info_t;

void z8530_init(uint32_t cpu_hz, uint32_t pclk_hz);
void z8530_reset(void);
void z8530_set_clock(uint32_t pclk_hz);
void z8530_set_log(int level);

/* Accessi dal bus: off = offset nella pagina MMIO, now = cicli di CPU. */
uint8_t z8530_read (uint32_t off, uint64_t now);
void    z8530_write(uint32_t off, uint8_t v, uint64_t now);

/* Avanzamento del tempo. z8530_next_event dice fra quanti cicli succedera'
 * qualcosa (serve all'harness per non mancare il momento buono). */
void     z8530_tick(uint64_t now);
uint64_t z8530_next_event(uint64_t now);

int z8530_irq(void);        /* 1 = /INT asserito verso la CPU            */
int z8530_iack(void);       /* riconosce l'interrupt: vettore, -1 se NV  */

/* Collegamento con il mondo esterno. */
void z8530_set_tx_sink(int ch, void (*fn)(uint8_t c));
int  z8530_rx_push(int ch, uint8_t c, uint64_t now);   /* 0 = coda piena */
int  z8530_rx_room(int ch);
void z8530_set_dcd(int ch, int on, uint64_t now);
void z8530_set_cts(int ch, int on, uint64_t now);

const z8530_info_t *z8530_info(int ch);

/* Autotest del modello (baud rate, FIFO, interrupt). 0 = tutto a posto. */
int z8530_selftest(void);

#endif
