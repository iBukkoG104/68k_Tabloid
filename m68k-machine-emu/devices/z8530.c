/*
 * z8530.c - Zilog Z8530 SCC: tempi al baud rate e interrupt.
 *
 * Semplificazioni dichiarate:
 *   - solo asincrono: niente SDLC/HDLC, CRC, DPLL, loop mode, DMA/wait;
 *   - parita' e framing non vengono mai generati (nessun rumore sulla
 *     linea), quindi i bit di errore in RR1 restano a 0 tranne l'overrun;
 *   - RR0 D1 (Zero Count del baud rate generator) resta 0;
 *   - il canale usa un'unica velocita' per Tx e Rx se WR11 indica la
 *     stessa sorgente, ma le due sono calcolate separatamente;
 *   - fra l'host e la FIFO del chip c'e' una coda di transito che fa da
 *     "filo": i byte entrano nella FIFO uno per tempo di carattere.
 */
#include "z8530.h"

#include <stdio.h>
#include <string.h>

#define STAGE_SZ 4096u              /* il "filo": byte in viaggio          */
#define FIFO_SZ  3                  /* la FIFO di ricezione del chip vero  */

/* Sorgenti di interrupt, dalla priorita' piu' alta alla piu' bassa. */
enum { S_ARX, S_ATX, S_AEXT, S_BRX, S_BTX, S_BEXT, S_N };

typedef struct {
    uint8_t  wr[16];
    uint16_t wr_seen;               /* quali WR il firmware ha scritto     */
    int      ptr;                   /* puntatore registri (per canale)     */

    uint8_t  stage[STAGE_SZ];
    unsigned sh, st;
    uint64_t rx_next;               /* quando il prossimo byte entra in FIFO */

    uint8_t  fifo[FIFO_SZ];
    int      fifo_n;

    uint8_t  tx_buf;
    int      tx_full;
    uint64_t tx_free;               /* quando lo shift register si libera  */

    int ip_rx, ip_tx, ip_ext;
    int ius_rx, ius_tx, ius_ext;
    int rx_first_armed;
    int rx_special;

    int dcd, cts, brk;
    int dcd_seen, cts_seen;         /* valori gia' notificati              */
    int ext_latched;
    uint8_t rr0_latch;

    int err_overrun, err_parity, err_framing;

    void (*sink)(uint8_t);
    z8530_info_t info;
} chan_t;

static chan_t   C[2];               /* [0] = canale B, [1] = canale A */
static uint8_t  wr2, wr9;           /* registri condivisi fra i canali */
static uint32_t cpu_hz = 10000000u;
static uint32_t pclk   = 3686400u;  /* cristallo classico da baud rate */
static int      logv;
static int      warned_unconfigured;

static const char *cname(const chan_t *c) { return c == &C[1] ? "A" : "B"; }

/* ------------------------------------------------------------------ */
/* Velocita' e durata di un carattere                                  */
/* ------------------------------------------------------------------ */
static unsigned clock_div(const chan_t *c)
{
    switch ((c->wr[4] >> 6) & 3) {
    case 0: return 1;  case 1: return 16;
    case 2: return 32; default: return 64;
    }
}

/* Decimi di bit per carattere: start + dati + parita' + stop. */
static unsigned char_tenths(const chan_t *c, int is_tx)
{
    unsigned data;
    unsigned sel = is_tx ? ((c->wr[5] >> 5) & 3) : ((c->wr[3] >> 6) & 3);
    switch (sel) {
    case 0: data = 5; break;  case 1: data = 7; break;
    case 2: data = 6; break;  default: data = 8; break;
    }
    unsigned t = 10 + data * 10;
    if (c->wr[4] & 0x01) t += 10;                 /* parita' abilitata */
    switch ((c->wr[4] >> 2) & 3) {
    case 0: t += 10; break;                       /* sync: non modellato */
    case 1: t += 10; break;
    case 2: t += 15; break;
    default: t += 20; break;
    }
    return t;
}

/* Il firmware ha toccato almeno un registro di controllo del canale?
 * Se no il chip non e' programmato e il modello resta passante, cosi' le
 * ROM che scrivono sul data register e basta continuano a funzionare. */
static int configured(const chan_t *c) { return c->wr_seen != 0; }

static uint32_t baud_rate(const chan_t *c, int is_tx)
{
    if (!configured(c)) return 0;
    unsigned src = is_tx ? ((c->wr[11] >> 3) & 3) : ((c->wr[11] >> 5) & 3);
    unsigned div = clock_div(c);
    uint32_t clk;

    if (src == 2) {                               /* baud rate generator */
        if (!(c->wr[14] & 0x01)) return 0;        /* BRG spento */
        /* WR14 D1 sceglie fra PCLK e RTxC. Sulla scheda i due arrivano
         * dallo stesso cristallo, quindi qui valgono uguale. */
        uint32_t base = pclk;
        unsigned tc = ((unsigned)c->wr[13] << 8) | c->wr[12];
        uint32_t out = base / (2u * (tc + 2u));
        clk = out;
    } else if (src == 3) {
        return 0;                                 /* DPLL: non modellato */
    } else {
        clk = pclk;                               /* RTxC/TRxC esterni   */
    }
    return div ? clk / div : 0;
}

/* Cicli di CPU che dura un carattere. 0 = clock non programmato: in quel
 * caso il modello resta "passante", come lo stub di prima, cosi' le ROM
 * che scrivono sul data register senza inizializzare nulla continuano a
 * funzionare. */
static uint64_t char_cycles(const chan_t *c, int is_tx)
{
    uint32_t baud = baud_rate(c, is_tx);
    if (!baud) return 0;
    return (uint64_t)cpu_hz * char_tenths(c, is_tx) / (10ull * baud);
}

/* ------------------------------------------------------------------ */
/* Interrupt: IP, IUS, catena di priorita'                             */
/* ------------------------------------------------------------------ */
static int *ip_of(int s)
{
    switch (s) {
    case S_ARX:  return &C[1].ip_rx;
    case S_ATX:  return &C[1].ip_tx;
    case S_AEXT: return &C[1].ip_ext;
    case S_BRX:  return &C[0].ip_rx;
    case S_BTX:  return &C[0].ip_tx;
    default:     return &C[0].ip_ext;
    }
}

static int *ius_of(int s)
{
    switch (s) {
    case S_ARX:  return &C[1].ius_rx;
    case S_ATX:  return &C[1].ius_tx;
    case S_AEXT: return &C[1].ius_ext;
    case S_BRX:  return &C[0].ius_rx;
    case S_BTX:  return &C[0].ius_tx;
    default:     return &C[0].ius_ext;
    }
}

/* La sorgente che sta chiedendo servizio. Scorrendo in ordine di
 * priorita', il primo IUS incontrato blocca tutto quello che sta sotto:
 * e' la catena interna del chip. */
static int top_pending(void)
{
    for (int s = 0; s < S_N; s++) {
        if (*ius_of(s)) return -1;
        if (*ip_of(s))  return s;
    }
    return -1;
}

/* Codice di stato che finisce nel vettore (WR9 VIS). */
static int vec_status(int s, int special)
{
    switch (s) {
    case S_BTX:  return 0;
    case S_BEXT: return 1;
    case S_BRX:  return special ? 3 : 2;
    case S_ATX:  return 4;
    case S_AEXT: return 5;
    case S_ARX:  return special ? 7 : 6;
    default:     return 3;             /* niente in sospeso */
    }
}

static uint8_t vector_for(int s)
{
    uint8_t v = wr2;
    int special = 0;
    if (!(wr9 & 0x01)) return v;        /* VIS spento: vettore non modificato */
    if (s == S_ARX) special = C[1].rx_special;
    if (s == S_BRX) special = C[0].rx_special;
    int st = vec_status(s, special);
    if (wr9 & 0x10) v = (uint8_t)((v & ~0x70) | (st << 4));   /* status high */
    else            v = (uint8_t)((v & ~0x0E) | (st << 1));   /* status low  */
    return v;
}

int z8530_irq(void)
{
    if (!(wr9 & 0x08)) return 0;        /* MIE spento */
    return top_pending() >= 0;
}

int z8530_iack(void)
{
    int s = top_pending();
    if (s < 0) return -1;
    *ius_of(s) = 1;
    if (logv >= 1)
        fprintf(stderr, "[SCC] IACK sorgente %d, vettore 0x%02X\n", s, vector_for(s));
    if (wr9 & 0x02) return -1;          /* NV: il chip non pilota il vettore */
    return vector_for(s);
}

static void reset_highest_ius(void)
{
    for (int s = 0; s < S_N; s++)
        if (*ius_of(s)) { *ius_of(s) = 0; return; }
}

/* ------------------------------------------------------------------ */
/* Ricezione                                                           */
/* ------------------------------------------------------------------ */
static void rx_int_check(chan_t *c)
{
    int mode = (c->wr[1] >> 3) & 3;
    if (mode == 0) return;
    if (c->err_overrun || c->err_parity || c->err_framing) {
        c->ip_rx = 1; c->rx_special = 1;
        return;
    }
    if (c->fifo_n == 0) return;
    if (mode == 2) c->ip_rx = 1;
    else if (mode == 1 && c->rx_first_armed) { c->ip_rx = 1; c->rx_first_armed = 0; }
}

static void rx_advance(chan_t *c, uint64_t now)
{
    uint64_t cc = char_cycles(c, 0);
    while (c->sh != c->st) {
        if (cc && now < c->rx_next) break;
        uint8_t b = c->stage[c->st++ & (STAGE_SZ - 1)];
        if (c->fifo_n >= FIFO_SZ) {
            /* FIFO piena: il byte nuovo sovrascrive l'ultimo e alza
             * l'overrun, come sul chip vero. */
            c->fifo[FIFO_SZ - 1] = b;
            c->err_overrun = 1;
            c->info.overruns++;
        } else {
            c->fifo[c->fifo_n++] = b;
        }
        c->info.rx_bytes++;
        /* Il filo continua a scorrere al baud rate: i byte seguenti erano
         * gia' in viaggio, non ripartono da adesso. Chi li fa partire e'
         * rx_push, quando il filo era fermo. */
        c->rx_next += cc;
        rx_int_check(c);
    }
}

static uint8_t rx_read(chan_t *c)
{
    uint8_t b = 0;
    if (c->fifo_n > 0) {
        b = c->fifo[0];
        for (int i = 1; i < c->fifo_n; i++) c->fifo[i - 1] = c->fifo[i];
        c->fifo_n--;
    }
    c->ip_rx = 0;
    c->rx_special = 0;
    rx_int_check(c);                 /* se ne restano altri, riarma */
    return b;
}

int z8530_rx_push(int ch, uint8_t c8, uint64_t now)
{
    chan_t *c = &C[ch & 1];
    if ((unsigned)(c->sh - c->st) >= STAGE_SZ) return 0;
    int was_empty = (c->sh == c->st);
    c->stage[c->sh++ & (STAGE_SZ - 1)] = c8;
    uint64_t cc = char_cycles(c, 0);
    if (was_empty && cc) {
        uint64_t base = (c->rx_next > now) ? c->rx_next : now;
        c->rx_next = base + cc;
    }
    rx_advance(c, now);
    return 1;
}

int z8530_rx_room(int ch)
{
    chan_t *c = &C[ch & 1];
    return (int)(STAGE_SZ - (unsigned)(c->sh - c->st));
}

/* ------------------------------------------------------------------ */
/* Trasmissione                                                        */
/* ------------------------------------------------------------------ */
static void tx_emit(chan_t *c, uint8_t v)
{
    c->info.tx_bytes++;
    if (c->sink) c->sink(v);
}

static int tx_enabled(const chan_t *c)
{
    if (!(c->wr_seen & (1u << 5))) return 1;    /* WR5 mai scritto: passante */
    return (c->wr[5] & 0x08) != 0;              /* Tx Enable */
}

static void tx_advance(chan_t *c, uint64_t now)
{
    uint64_t cc = char_cycles(c, 1);
    if (!c->tx_full) return;
    if (cc && now < c->tx_free) return;
    tx_emit(c, c->tx_buf);
    c->tx_full = 0;
    c->tx_free = (cc ? ((c->tx_free > now ? c->tx_free : now) + cc) : now);
    /* Il latch si arma solo se l'interrupt di trasmissione e' abilitato,
     * altrimenti un canale usato a polling (p.es. la console) terrebbe
     * /INT basso per sempre. */
    if (c->wr[1] & 0x02) c->ip_tx = 1;
}

static void tx_write(chan_t *c, uint8_t v, uint64_t now)
{
    c->ip_tx = 0;                               /* scrivere azzera Tx IP */
    if (!tx_enabled(c)) {
        c->info.lost_tx++;
        if (logv >= 1)
            fprintf(stderr, "[SCC %s] scrittura col trasmettitore disabilitato "
                            "(WR5 D3 = 0): byte perso\n", cname(c));
        return;
    }
    if (!char_cycles(c, 1) && !warned_unconfigured++) {
        fprintf(stderr, "[SCC %s] nessun registro di controllo programmato: "
                        "trasmissione istantanea e Tx Buffer Empty sempre a 1.\n"
                        "        Programma WR4/WR11/WR12/WR13/WR14 per avere i "
                        "tempi veri al baud rate.\n", cname(c));
    }
    if (c->tx_full) {
        /* Il firmware ha scritto senza aspettare Tx Buffer Empty: sul ferro
         * il byte precedente si perde. */
        c->info.lost_tx++;
        if (logv >= 1 || c->info.lost_tx <= 3)
            fprintf(stderr, "[SCC %s] scrittura su buffer pieno: byte 0x%02X "
                            "perso (manca il polling di RR0 D2)\n",
                    cname(c), c->tx_buf);
    }
    c->tx_buf = v;
    c->tx_full = 1;
    tx_advance(c, now);                         /* se lo shift e' libero parte subito */
}

/* ------------------------------------------------------------------ */
/* External / Status                                                   */
/* ------------------------------------------------------------------ */
static uint8_t rr0_value(const chan_t *c)
{
    uint8_t v = 0;
    if (c->fifo_n > 0)  v |= 0x01;      /* D0 Rx Character Available */
    if (!c->tx_full)    v |= 0x04;      /* D2 Tx Buffer Empty        */
    if (c->dcd)         v |= 0x08;      /* D3 DCD                    */
    if (c->cts)         v |= 0x20;      /* D5 CTS                    */
    if (c->info.all_sent) v |= 0x40;    /* D6 Tx Underrun/EOM        */
    if (c->brk)         v |= 0x80;      /* D7 Break/Abort            */
    return v;
}

static void ext_check(chan_t *c)
{
    if (c->dcd == c->dcd_seen && c->cts == c->cts_seen && !c->brk) return;
    c->dcd_seen = c->dcd;
    c->cts_seen = c->cts;
    if (c->ext_latched) return;
    c->ext_latched = 1;
    c->rr0_latch = rr0_value(c);
    if (c->wr[1] & 0x01) c->ip_ext = 1;         /* Ext Int Enable */
}

void z8530_set_dcd(int ch, int on, uint64_t now)
{
    chan_t *c = &C[ch & 1];
    if (c->dcd == !!on) return;
    c->dcd = !!on;
    c->info.dcd = c->dcd;
    z8530_tick(now);
    ext_check(c);
}

void z8530_set_cts(int ch, int on, uint64_t now)
{
    chan_t *c = &C[ch & 1];
    if (c->cts == !!on) return;
    c->cts = !!on;
    c->info.cts = c->cts;
    z8530_tick(now);
    ext_check(c);
}

/* ------------------------------------------------------------------ */
/* Registri                                                            */
/* ------------------------------------------------------------------ */
static void chan_reset(chan_t *c)
{
    c->ptr = 0;
    c->tx_full = 0; c->tx_free = 0;
    c->fifo_n = 0; c->sh = c->st = 0; c->rx_next = 0;
    c->ip_rx = c->ip_tx = c->ip_ext = 0;
    c->ius_rx = c->ius_tx = c->ius_ext = 0;
    c->rx_first_armed = 0; c->rx_special = 0;
    c->ext_latched = 0;
    c->err_overrun = c->err_parity = c->err_framing = 0;
    c->wr[1] = 0; c->wr[3] = 0; c->wr[5] = 0;
    c->wr_seen = 0;
}

static void wr9_write(uint8_t v)
{
    wr9 = v;
    switch ((v >> 6) & 3) {
    case 1: chan_reset(&C[0]); break;           /* channel reset B */
    case 2: chan_reset(&C[1]); break;           /* channel reset A */
    case 3: z8530_reset(); break;               /* force hardware reset */
    default: break;
    }
}

static void wr0_write(chan_t *c, uint8_t v)
{
    int reg = v & 7;
    int cmd = (v >> 3) & 7;
    c->wr[0] = v;
    if (cmd == 1) reg |= 8;                     /* point high */
    c->ptr = reg;

    switch (cmd) {
    case 2:                                     /* reset ext/status int */
        c->ext_latched = 0; c->ip_ext = 0;
        break;
    case 4:                                     /* enable int on next rx char */
        c->rx_first_armed = 1;
        break;
    case 5:                                     /* reset tx int pending */
        c->ip_tx = 0;
        break;
    case 6:                                     /* error reset */
        c->err_overrun = c->err_parity = c->err_framing = 0;
        c->rx_special = 0;
        break;
    case 7:
        reset_highest_ius();
        break;
    default:
        break;
    }
}

static void ctrl_write(chan_t *c, uint8_t v, uint64_t now)
{
    if (c->ptr == 0) { wr0_write(c, v); return; }

    int r = c->ptr;
    c->ptr = 0;
    if (logv >= 2)
        fprintf(stderr, "[SCC %s] WR%d <- 0x%02X\n", cname(c), r, v);

    switch (r) {
    case 2: wr2 = v; c->wr[2] = v; break;
    case 9: wr9_write(v); c->wr[9] = v; break;
    case 8: tx_write(c, v, now); break;
    case 1: {
        int was_tx_ie = c->wr[1] & 0x02;
        c->wr[1] = v;
        /* abilitare l'interrupt Tx a buffer vuoto genera subito la
         * richiesta: e' cosi' che i driver fanno partire la trasmissione */
        if (!was_tx_ie && (v & 0x02) && !c->tx_full) c->ip_tx = 1;
        if (((v >> 3) & 3) == 1) c->rx_first_armed = 1;
        if (((v >> 3) & 3) == 0) c->ip_rx = 0;
        break;
    }
    default: c->wr[r] = v; break;
    }
    c->wr_seen |= (uint16_t)(1u << (r & 15));
}

static uint8_t rr1_value(const chan_t *c)
{
    uint8_t v = 0;
    if (c->info.all_sent) v |= 0x01;
    if (c->err_parity)    v |= 0x10;
    if (c->err_overrun)   v |= 0x20;
    if (c->err_framing)   v |= 0x40;
    return v;
}

static uint8_t rr3_value(void)
{
    uint8_t v = 0;
    if (C[1].ip_rx)  v |= 0x20;
    if (C[1].ip_tx)  v |= 0x10;
    if (C[1].ip_ext) v |= 0x08;
    if (C[0].ip_rx)  v |= 0x04;
    if (C[0].ip_tx)  v |= 0x02;
    if (C[0].ip_ext) v |= 0x01;
    return v;
}

static uint8_t ctrl_read(chan_t *c)
{
    int r = c->ptr;
    int is_a = (c == &C[1]);
    c->ptr = 0;
    switch (r) {
    case 0:  return c->ext_latched ? c->rr0_latch : rr0_value(c);
    case 1:  return rr1_value(c);
    case 2:  return is_a ? wr2 : vector_for(top_pending());
    case 3:  return is_a ? rr3_value() : 0x00;   /* RR3 solo sul canale A */
    case 8:  return rx_read(c);
    case 10: return 0x00;
    case 12: return c->wr[12];
    case 13: return c->wr[13];
    case 15: return c->wr[15];
    default: return 0x00;
    }
}

/* ------------------------------------------------------------------ */
/* Tempo                                                               */
/* ------------------------------------------------------------------ */
void z8530_tick(uint64_t now)
{
    for (int i = 0; i < 2; i++) {
        chan_t *c = &C[i];
        tx_advance(c, now);
        rx_advance(c, now);
        uint64_t cc = char_cycles(c, 1);
        c->info.all_sent  = !c->tx_full && (!cc || now >= c->tx_free);
        c->info.tx_ready  = !c->tx_full;
        c->info.rx_avail  = c->fifo_n;
        c->info.baud        = baud_rate(c, 1);
        c->info.char_tenths = char_tenths(c, 1);
    }
}

uint64_t z8530_next_event(uint64_t now)
{
    uint64_t best = ~0ull;
    for (int i = 0; i < 2; i++) {
        chan_t *c = &C[i];
        if (c->tx_full) {
            uint64_t cc = char_cycles(c, 1);
            uint64_t t = (!cc || c->tx_free <= now) ? 0 : c->tx_free - now;
            if (t < best) best = t;
        }
        if (c->sh != c->st) {
            uint64_t cc = char_cycles(c, 0);
            uint64_t t = (!cc || c->rx_next <= now) ? 0 : c->rx_next - now;
            if (t < best) best = t;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* Interfaccia bus                                                     */
/* ------------------------------------------------------------------ */
/* A2 = A/B#, A1 = D/C#, A0 non entra nel decode:
 *   reg 0 = B controllo, 1 = B dati, 2 = A controllo, 3 = A dati */
uint8_t z8530_read(uint32_t off, uint64_t now)
{
    z8530_tick(now);
    int reg = (int)((off >> 1) & 3);
    chan_t *c = &C[reg >> 1];
    uint8_t v = (reg & 1) ? rx_read(c) : ctrl_read(c);
    z8530_tick(now);
    return v;
}

void z8530_write(uint32_t off, uint8_t v, uint64_t now)
{
    z8530_tick(now);
    int reg = (int)((off >> 1) & 3);
    chan_t *c = &C[reg >> 1];
    if (reg & 1) tx_write(c, v, now);
    else         ctrl_write(c, v, now);
    z8530_tick(now);
}

void z8530_set_tx_sink(int ch, void (*fn)(uint8_t)) { C[ch & 1].sink = fn; }
void z8530_set_log(int level) { logv = level; }
void z8530_set_clock(uint32_t hz) { if (hz) pclk = hz; }
const z8530_info_t *z8530_info(int ch) { return &C[ch & 1].info; }

void z8530_reset(void)
{
    void (*sa)(uint8_t) = C[1].sink, (*sb)(uint8_t) = C[0].sink;
    z8530_info_t ia = C[1].info, ib = C[0].info;
    memset(C, 0, sizeof C);
    C[1].sink = sa; C[0].sink = sb;
    C[1].info = ia; C[0].info = ib;
    wr2 = 0; wr9 = 0;
    /* CTS tirato alto: sulla scheda non c'e' controllo di flusso hardware */
    for (int i = 0; i < 2; i++) {
        C[i].cts = 1; C[i].cts_seen = 1; C[i].info.cts = 1;
        C[i].info.all_sent = 1; C[i].info.tx_ready = 1;
    }
}

void z8530_init(uint32_t cpu, uint32_t clk)
{
    memset(C, 0, sizeof C);
    if (cpu) cpu_hz = cpu;
    if (clk) pclk = clk;
    z8530_reset();
}

/* ------------------------------------------------------------------ */
/* Autotest                                                            */
/* ------------------------------------------------------------------ */
#define B_CTRL 1u
#define B_DATA 3u
#define A_CTRL 5u
#define A_DATA 7u

static uint8_t st_out[64];
static int     st_outn;
static void st_sink(uint8_t v) { if (st_outn < 64) st_out[st_outn++] = v; }

static void st_wr(uint32_t ctrl, int r, uint8_t v, uint64_t t)
{
    if (r != 0) z8530_write(ctrl, (uint8_t)((r & 7) | (r >= 8 ? 0x08 : 0)), t);
    z8530_write(ctrl, v, t);
}

static uint8_t st_rd(uint32_t ctrl, int r, uint64_t t)
{
    if (r != 0) z8530_write(ctrl, (uint8_t)((r & 7) | (r >= 8 ? 0x08 : 0)), t);
    return z8530_read(ctrl, t);
}

static int st_bad;
static void ck(int cond, const char *what, long got, long want)
{
    if (cond) return;
    fprintf(stderr, "[SCC] FAIL %s: ho %ld, mi aspetto %ld\n", what, got, want);
    st_bad++;
}

/* Programma il canale a 9600 8N1 con il baud rate generator. */
static void st_setup(uint32_t ctrl, uint64_t t)
{
    st_wr(ctrl, 4,  0x44, t);      /* x16, 1 stop bit, niente parita'  */
    st_wr(ctrl, 3,  0xC1, t);      /* Rx 8 bit, Rx enable              */
    st_wr(ctrl, 5,  0x68, t);      /* Tx 8 bit, Tx enable              */
    st_wr(ctrl, 11, 0x50, t);      /* Tx e Rx clock dal BRG            */
    st_wr(ctrl, 12, 10,   t);      /* time constant: 9600 a 3.6864 MHz */
    st_wr(ctrl, 13, 0,    t);
    st_wr(ctrl, 14, 0x03, t);      /* BRG acceso, sorgente PCLK        */
}

int z8530_selftest(void)
{
    /* 10 bit a 9600 baud su una CPU a 10 MHz: 10e6 * 100 / (10 * 9600) */
    const uint64_t CC = 10416;
    st_bad = 0;

    /* ---- 1) baud rate e durata del carattere --------------------- */
    z8530_init(10000000u, 3686400u);
    z8530_set_tx_sink(SCC_B, st_sink);
    st_outn = 0;
    st_setup(B_CTRL, 0);
    z8530_tick(0);
    ck(z8530_info(SCC_B)->baud == 9600, "baud con TC=10 x16",
       (long)z8530_info(SCC_B)->baud, 9600);
    ck(z8530_info(SCC_B)->char_tenths == 100, "bit per carattere (decimi), 8N1",
       (long)z8530_info(SCC_B)->char_tenths, 100);

    /* ---- 2) Tx Buffer Empty con i tempi veri --------------------- */
    z8530_write(B_DATA, 'A', 0);
    ck((st_rd(B_CTRL, 0, 0) & 0x04) != 0,
       "RR0 D2 dopo il primo byte (shift libero, buffer vuoto)",
       st_rd(B_CTRL, 0, 0) & 0x04, 4);
    z8530_write(B_DATA, 'B', 100);
    ck((st_rd(B_CTRL, 0, 100) & 0x04) == 0,
       "RR0 D2 dopo il secondo byte entro il tempo di carattere",
       st_rd(B_CTRL, 0, 100) & 0x04, 0);
    ck((st_rd(B_CTRL, 1, 100) & 0x01) == 0, "RR1 All Sent mentre trasmette",
       st_rd(B_CTRL, 1, 100) & 0x01, 0);
    ck((st_rd(B_CTRL, 0, CC - 1) & 0x04) == 0, "RR0 D2 ancora 0 appena prima",
       st_rd(B_CTRL, 0, CC - 1) & 0x04, 0);
    ck((st_rd(B_CTRL, 0, CC) & 0x04) != 0, "RR0 D2 torna a 1 dopo un carattere",
       st_rd(B_CTRL, 0, CC) & 0x04, 4);
    ck((st_rd(B_CTRL, 1, 2 * CC) & 0x01) != 0, "RR1 All Sent a fine trasmissione",
       st_rd(B_CTRL, 1, 2 * CC) & 0x01, 1);
    ck(st_outn == 2 && st_out[0] == 'A' && st_out[1] == 'B',
       "i due byte escono nell'ordine giusto", st_outn, 2);

    /* ---- 3) ricezione al baud rate e FIFO da 3 byte -------------- */
    z8530_init(10000000u, 3686400u);
    st_setup(B_CTRL, 0);
    z8530_rx_push(SCC_B, 'x', 0);
    ck((st_rd(B_CTRL, 0, 0) & 0x01) == 0, "RR0 D0 subito dopo il push (byte in volo)",
       st_rd(B_CTRL, 0, 0) & 0x01, 0);
    ck((st_rd(B_CTRL, 0, CC) & 0x01) != 0, "RR0 D0 dopo un tempo di carattere",
       st_rd(B_CTRL, 0, CC) & 0x01, 1);
    ck(z8530_read(B_DATA, CC) == 'x', "il byte letto e' quello spedito", 0, 0);
    ck((st_rd(B_CTRL, 0, CC) & 0x01) == 0, "RR0 D0 si azzera dopo la lettura",
       st_rd(B_CTRL, 0, CC) & 0x01, 0);

    for (int i = 0; i < 5; i++) z8530_rx_push(SCC_B, (uint8_t)('0' + i), CC);
    z8530_tick(CC + 10 * CC);
    ck((st_rd(B_CTRL, 1, CC + 10 * CC) & 0x20) != 0,
       "RR1 D5 Rx Overrun con 5 byte in una FIFO da 3",
       st_rd(B_CTRL, 1, CC + 10 * CC) & 0x20, 0x20);

    /* ---- 4) interrupt di ricezione sul canale B ------------------ */
    z8530_init(10000000u, 3686400u);
    st_setup(B_CTRL, 0);
    st_wr(B_CTRL, 2, 0x40, 0);          /* vettore base                */
    st_wr(B_CTRL, 9, 0x09, 0);          /* MIE + VIS                   */
    st_wr(B_CTRL, 1, 0x10, 0);          /* interrupt su ogni carattere */
    ck(z8530_irq() == 0, "nessun interrupt a riposo", z8530_irq(), 0);

    z8530_rx_push(SCC_B, 'P', 0);
    z8530_tick(CC);
    ck(z8530_irq() == 1, "interrupt dopo un byte in arrivo", z8530_irq(), 1);
    ck((st_rd(A_CTRL, 3, CC) & 0x04) != 0, "RR3 D2 (B Rx IP), letta sul canale A",
       st_rd(A_CTRL, 3, CC) & 0x04, 0x04);
    ck(st_rd(B_CTRL, 3, CC) == 0x00, "RR3 sul canale B legge 0",
       st_rd(B_CTRL, 3, CC), 0);
    ck(st_rd(B_CTRL, 2, CC) == 0x44, "RR2 sul canale B: vettore con stato B Rx",
       st_rd(B_CTRL, 2, CC), 0x44);
    ck(st_rd(A_CTRL, 2, CC) == 0x40, "RR2 sul canale A: vettore non modificato",
       st_rd(A_CTRL, 2, CC), 0x40);

    int vec = z8530_iack();
    ck(vec == 0x44, "vettore restituito nell'IACK", vec, 0x44);
    ck(z8530_irq() == 0, "l'IUS blocca le sorgenti sotto", z8530_irq(), 0);
    ck(z8530_read(B_DATA, CC) == 'P', "il gestore legge il byte", 0, 0);
    st_wr(B_CTRL, 0, 0x38, CC);         /* Reset Highest IUS           */
    ck(z8530_irq() == 0, "niente piu' interrupt dopo la lettura", z8530_irq(), 0);

    /* ---- 5) interrupt di trasmissione ---------------------------- */
    z8530_init(10000000u, 3686400u);
    z8530_set_tx_sink(SCC_B, st_sink);
    st_outn = 0;
    st_setup(B_CTRL, 0);
    st_wr(B_CTRL, 2, 0x40, 0);
    st_wr(B_CTRL, 9, 0x09, 0);
    st_wr(B_CTRL, 1, 0x02, 0);          /* Tx Int Enable a buffer vuoto */
    ck(z8530_irq() == 1, "abilitare Tx Int a buffer vuoto chiede subito servizio",
       z8530_irq(), 1);
    ck(z8530_iack() == 0x40, "vettore con stato B Tx", 0, 0x40);
    z8530_write(B_DATA, 'Z', 0);        /* il byte passa subito allo shift */
    st_wr(B_CTRL, 0, 0x38, 0);          /* Reset Highest IUS               */
    ck(z8530_irq() == 1, "buffer subito vuoto: il chip richiede il byte dopo",
       z8530_irq(), 1);
    st_wr(B_CTRL, 0, 0x28, 0);          /* Reset Tx Int Pending            */
    ck(z8530_irq() == 0, "Reset Tx Int Pending chiude la trasmissione",
       z8530_irq(), 0);
    z8530_write(B_DATA, 'Y', 100);      /* secondo byte entro il carattere */
    ck(z8530_irq() == 0, "buffer pieno: nessuna richiesta", z8530_irq(), 0);
    ck(st_outn == 1, "il secondo byte e' ancora nel buffer", st_outn, 1);
    z8530_tick(2 * CC);
    ck(z8530_irq() == 1, "richiesta quando il buffer torna vuoto", z8530_irq(), 1);
    ck(st_outn == 2 && st_out[1] == 'Y', "e il byte esce", st_outn, 2);

    /* ---- 5b) un canale a polling non deve generare interrupt ----- */
    z8530_init(10000000u, 3686400u);
    z8530_set_tx_sink(SCC_A, st_sink);
    st_outn = 0;
    st_setup(A_CTRL, 0);
    st_setup(B_CTRL, 0);
    st_wr(B_CTRL, 2, 0x40, 0);
    st_wr(B_CTRL, 9, 0x09, 0);
    st_wr(B_CTRL, 1, 0x10, 0);          /* interrupt solo in Rx, solo su B */
    z8530_write(A_DATA, 'c', 0);        /* console a polling sul canale A  */
    z8530_tick(3 * CC);
    ck(z8530_irq() == 0, "il canale A a polling non chiede interrupt",
       z8530_irq(), 0);

    /* ---- 6) priorita': il canale A vince sul B ------------------- */
    z8530_init(10000000u, 3686400u);
    st_setup(A_CTRL, 0);
    st_setup(B_CTRL, 0);
    st_wr(A_CTRL, 2, 0x40, 0);
    st_wr(A_CTRL, 9, 0x09, 0);
    st_wr(A_CTRL, 1, 0x10, 0);
    st_wr(B_CTRL, 1, 0x10, 0);
    z8530_rx_push(SCC_A, 'a', 0);
    z8530_rx_push(SCC_B, 'b', 0);
    z8530_tick(CC);
    ck(z8530_iack() == 0x4C, "l'IACK serve prima il canale A (stato 110)", 0, 0x4C);

    /* ---- 7) senza clock programmato resta passante --------------- */
    z8530_init(10000000u, 3686400u);
    z8530_set_tx_sink(SCC_A, st_sink);
    st_outn = 0;
    z8530_write(A_DATA, 'q', 0);
    ck(st_outn == 1 && st_out[0] == 'q',
       "ROM che scrive senza inizializzare: il byte esce lo stesso", st_outn, 1);
    ck((st_rd(A_CTRL, 0, 0) & 0x04) != 0,
       "Tx Buffer Empty resta a 1 senza clock", st_rd(A_CTRL, 0, 0) & 0x04, 4);

    z8530_init(10000000u, 3686400u);
    if (!st_bad) fprintf(stderr, "[SCC] autotest: ok (baud rate, FIFO, "
                                 "interrupt, priorita', vettori)\n");
    return st_bad ? -1 : 0;
}
