/*
 * harness.c - emulatore di bring-up per il PDA M68K (MC68010 @ 10 MHz)
 * Basato su Musashi. Una sola unita', niente dipendenze oltre libc.
 *
 * Memory map modellata (24 bit di indirizzo, come il 68010 vero):
 *
 *   0x000000-0x01FFFF  ROM 128 kB   (scritture ignorate + warning)
 *   0x020000-0x027FFF  SRAM 32 kB
 *   0x028000-0x1FFFFF  non allocato  -> BUS ERROR (timeout DTACK)
 *   0x200000-0xDFFFFF  finestra DRAM 12 MB, popolata solo per -d MB
 *   0xE00000-0xFFFFFF  MMIO (vedi sotto)
 *
 * MMIO, secondo la mappa fornita (A23-21=111, poi 4 periferiche da 256
 * byte ciascuna selezionate da A12-A8, con A20-13 FORZATI a 0 qui: le
 * equazioni originali li lasciano "don't care", il che aliaserebbe ogni
 * periferica su tutta la finestra da 2MB. Qui li pinniamo per avere un
 * comportamento pulito e per far scattare un bus error su tutto il resto,
 * cosi' si individuano subito gli accessi fuori mappa):
 *
 *   0xE00000-0xE000FF  Z8530 SCC     (A2=A/B#, A1=D/C#, A0 don't care)
 *   0xE00100-0xE001FF  IDE/ATA       (A3..A1 = registro task file, A4 = CS0#:
 *                                     A4=1 -> banco alternate, NON implementato)
 *   0xE00200-0xE002FF  SSD1963       (A1=D/C#, A0 don't care): display 7"
 *                                     800x480 RGB565, bus 8080 a 16 bit.
 *                                     $E00201 comando, $E00202/3 dato
 *   0xE00300-0xE003FF  XPT2046 touch (qualsiasi indirizzo nella pagina):
 *                                     touch resistivo, SPI a 8 bit
 *   0xE00400-0xFFFFFF  non allocato  -> BUS ERROR
 *
 * Tutto cio' che non e' mappato da' bus error, come farebbe il GAL con il
 * timer di DTACK. Se l'hardware non lo fara', cambia decode() qui sotto.
 *
 * Larghezza di bus delle periferiche:
 *   - SCC e XPT2046 sono a 8 bit con A0 fisso a 1 (abilitate da LDS, dati su
 *     D7-D0); un accesso .W/.L e' un ciclo per word con il solo byte basso
 *     valido (warning + un solo accesso al registro per word, byte alto
 *     letto 0xFF);
 *   - l'SSD1963 e' a 16 bit (D0-D15 diretti): un accesso .W e' il ciclo
 *     nativo, con i pixel RGB565 su tutte e 16 le linee; comando e parametri
 *     stanno su D7-D0 e si scrivono bene anche con .B all'indirizzo dispari;
 *   - il registro Data della IDE (reg 0, indirizzo pari),
 *     a 16 bit con D0-D15 della CPU collegati DIRETTAMENTE a D0-D15 della IDE:
 *     la CPU vede la word ATA "vera" (byte basso del settore in D7-D0).
 *     In memoria 68k (big-endian) i byte di ogni coppia risultano quindi
 *     scambiati rispetto al disco: il firmware deve fare lo swap.
 *
 * Timer hardware: 100 Hz (10 ms), IRQ autovector (opzione -t).
 * /PENIRQ del touch: IRQ autovector opzionale (opzione -p).
 *
 * Il pannello viene disegnato in una finestra SDL2 (devices/panel.c); il
 * mouse fa da dito sul vetro resistivo e la tastiera della finestra entra
 * nella console sul canale A dell'SCC. Senza SDL (make NOGUI=1) o senza
 * display l'emulazione dei due chip resta attiva: -S salva lo schermo.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "m68k.h"

#include "devices/panel.h"
#include "devices/ssd1963.h"
#include "devices/xpt2046.h"
#include "devices/z8530.h"

/* ------------------------------------------------------------------ */
/* Parametri del sistema                                               */
/* ------------------------------------------------------------------ */
#define CPU_HZ        10000000u
#define SLICE_CYCLES  (CPU_HZ / 1000)      /* 1 ms emulato per fetta   */
#define TICK_CYCLES   (CPU_HZ / 100)       /* timer hardware: 100 Hz = 10 ms */
#define ADDR_MASK     0xFFFFFFu            /* 24 linee di indirizzo    */

#define ROM_BASE      0x000000u
#define ROM_SIZE      0x020000u
#define SRAM_BASE     0x020000u
#define SRAM_SIZE     0x008000u
#define DRAM_BASE     0x200000u
#define DRAM_WINDOW   0xC00000u

/* MMIO: finestra attiva 64 kB (A20-13 pinnati a 0), 4 periferiche da 256 B */
#define MMIO_BASE     0xE00000u
#define MMIO_SPAN     0x010000u            /* 0xE00000-0xE0FFFF */
#define DEV_PAGE      0x000100u            /* 256 byte per periferica */

#define SCC_BASE      (MMIO_BASE + 0x000u)
#define SCC_REG(off)  (((off) >> 1) & 3)   /* A2=A/B#, A1=D/C#, A0 non conta: 0=B ctrl 1=B data 2=A ctrl 3=A data */

#define IDE_BASE      (MMIO_BASE + 0x100u)
#define IDE_REG(off)  (((off) >> 1) & 7)   /* A3..A1 -> 0..7, vedi tabella dei registri task-file */
#define IDE_CS0_N(off) (((off) >> 4) & 1)  /* A4: 0 = task file (CS0# attivo), 1 = alternate (assente) */

#define SSD_BASE      (MMIO_BASE + 0x200u)
#define SSD_REG_DC(off) (((off) >> 1) & 1) /* A1=D/C#: 0=comando 1=dato */

#define XPT_BASE      (MMIO_BASE + 0x300u)

/* ------------------------------------------------------------------ */
/* Stato globale                                                       */
/* ------------------------------------------------------------------ */
static uint8_t  rom[ROM_SIZE];
static uint8_t  sram[SRAM_SIZE];
static uint8_t *dram;
static uint32_t dram_bytes;

static unsigned cur_fc;
static int  opt_trace, opt_strict, opt_fast, opt_verbose;
static int  timer_level;
static int  pen_level;                     /* /PENIRQ del touch su IPL (0 = polling) */
static int  quit_flag;
static int  rom_warns;
static struct termios saved_tio;
static int  tio_saved;

static void dump_regs(void);

/* Warning sugli accessi MMIO anomali (contatore condiviso, max 20) */
static int  mmio_warns;
static void mmio_warn(const char *msg, uint32_t a)
{
    if (mmio_warns++ < 20)
        fprintf(stderr, "[warn] %s @ 0x%06X PC=0x%06X\n", msg, a,
                m68k_get_reg(NULL, M68K_REG_PPC) & ADDR_MASK);
}

/* ------------------------------------------------------------------ */
/* Tempo in cicli di CPU                                               */
/* ------------------------------------------------------------------ */
/* Dentro m68k_execute il conteggio della fetta in corso lo tiene Musashi:
 * cosi' le periferiche vedono il tempo con la precisione dell'istruzione e
 * non della fetta da 1 ms. Serve all'SCC per i tempi al baud rate. */
static uint64_t cycle_base;
static int      in_execute;

static uint64_t now_cycles(void)
{
    return cycle_base + (in_execute ? (uint64_t)m68k_cycles_run() : 0);
}

/* ------------------------------------------------------------------ */
/* Z8530 SCC: canale A = console, canale B = ESP32 con ZiModem        */
/* ------------------------------------------------------------------ */
/* Il chip sta in devices/z8530.c. Qui c'e' solo l'aggancio al mondo:
 * il canale A parla con stdout/stdin (e con la tastiera della finestra del
 * pannello), il canale B con un pty o un device scelto da --scc-b. */
static int scc_irq_level;          /* 0 = /INT non cablato             */
static int scc_vectored;           /* 1 = il chip pilota il vettore    */
static int chb_fd = -1;            /* capo host del canale B           */
static int chb_slave_fd = -1;      /* tenuto aperto: senza, il master
                                      del pty darebbe EIO              */
static char chb_name[128];

static void scc_tx_a(uint8_t v) { ssize_t r = write(1, &v, 1); (void)r; }

static void scc_tx_b(uint8_t v)
{
    if (chb_fd < 0) return;
    ssize_t r = write(chb_fd, &v, 1); (void)r;
}

/* Un carattere nella linea di ricezione del canale A (console). Lo usano
 * sia stdin sia la tastiera della finestra del pannello. */
static void scc_push_rx(unsigned char c)
{
    z8530_rx_push(SCC_A, c, now_cycles());
}

static uint8_t scc_read8(uint32_t off)
{
    return z8530_read(off, now_cycles());
}

static void scc_write8(uint32_t off, uint8_t v)
{
    z8530_write(off, v, now_cycles());
}

/* ------------------------------------------------------------------ */
/* IDE/ATA in PIO, 16 bit, LBA28 (minimale: READ/WRITE SECTORS,       */
/* IDENTIFY DEVICE). Backing store opzionale via -i immagine.bin.     */
/* ------------------------------------------------------------------ */
#define ATA_SECSZ 512u

static FILE    *ide_img;
static uint64_t ide_img_sectors;
static uint8_t  ide_error, ide_seccnt, ide_secnum, ide_cyllo, ide_cylhi, ide_drvhead;
static uint8_t  ide_status;                /* BSY7 DRDY6 DF5 DSC4 DRQ3 COR2 IDX1 ERR0 */
static uint8_t  ide_buf[ATA_SECSZ];        /* ordine byte = ordine nel file/sul disco */
static unsigned ide_bufpos;                /* indice a word nel buffer corrente */
static unsigned ide_pending;                /* settori ancora da trasferire dopo quello in corso */
static int      ide_is_write;

enum { ATA_BSY=0x80, ATA_DRDY=0x40, ATA_DRQ=0x08, ATA_ERR=0x01 };

/* LBA28 corrente dai registri task-file (sector number = bit 0-7, cyl low/high
 * = bit 8-23, i 4 bit bassi di drive/head = bit 24-27). */
static uint32_t ide_cur_lba(void)
{
    return ((uint32_t)(ide_drvhead & 0x0F) << 24) | ((uint32_t)ide_cylhi << 16)
         | ((uint32_t)ide_cyllo << 8) | ide_secnum;
}

static void ide_set_lba(uint32_t lba)
{
    ide_secnum  = (uint8_t)lba;
    ide_cyllo   = (uint8_t)(lba >> 8);
    ide_cylhi   = (uint8_t)(lba >> 16);
    ide_drvhead = (ide_drvhead & 0xF0) | ((lba >> 24) & 0x0F);
}

static void ide_cmd(uint8_t cmd);
static uint16_t ide_data_read16(void);

static uint8_t ide_read8(uint32_t off)
{
    if (IDE_CS0_N(off)) {
        mmio_warn("lettura dal banco IDE alternate (CS0#=1), non implementato: 0xFF", IDE_BASE + off);
        return 0xFF;
    }
    int reg = IDE_REG(off);
    switch (reg) {
    case 0: {   /* Data letto a byte: sul ferro e' comunque un ciclo di lettura completo
                 * (la IDE avanza di una word). Indirizzo pari = UDS = D15-D8, dispari =
                 * LDS = D7-D0. Il path normale a 16 bit e' in bus_read (.W pari). */
        mmio_warn("accesso a byte al registro Data IDE (avanza di una word)", IDE_BASE + off);
        uint16_t w = ide_data_read16();
        return (off & 1) ? (uint8_t)(w & 0xFF) : (uint8_t)(w >> 8);
    }
    case 1: return ide_error;
    case 2: return ide_seccnt;
    case 3: return ide_secnum;
    case 4: return ide_cyllo;
    case 5: return ide_cylhi;
    case 6: return ide_drvhead | 0xA0;     /* bit 7,5 sempre a 1 per compatibilita' */
    case 7: return ide_status;             /* leggere lo status non ha side-effect qui (irq non modellato) */
    default: return 0xFF;
    }
}

static void ide_write8(uint32_t off, uint8_t v)
{
    if (IDE_CS0_N(off)) {
        mmio_warn("scrittura sul banco IDE alternate (CS0#=1), non implementato: ignorata", IDE_BASE + off);
        return;
    }
    int reg = IDE_REG(off);
    switch (reg) {
    case 0: mmio_warn("scrittura a byte sul registro Data IDE: ignorata (serve .W)", IDE_BASE + off); break;
    case 1: /* Features: ignorato in questo modello minimale */ break;
    case 2: ide_seccnt = v; break;
    case 3: ide_secnum = v; break;
    case 4: ide_cyllo  = v; break;
    case 5: ide_cylhi  = v; break;
    case 6: ide_drvhead = v; break;
    case 7: ide_cmd(v); break;
    default: break;
    }
}

/* Accesso a 16 bit al registro Data: Musashi passa dal bus generico, ma la
 * Data register IDE e' l'unica davvero a 16 bit. bus_read/bus_write (sotto)
 * la intercettano prima di spezzarla in byte quando l'indirizzo cade qui. */
/* Cablaggio D0-D15 CPU <-> D0-D15 IDE diretto: ATA trasferisce ogni word col
 * byte basso del settore su D7-D0 e il byte alto su D15-D8, quindi la word
 * vista dalla CPU e' little-endian rispetto ai byte del file/disco:
 *     word = buf[2i] | buf[2i+1] << 8
 * Un move.w (IDE),(a0)+ scrive poi in memoria 68k (big-endian) i due byte
 * scambiati rispetto al disco: il firmware deve fare lo swap (ror.w #8) per
 * ottenere il layout del file. */
static uint16_t ide_data_read16(void)
{
    uint16_t w = 0xFFFF;
    if (ide_status & ATA_DRQ) {
        w = ide_buf[ide_bufpos*2] | ((uint16_t)ide_buf[ide_bufpos*2 + 1] << 8);
        ide_bufpos++;
        if (ide_bufpos >= ATA_SECSZ/2) {
            ide_bufpos = 0;
            if (ide_pending > 0) {
                ide_pending--;
                uint32_t lba = ide_cur_lba() + 1;      /* avanza LBA, carica il prossimo */
                ide_set_lba(lba);
                if (ide_img &&
                    fseek(ide_img, (long)lba*ATA_SECSZ, SEEK_SET)==0 &&
                    fread(ide_buf,1,ATA_SECSZ,ide_img)==ATA_SECSZ) {
                    ide_status = ATA_DRDY | ATA_DRQ;
                } else {
                    ide_status = ATA_DRDY | ATA_ERR; ide_error = 0x40; /* UNC */
                }
            } else {
                ide_status = ATA_DRDY;                 /* ultimo settore letto */
            }
        }
    }
    return w;
}

static void ide_data_write16(uint16_t v)
{
    if (!(ide_status & ATA_DRQ)) return;
    ide_buf[ide_bufpos*2]     = (uint8_t)(v & 0xFF);
    ide_buf[ide_bufpos*2 + 1] = (uint8_t)(v >> 8);
    ide_bufpos++;
    if (ide_bufpos >= ATA_SECSZ/2) {
        ide_bufpos = 0;
        uint32_t lba = ide_cur_lba();
        if (ide_img) {
            fseek(ide_img, (long)lba*ATA_SECSZ, SEEK_SET);
            fwrite(ide_buf,1,ATA_SECSZ,ide_img);
            fflush(ide_img);
        }
        if (ide_pending > 0) {
            ide_pending--;
            ide_set_lba(lba + 1);
            ide_status = ATA_DRDY | ATA_DRQ;
        } else {
            ide_status = ATA_DRDY;
        }
    }
}

static void ide_cmd(uint8_t cmd)
{
    uint32_t lba = ide_cur_lba();
    unsigned n = ide_seccnt ? ide_seccnt : 256;
    ide_error = 0;
    if (!ide_img) {
        ide_status = ATA_ERR; ide_error = 0x02 /* ABRT, nessun disco */; return;
    }
    switch (cmd) {
    case 0x20: case 0x21:  /* READ SECTOR(S) [no-retry] */
        if (lba >= ide_img_sectors) { ide_status = ATA_DRDY|ATA_ERR; ide_error = 0x10 /* IDNF */; return; }
        fseek(ide_img, (long)lba*ATA_SECSZ, SEEK_SET);
        if (fread(ide_buf,1,ATA_SECSZ,ide_img) != ATA_SECSZ) { ide_status=ATA_DRDY|ATA_ERR; ide_error=0x40; return; }
        ide_bufpos = 0; ide_pending = n - 1; ide_is_write = 0;
        ide_status = ATA_DRDY | ATA_DRQ;
        break;
    case 0x30: case 0x31:  /* WRITE SECTOR(S) [no-retry] */
        if (lba >= ide_img_sectors) { ide_status = ATA_DRDY|ATA_ERR; ide_error = 0x10; return; }
        ide_bufpos = 0; ide_pending = n - 1; ide_is_write = 1;
        memset(ide_buf, 0, sizeof ide_buf);
        ide_status = ATA_DRDY | ATA_DRQ;
        break;
    case 0xEC:              /* IDENTIFY DEVICE: i campi sono word ATA, ide_buf e'
                               il flusso di byte come sul bus: parola W sta a
                               [2*W] (byte basso) e [2*W+1] (byte alto), cosi'
                               la CPU legge esattamente il valore della word. */
        memset(ide_buf, 0, sizeof ide_buf);
#define IDWORD(w, val) do { uint16_t _v=(val); ide_buf[(w)*2]=(uint8_t)_v; ide_buf[(w)*2+1]=(uint8_t)(_v>>8); } while (0)
        IDWORD(0,  0x0040);                        /* fixed device, non rimovibile */
        IDWORD(49, 0x0200);                        /* bit9 = supporta LBA */
        IDWORD(60, (uint16_t)(ide_img_sectors & 0xFFFF));
        IDWORD(61, (uint16_t)(ide_img_sectors >> 16));
#undef IDWORD
        ide_bufpos = 0; ide_pending = 0;
        ide_status = ATA_DRDY | ATA_DRQ;
        break;
    case 0x00: case 0x08: case 0xE0: case 0xE7: case 0xEF:
        /* NOP / EXEC DEVICE DIAG / soft reset / FLUSH CACHE / SET FEATURES: no-op */
        ide_status = ATA_DRDY;
        break;
    default:
        ide_status = ATA_DRDY | ATA_ERR; ide_error = 0x04 /* ABRT */;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* SSD1963: display 7" 800x480 RGB565, 8080 a 16 bit, A1 = D/C#        */
/* ------------------------------------------------------------------ */
/* Insieme al registro Data della IDE e' l'unica periferica con D0-D15
 * cablati direttamente sul bus dati del 68010. Comandi e parametri viaggiano
 * su D7-D0 (come vuole l'SSD1963 in modo 16 bit), i pixel in formato 565 su
 * tutte e 16 le linee:
 *
 *   move.b #cmd,$E00201    comando   (A1=0, LDS)
 *   move.b #par,$E00203    parametro (A1=1, LDS)
 *   move.w #px, $E00202    pixel     (A1=1, un ciclo di bus a word)
 *
 * Un accesso .L non serve: il secondo ciclo di bus cade su $E00204, dove A1
 * e' tornato a 0, quindi finisce sulla porta comandi. Il modello lo fa
 * davvero (con un warning) invece di fingere un burst che sul ferro non
 * esiste. Per riempire lo schermo in fretta: move.w (a0)+,(a1) con a1 fisso
 * a $E00202. */
static int ssd_uds_warned;

static void ssd_write8(uint32_t off, uint8_t v)
{
    if (!(off & 1) && !ssd_uds_warned++)
        mmio_warn("accesso .B a indirizzo pari sull'SSD1963: sul ferro il byte "
                  "va su D15-D8 (qui lo accetto su D7-D0)", SSD_BASE + off);
    if (SSD_REG_DC(off)) ssd1963_write_data(v, 8);
    else                 ssd1963_write_cmd(v, 8);
}

static uint8_t ssd_read8(uint32_t off)
{
    return (uint8_t)(SSD_REG_DC(off) ? ssd1963_read_data(8)
                                     : ssd1963_read_cmd(8));
}

static void ssd_write16(uint32_t a, uint16_t v)
{
    uint32_t off = (a - MMIO_BASE) & (DEV_PAGE - 1);
    if (SSD_REG_DC(off)) ssd1963_write_data(v, 16);
    else                 ssd1963_write_cmd(v, 16);
}

static uint16_t ssd_read16(uint32_t a)
{
    uint32_t off = (a - MMIO_BASE) & (DEV_PAGE - 1);
    return (uint16_t)(SSD_REG_DC(off) ? ssd1963_read_data(16)
                                      : ssd1963_read_cmd(16));
}

/* ------------------------------------------------------------------ */
/* XPT2046: touch resistivo su SPI dietro la porta a 8 bit             */
/* ------------------------------------------------------------------ */
/* Ogni accesso alla pagina $E00300 vale un trasferimento SPI full duplex a
 * 8 bit (il glue logic clocka 8 impulsi e allunga il DTACK). In lettura
 * MOSI resta a 0 per non generare un bit di start spurio. Con -k w il
 * modello passa all'altra variante possibile: clockano solo le scritture e
 * la lettura restituisce il byte entrato durante l'ultima scrittura. */
static void xpt_write8(uint8_t v) { xpt2046_bus_write(v); }
static uint8_t xpt_read8(void) { return xpt2046_bus_read(); }

/* ------------------------------------------------------------------ */
/* Decode del bus                                                      */
/* ------------------------------------------------------------------ */
typedef enum { R_NONE, R_ROM, R_SRAM, R_DRAM, R_SCC, R_IDE, R_SSD, R_XPT } rkind_t;

static rkind_t decode(uint32_t a, uint8_t **base, uint32_t *off)
{
    if (a < ROM_SIZE)                               { *base = rom;  *off = a;             return R_ROM; }
    if (a >= SRAM_BASE && a < SRAM_BASE+SRAM_SIZE)  { *base = sram; *off = a - SRAM_BASE; return R_SRAM; }
    if (a >= DRAM_BASE && a < DRAM_BASE+dram_bytes) { *base = dram; *off = a - DRAM_BASE; return R_DRAM; }
    if (a >= MMIO_BASE && a < MMIO_BASE+MMIO_SPAN) {
        uint32_t page = (a - MMIO_BASE) & ~(DEV_PAGE - 1);
        uint32_t o    = (a - MMIO_BASE) & (DEV_PAGE - 1);
        *base = NULL; *off = o;
        switch (page) {
        case 0x000: return R_SCC;
        case 0x100: return R_IDE;
        case 0x200: return R_SSD;
        case 0x300: return R_XPT;
        default:    return R_NONE;      /* pagine libere per espansione futura */
        }
    }
    return R_NONE;
}

static const char *fc_name(unsigned fc)
{
    switch (fc & 7) {
    case 1: return "user data";   case 2: return "user prog";
    case 5: return "super data";  case 6: return "super prog";
    case 7: return "cpu space";   default: return "?";
    }
}

/* Non ritorna: m68k_pulse_bus_error() fa longjmp nel loop del core. */
static void bus_fault(uint32_t a, int size, int write)
{
    if (a == 0xFFFF01u) {
        /* Musashi legge questo indirizzo quando si verifica un bus error
         * mentre sta gia' scrivendo il frame di un bus error: la CPU
         * sul ferro andrebbe in HALT. */
        fprintf(stderr, "\n*** DOPPIO BUS FAULT: CPU in HALT (SSP o vettore 2 non validi?)\n");
        dump_regs();
        exit(2);
    }
    fprintf(stderr, "[BUS ERROR] %s%d @ 0x%06X  FC=%u (%s)  PC=0x%06X\n",
            write ? "write" : "read", size * 8, a, cur_fc & 7, fc_name(cur_fc),
            m68k_get_reg(NULL, M68K_REG_PPC) & ADDR_MASK);
    m68k_pulse_bus_error();
}

static uint8_t read_byte(uint32_t a, int size)
{
    uint8_t *b; uint32_t o;
    switch (decode(a, &b, &o)) {
    case R_ROM: case R_SRAM: case R_DRAM: return b[o];
    case R_SCC:  return scc_read8(o);
    case R_IDE:  return ide_read8(o);
    case R_SSD:  return ssd_read8(o);
    case R_XPT:  return xpt_read8();
    default:     bus_fault(a, size, 0); return 0;
    }
}

static void write_byte(uint32_t a, uint8_t v, int size)
{
    uint8_t *b; uint32_t o;
    switch (decode(a, &b, &o)) {
    case R_SRAM: case R_DRAM: b[o] = v; break;
    case R_SCC:  scc_write8(o, v); break;
    case R_IDE:  ide_write8(o, v); break;
    case R_SSD:  ssd_write8(o, v); break;
    case R_XPT:  xpt_write8(v); break;
    case R_ROM:
        if (opt_strict) bus_fault(a, size, 1);
        if (rom_warns++ < 20)
            fprintf(stderr, "[warn] scrittura su ROM @ 0x%06X (ignorata) PC=0x%06X\n",
                    a, m68k_get_reg(NULL, M68K_REG_PPC) & ADDR_MASK);
        break;
    default: bus_fault(a, size, 1);
    }
}

/* Il registro Data della IDE (reg 0, pagina IDE) e' l'unico davvero a 16
 * bit: un accesso .W li' va intercettato qui, prima di spezzarlo in byte,
 * altrimenti ide_bufpos avanzerebbe due volte per ogni word letta/scritta
 * dal firmware (byte alto e byte basso finirebbero su due word del buffer). */
static int is_ide_data16(uint32_t a, int size)
{
    uint32_t off = a - IDE_BASE;
    return size == 2 && a >= IDE_BASE && off < DEV_PAGE && !IDE_CS0_N(off) && IDE_REG(off) == 0;
}

/* Periferica a 8 bit (A0=1 fisso, solo LDS)? L'SSD1963 no: e' a 16 bit. */
static int is_mmio8(uint32_t a)
{
    uint8_t *b; uint32_t o;
    rkind_t k = decode(a, &b, &o);
    return k == R_SCC || k == R_IDE || k == R_XPT;
}

/* Pagina dell'SSD1963 (periferica a 16 bit)? */
static int is_ssd(uint32_t a)
{
    uint8_t *b; uint32_t o;
    return decode(a, &b, &o) == R_SSD;
}

static uint32_t bus_read(uint32_t a, int size)
{
    a &= ADDR_MASK;
    if (is_ide_data16(a, size)) return ide_data_read16();
    if (size > 1 && (is_ssd(a) || is_ssd((a + (uint32_t)size - 1) & ADDR_MASK))) {
        /* Periferica a 16 bit: un ciclo di bus per word, con il decode
         * rifatto su ogni indirizzo (A1 = D/C# cambia a ogni ciclo). */
        if (size > 2)
            mmio_warn("accesso .L alla porta SSD1963: il secondo ciclo di bus "
                      "cade dove A1 e' cambiato, cioe' sull'altra porta", a);
        uint32_t r = 0;
        for (int i = 0; i < size; i += 2) {
            uint32_t wa = (a + (uint32_t)i) & ADDR_MASK;
            uint16_t w;
            if      (is_ssd(wa))           w = ssd_read16(wa);
            else if (is_ide_data16(wa, 2)) w = ide_data_read16();
            else if (is_mmio8(wa))         w = (uint16_t)(0xFF00 | read_byte(wa, size));
            else w = (uint16_t)((read_byte(wa, size) << 8) | read_byte(wa + 1, size));
            r = (r << 16) | w;
        }
        return r;
    }
    if (size > 1 && is_mmio8(a)) {
        /* Un ciclo di bus per word, un solo accesso al registro per ciclo
         * (non due come con lo split in byte): byte alto non pilotato. */
        mmio_warn("accesso .W/.L a periferica a 8 bit (un ciclo LDS per word, byte alto letto 0xFF)", a);
        uint32_t r = 0;
        for (int i = 0; i < size; i += 2) {
            uint32_t wa = (a + i) & ADDR_MASK;
            uint16_t w = is_ide_data16(wa, 2) ? ide_data_read16()
                                              : (uint16_t)(0xFF00 | read_byte(wa, size));
            r = (r << 16) | w;
        }
        return r;
    }
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v = (v << 8) | read_byte((a + i) & ADDR_MASK, size);
    return v;
}

static void bus_write(uint32_t a, uint32_t v, int size)
{
    a &= ADDR_MASK;
    if (is_ide_data16(a, size)) { ide_data_write16((uint16_t)v); return; }
    if (size > 1 && (is_ssd(a) || is_ssd((a + (uint32_t)size - 1) & ADDR_MASK))) {
        if (size > 2)
            mmio_warn("accesso .L alla porta SSD1963: il secondo ciclo di bus "
                      "cade dove A1 e' cambiato, cioe' sull'altra porta", a);
        int cycles = size / 2;
        for (int i = 0; i < cycles; i++) {
            uint32_t wa = (a + 2u * (uint32_t)i) & ADDR_MASK;
            uint16_t w  = (uint16_t)(v >> (16 * (cycles - 1 - i)));
            if      (is_ssd(wa))           ssd_write16(wa, w);
            else if (is_ide_data16(wa, 2)) ide_data_write16(w);
            else if (is_mmio8(wa))         write_byte(wa, (uint8_t)(w & 0xFF), size);
            else { write_byte(wa, (uint8_t)(w >> 8), size);
                   write_byte(wa + 1, (uint8_t)(w & 0xFF), size); }
        }
        return;
    }
    if (size > 1 && is_mmio8(a)) {
        mmio_warn("accesso .W/.L a periferica a 8 bit (un ciclo LDS per word, vale solo il byte basso)", a);
        int cycles = size / 2;
        for (int i = 0; i < cycles; i++) {
            uint32_t wa = (a + 2 * i) & ADDR_MASK;
            uint16_t w = (uint16_t)(v >> (16 * (cycles - 1 - i)));
            if (is_ide_data16(wa, 2)) ide_data_write16(w);
            else                      write_byte(wa, (uint8_t)(w & 0xFF), size);
        }
        return;
    }
    for (int i = 0; i < size; i++)
        write_byte((a + i) & ADDR_MASK, (v >> (8 * (size - 1 - i))) & 0xFF, size);
}

/* Callback richieste da Musashi */
unsigned int m68k_read_memory_8 (unsigned int a) { return bus_read(a, 1); }
unsigned int m68k_read_memory_16(unsigned int a) { return bus_read(a, 2); }
unsigned int m68k_read_memory_32(unsigned int a) { return bus_read(a, 4); }
void m68k_write_memory_8 (unsigned int a, unsigned int v) { bus_write(a, v, 1); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { bus_write(a, v, 2); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { bus_write(a, v, 4); }

/* Il disassemblatore legge senza effetti collaterali e senza bus error */
static uint8_t peek8(uint32_t a)
{
    uint8_t *b; uint32_t o;
    rkind_t k = decode(a & ADDR_MASK, &b, &o);
    return (k == R_ROM || k == R_SRAM || k == R_DRAM) ? b[o] : 0xFF;
}
unsigned int m68k_read_disassembler_8 (unsigned int a) { return peek8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return (peek8(a) << 8) | peek8(a + 1); }
unsigned int m68k_read_disassembler_32(unsigned int a)
{ return (m68k_read_disassembler_16(a) << 16) | m68k_read_disassembler_16(a + 2); }

/* ------------------------------------------------------------------ */
/* Linee di interrupt: piu' sorgenti sulle stesse IPL2-IPL0            */
/* ------------------------------------------------------------------ */
/* Il 68010 vede un solo codice di livello, quindi se piu' periferiche
 * asseriscono la propria linea vince la priorita' piu' alta, esattamente
 * come l'encoder di priorita' sulla scheda. */
static unsigned irq_lines;                 /* bitmask dei livelli asseriti */

static void irq_update(void)
{
    unsigned hi = 0;
    for (unsigned l = 7; l >= 1; l--)
        if (irq_lines & (1u << l)) { hi = l; break; }
    m68k_set_irq(hi);
}

static void irq_assert(int lvl)
{
    if (lvl <= 0 || (irq_lines & (1u << lvl))) return;
    irq_lines |= 1u << lvl;
    irq_update();
}

static void irq_release(int lvl)
{
    if (lvl <= 0 || !(irq_lines & (1u << lvl))) return;
    irq_lines &= ~(1u << lvl);
    irq_update();
}

/* ------------------------------------------------------------------ */
/* Hook della CPU                                                      */
/* ------------------------------------------------------------------ */
void harness_set_fc(unsigned int fc) { cur_fc = fc; }

int harness_int_ack(int level)
{
    irq_release(level);                    /* la periferica viene "servita" */
    /* Se a quel livello c'e' l'SCC e sta chiedendo, e' lui che vince il
     * ciclo di riconoscimento: va avvisato comunque, perche' e' li' che
     * nasce l'IUS che blocca le sorgenti di priorita' piu' bassa. */
    if (scc_irq_level == level && z8530_irq()) {
        int v = z8530_iack();
        if (scc_vectored && v >= 0) return (unsigned)v;
    }
    return M68K_INT_ACK_AUTOVECTOR;
}

void harness_instr_hook(unsigned int pc)
{
    if (!opt_trace) return;
    char buf[128];
    m68k_disassemble(buf, pc, M68K_CPU_TYPE_68010);
    fprintf(stderr, "%06X  %s\n", pc & ADDR_MASK, buf);
}

static void dump_regs(void)
{
    fprintf(stderr, "PC=%06X SR=%04X  USP=%08X ISP=%08X VBR=%08X\n",
            m68k_get_reg(NULL, M68K_REG_PC) & ADDR_MASK, m68k_get_reg(NULL, M68K_REG_SR),
            m68k_get_reg(NULL, M68K_REG_USP), m68k_get_reg(NULL, M68K_REG_ISP),
            m68k_get_reg(NULL, M68K_REG_VBR));
    for (int i = 0; i < 8; i++)
        fprintf(stderr, "D%d=%08X ", i, m68k_get_reg(NULL, M68K_REG_D0 + i));
    fprintf(stderr, "\n");
    for (int i = 0; i < 8; i++)
        fprintf(stderr, "A%d=%08X ", i, m68k_get_reg(NULL, M68K_REG_A0 + i));
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */
/* Terminale, input, tempo                                             */
/* ------------------------------------------------------------------ */
static void restore_tty(void)
{
    if (tio_saved) tcsetattr(0, TCSANOW, &saved_tio);
}

static void setup_tty(void)
{
    if (!isatty(0)) return;
    if (tcgetattr(0, &saved_tio) != 0) return;
    tio_saved = 1;
    atexit(restore_tty);
    struct termios t = saved_tio;
    cfmakeraw(&t);
    t.c_oflag |= OPOST | ONLCR;            /* '\n' -> CR LF in uscita */
    tcsetattr(0, TCSANOW, &t);
}

/* --- capo host del canale B ------------------------------------------
 * "pty" crea una coppia e stampa il nome dello slave: ci si attacca
 * pppd, picocom, socat o quello che serve. Altrimenti si apre il path
 * dato (un device seriale vero, un pty gia' esistente, una fifo). */
static int open_chb(const char *spec)
{
    struct termios t;

    if (!strcmp(spec, "pty")) {
        chb_fd = posix_openpt(O_RDWR | O_NOCTTY);
        if (chb_fd < 0) { perror("posix_openpt"); return -1; }
        if (grantpt(chb_fd) != 0 || unlockpt(chb_fd) != 0) {
            perror("pty"); return -1;
        }
        const char *nm = ptsname(chb_fd);
        if (!nm) { perror("ptsname"); return -1; }
        snprintf(chb_name, sizeof chb_name, "%s", nm);
        /* Lo slave resta aperto per tutta la vita dell'harness: se lo
         * chiudessimo, alla disconnessione del programma host il master
         * comincerebbe a dare EIO. */
        chb_slave_fd = open(nm, O_RDWR | O_NOCTTY);
        if (chb_slave_fd >= 0 && tcgetattr(chb_slave_fd, &t) == 0) {
            cfmakeraw(&t);                 /* niente eco ne' CR/LF dal kernel */
            tcsetattr(chb_slave_fd, TCSANOW, &t);
        }
    } else {
        chb_fd = open(spec, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (chb_fd < 0) { perror(spec); return -1; }
        snprintf(chb_name, sizeof chb_name, "%s", spec);
        if (isatty(chb_fd) && tcgetattr(chb_fd, &t) == 0) {
            cfmakeraw(&t);
            tcsetattr(chb_fd, TCSANOW, &t);
        }
    }
    int fl = fcntl(chb_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(chb_fd, F_SETFL, fl | O_NONBLOCK);
    return 0;
}

static void poll_chb(void)
{
    if (chb_fd < 0) return;
    uint8_t buf[256];
    while (z8530_rx_room(SCC_B) > (int)sizeof buf) {
        struct pollfd p = { .fd = chb_fd, .events = POLLIN };
        if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) break;
        ssize_t n = read(chb_fd, buf, sizeof buf);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++)
            z8530_rx_push(SCC_B, buf[i], now_cycles());
    }
}

static void poll_stdin(void)
{
    struct pollfd p = { .fd = 0, .events = POLLIN };
    while (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
        uint8_t c;
        ssize_t n = read(0, &c, 1);
        if (n <= 0) { p.events = 0; break; }
        if (c == 0x1D) { quit_flag = 1; return; }       /* Ctrl-] = esci */
        scc_push_rx(c);
    }
}

static void pace(uint64_t cycles, const struct timespec *t0)
{
    uint64_t target_ns = cycles * 1000000000ull / CPU_HZ;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t el = (uint64_t)(now.tv_sec - t0->tv_sec) * 1000000000ull
                + (uint64_t)(now.tv_nsec - t0->tv_nsec);
    if (el < target_ns) {
        uint64_t d = target_ns - el;
        struct timespec ts = { d / 1000000000ull, d % 1000000000ull };
        nanosleep(&ts, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
static void usage(const char *p)
{
    fprintf(stderr,
        "uso: %s [opzioni] rom.bin\n"
        "\nSistema:\n"
        "  -d, --dram MB         DRAM installata, 0..12 (default 4)\n"
        "  -i, --disk FILE       immagine disco per la IDE (PIO, settori da 512 B)\n"
        "  -t, --timer LIV       timer hw 100 Hz come IRQ autovector di livello LIV (1..7)\n"
        "  -c, --cycles N        ferma dopo N cicli e stampa i registri\n"
        "  -f, --fast            niente pacing a 10 MHz: gira piu' veloce che puo'\n"
        "  -s, --strict          scrivere in ROM da' bus error invece di un warning\n"
        "  -T, --trace           stampa ogni istruzione (PC + disassembly)\n"
        "  -v, --verbose         log periferiche (ripetibile: -vv logga anche i dati)\n"
        "\nPannello 7\" 800x480 (SSD1963):\n"
        "  -n, --no-gui          niente finestra: emula il chip e basta (headless)\n"
        "  -z, --scale N         ingrandimento della finestra 1..4 (default automatico)\n"
        "  -F, --fullscreen      parti a schermo intero\n"
        "      --no-marker       non disegnare il mirino dove si tocca\n"
        "  -S, --screenshot FILE salva lo schermo in PPM all'uscita\n"
        "\nSeriale (Z8530 SCC):\n"
        "      --scc-irq LIV     /INT dell'SCC su IRQ autovector di livello LIV\n"
        "      --scc-vectored    il chip mette il vettore sul bus nel ciclo di IACK\n"
        "                        (altrimenti autovector: l'ISR legge RR2/RR3)\n"
        "      --scc-clock HZ    PCLK/RTxC dell'SCC (default 3686400)\n"
        "      --scc-b SPEC      canale B: 'pty' crea una coppia e stampa il nome,\n"
        "                        oppure un path (device seriale, pty, fifo)\n"
        "      --scc-selftest    verifica baud rate, FIFO e interrupt ed esce\n"
        "\nTouch (XPT2046):\n"
        "  -p, --pen-irq LIV     /PENIRQ come IRQ autovector di livello LIV (default: polling)\n"
        "  -k, --touch-clock M   rw = ogni accesso clocka 8 bit SPI (default)\n"
        "                        w  = clockano solo le scritture, la lettura e' il latch\n"
        "  -X, --touch-cal SPEC  xmin,xmax,ymin,ymax[,swapxy,invx,invy]\n"
        "                        (default 200,3900,200,3900,0,0,0)\n"
        "  -N, --touch-noise N   rumore dell'ADC in LSB (default 3, 0 = nessuno)\n"
        "      --touch-script F  tocchi finti a tempo da file (prove headless):\n"
        "                        righe \"<ms> down|move <x> <y>\" / \"<ms> up\"\n"
        "      --panel-selftest  verifica la geometria finestra/pannello ed esce\n"
        "\nNella finestra: click/trascina = dito sul vetro, tastiera = console SCC A,\n"
        "F1 mirino, F11 fullscreen, F12 screenshot, Ctrl-Q esci.\n"
        "Sul terminale: Ctrl-] per uscire.\n", p);
}

/* ------------------------------------------------------------------ */
/* Script del touch: tocchi finti a tempo, per le prove headless        */
/* ------------------------------------------------------------------ */
/* Formato (una riga per evento, '#' = commento):
 *
 *     <ms>  down  <x> <y>      dito appoggiato in (x,y)
 *     <ms>  move  <x> <y>      trascinamento
 *     <ms>  up                 dito alzato
 *
 * <ms> sono millisecondi di tempo emulato dall'avvio. Serve per provare il
 * percorso completo del touch senza finestra:
 *     ./m68k-harness test/touch.bin -n -f --touch-script t.txt -S out.ppm
 */
typedef struct { uint32_t ms; int down, x, y; } touch_ev_t;
static touch_ev_t *tscript;
static unsigned     tscript_n, tscript_pos;

static int load_touch_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    char line[256];
    unsigned cap = 0;
    int last_x = 0, last_y = 0;
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        unsigned long ms; char act[16]; int x = 0, y = 0;
        int n = sscanf(line, "%lu %15s %d %d", &ms, act, &x, &y);
        if (n < 2) continue;
        int down;
        if      (!strcmp(act, "down") || !strcmp(act, "move")) { down = 1; }
        else if (!strcmp(act, "up"))  { down = 0; x = last_x; y = last_y; }
        else { fprintf(stderr, "%s: azione '%s' sconosciuta\n", path, act); fclose(f); return -1; }
        if (down && n < 4) { fprintf(stderr, "%s: a '%s' servono x e y\n", path, act); fclose(f); return -1; }
        if (tscript_n == cap) {
            cap = cap ? cap * 2 : 16;
            touch_ev_t *t = realloc(tscript, cap * sizeof *t);
            if (!t) { fclose(f); return -1; }
            tscript = t;
        }
        tscript[tscript_n].ms = (uint32_t)ms;
        tscript[tscript_n].down = down;
        tscript[tscript_n].x = x;
        tscript[tscript_n].y = y;
        tscript_n++;
        last_x = x; last_y = y;
    }
    fclose(f);
    fprintf(stderr, "[touch] script %s: %u eventi\n", path, tscript_n);
    return 0;
}

/* Applica gli eventi la cui ora e' passata. ms = tempo emulato. */
static void run_touch_script(uint64_t ms)
{
    while (tscript_pos < tscript_n && tscript[tscript_pos].ms <= ms) {
        const touch_ev_t *e = &tscript[tscript_pos++];
        const ssd1963_state_t *ds = ssd1963_state();
        xpt2046_set_touch(e->down, e->x, e->y, ds->w, ds->h);
        if (opt_verbose)
            fprintf(stderr, "[touch] %ums: %s %d,%d\n", e->ms,
                    e->down ? "giu'" : "su", e->x, e->y);
    }
}

/* "xmin,xmax,ymin,ymax[,swapxy,invx,invy]" */
static int parse_touch_cal(const char *spec)
{
    int v[7] = { 200, 3900, 200, 3900, 0, 0, 0 };
    int n = 0;
    const char *q = spec;
    while (*q && n < 7) {
        char *end;
        long x = strtol(q, &end, 0);
        if (end == q) return -1;
        v[n++] = (int)x;
        q = end;
        if (*q == ',') q++;
        else if (*q) return -1;
    }
    if (n < 4) return -1;
    xpt2046_set_cal(v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned dram_mb = 4;
    uint64_t max_cycles = 0;
    const char *disk_path = NULL;
    const char *shot_path = NULL;
    const char *chb_spec = NULL;
    int opt_nogui = 0;
    panel_opts_t pan = { 0, 0, 1, 0 };
    int opt;

    /* I device vanno inizializzati prima delle opzioni: la calibrazione del
     * touch e il log si appoggiano sul loro stato. */
    ssd1963_init();
    xpt2046_init();
    z8530_init(CPU_HZ, 0);
    z8530_set_tx_sink(SCC_A, scc_tx_a);
    z8530_set_tx_sink(SCC_B, scc_tx_b);

    enum { OPT_NOMARKER = 1000, OPT_TSCRIPT, OPT_SELFTEST,
       OPT_SCCIRQ, OPT_SCCVEC, OPT_SCCCLK, OPT_SCCB, OPT_SCCTEST };
    static const struct option longopts[] = {
        { "dram",        required_argument, NULL, 'd' },
        { "disk",        required_argument, NULL, 'i' },
        { "timer",       required_argument, NULL, 't' },
        { "cycles",      required_argument, NULL, 'c' },
        { "fast",        no_argument,       NULL, 'f' },
        { "strict",      no_argument,       NULL, 's' },
        { "trace",       no_argument,       NULL, 'T' },
        { "verbose",     no_argument,       NULL, 'v' },
        { "no-gui",      no_argument,       NULL, 'n' },
        { "scale",       required_argument, NULL, 'z' },
        { "fullscreen",  no_argument,       NULL, 'F' },
        { "no-marker",   no_argument,       NULL, OPT_NOMARKER },
        { "screenshot",  required_argument, NULL, 'S' },
        { "pen-irq",     required_argument, NULL, 'p' },
        { "touch-clock", required_argument, NULL, 'k' },
        { "touch-cal",   required_argument, NULL, 'X' },
        { "touch-noise", required_argument, NULL, 'N' },
        { "touch-script",required_argument, NULL, OPT_TSCRIPT },
        { "panel-selftest", no_argument,    NULL, OPT_SELFTEST },
        { "scc-irq",     required_argument, NULL, OPT_SCCIRQ },
        { "scc-vectored",no_argument,       NULL, OPT_SCCVEC },
        { "scc-clock",   required_argument, NULL, OPT_SCCCLK },
        { "scc-b",       required_argument, NULL, OPT_SCCB },
        { "scc-selftest",no_argument,       NULL, OPT_SCCTEST },
        { "help",        no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "d:i:t:c:fsTvnz:FS:p:k:X:N:h",
                              longopts, NULL)) != -1) {
        switch (opt) {
        case 'd': dram_mb = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'i': disk_path = optarg; break;
        case 't': timer_level = atoi(optarg); break;
        case 'T': opt_trace = 1; break;
        case 's': opt_strict = 1; break;
        case 'v': opt_verbose++; break;
        case 'f': opt_fast = 1; break;
        case 'c': max_cycles = strtoull(optarg, NULL, 0); break;
        case 'n': opt_nogui = 1; break;
        case 'z': pan.scale = atoi(optarg); break;
        case 'F': pan.fullscreen = 1; break;
        case OPT_NOMARKER: pan.show_touch = 0; break;
        case 'S': shot_path = optarg; break;
        case 'p': pen_level = atoi(optarg); break;
        case 'k':
            if      (!strcmp(optarg, "rw")) xpt2046_set_clock_mode(XPT_CLK_RW);
            else if (!strcmp(optarg, "w"))  xpt2046_set_clock_mode(XPT_CLK_WONLY);
            else { fprintf(stderr, "-k: usa rw oppure w\n"); return 1; }
            break;
        case 'X':
            if (parse_touch_cal(optarg) != 0) {
                fprintf(stderr, "-X: formato xmin,xmax,ymin,ymax[,swapxy,invx,invy]\n");
                return 1;
            }
            break;
        case 'N': xpt2046_set_noise(atoi(optarg)); break;
        case OPT_TSCRIPT: if (load_touch_script(optarg) != 0) return 1; break;
        case OPT_SELFTEST: return panel_selftest() == 0 ? 0 : 1;
        case OPT_SCCTEST:  return z8530_selftest() == 0 ? 0 : 1;
        case OPT_SCCIRQ:   scc_irq_level = atoi(optarg); break;
        case OPT_SCCVEC:   scc_vectored = 1; break;
        case OPT_SCCCLK:   z8530_set_clock((uint32_t)strtoul(optarg, NULL, 0)); break;
        case OPT_SCCB:     chb_spec = optarg; break;
        default:  usage(argv[0]); return 1;
        }
    }
    if (optind >= argc || dram_mb > 12 || timer_level < 0 || timer_level > 7
        || pen_level < 0 || pen_level > 7
        || scc_irq_level < 0 || scc_irq_level > 7) {
        usage(argv[0]); return 1;
    }
    ssd1963_set_log(opt_verbose);
    xpt2046_set_log(opt_verbose);
    z8530_set_log(opt_verbose);

    if (chb_spec) {
        if (open_chb(chb_spec) != 0) return 1;
        fprintf(stderr, "[SCC B] canale B collegato a %s\n", chb_name);
        z8530_set_dcd(SCC_B, 1, 0);        /* il collegamento c'e' */
    }

    /* ROM: erased EPROM = 0xFF */
    memset(rom, 0xFF, sizeof rom);
    FILE *f = fopen(argv[optind], "rb");
    if (!f) { perror(argv[optind]); return 1; }
    static uint8_t tmp[ROM_SIZE + 1];      /* +1 per accorgersi di un file troppo grande */
    size_t n = fread(tmp, 1, sizeof tmp, f);
    fclose(f);
    if (n > ROM_SIZE) { fprintf(stderr, "ROM piu' grande di 128 kB\n"); return 1; }
    memcpy(rom, tmp, n);
    if (n >= 4 && memcmp(rom, "\x7f" "ELF", 4) == 0) {
        fprintf(stderr,
            "%s e' un file ELF, non un'immagine binaria piatta.\n"
            "Assembla con -Fbin oppure converti: m68k-elf-objcopy -O binary in.elf rom.bin\n",
            argv[optind]);
        return 1;
    }

    /* RAM: contenuto pseudo-casuale, come all'accensione (cosi' le variabili
     * non inizializzate si fanno notare). */
    uint32_t x = 0x1234567u;
    #define RND() (x ^= x << 13, x ^= x >> 17, x ^= x << 5, (uint8_t)x)
    for (size_t i = 0; i < sizeof sram; i++) sram[i] = RND();
    dram_bytes = dram_mb << 20;
    if (dram_bytes) {
        dram = malloc(dram_bytes);
        if (!dram) { perror("malloc"); return 1; }
        for (uint32_t i = 0; i < dram_bytes; i++) dram[i] = RND();
    }

    if (disk_path) {
        ide_img = fopen(disk_path, "r+b");
        if (!ide_img) { perror(disk_path); return 1; }
        fseek(ide_img, 0, SEEK_END);
        long sz = ftell(ide_img);
        if (sz < 0) { perror(disk_path); return 1; }
        ide_img_sectors = (uint64_t)sz / ATA_SECSZ;
        ide_status = ATA_DRDY;
    }

    char scc_irq_desc[80];
    if (scc_irq_level)
        snprintf(scc_irq_desc, sizeof scc_irq_desc, "su livello %d, %s",
                 scc_irq_level, scc_vectored ? "vettore dal chip" : "autovector");
    else
        snprintf(scc_irq_desc, sizeof scc_irq_desc, "non cablato (polling)");

    char pen_irq_desc[40];
    if (pen_level) snprintf(pen_irq_desc, sizeof pen_irq_desc,
                            "su IRQ autovector livello %d", pen_level);
    else           snprintf(pen_irq_desc, sizeof pen_irq_desc, "non cablato (polling)");

    fprintf(stderr,
        "ROM   %06X-%06X  (%zu byte caricati)\n"
        "SRAM  %06X-%06X\n"
        "DRAM  %06X-%06X  (%u MB installati, finestra fino a %06X)\n"
        "SCC   %06X-%06X  Z8530: A = console, B = %s\n"
        "                      /INT %s\n"
        "IDE   %06X-%06X  %s\n"
        "SSD   %06X-%06X  display %ux%u RGB565, 16 bit: cmd %06X / dato %06X\n"
        "XPT   %06X-%06X  touch XPT2046 (SPI a 8 bit), /PENIRQ %s\n"
        "Timer %s\n\n",
        ROM_BASE, ROM_SIZE - 1, n,
        SRAM_BASE, SRAM_BASE + SRAM_SIZE - 1,
        DRAM_BASE, dram_bytes ? DRAM_BASE + dram_bytes - 1 : DRAM_BASE, dram_mb,
        DRAM_BASE + DRAM_WINDOW - 1,
        SCC_BASE, SCC_BASE + DEV_PAGE - 1,
        chb_fd >= 0 ? chb_name : "non collegato (--scc-b)",
        scc_irq_desc,
        IDE_BASE, IDE_BASE + DEV_PAGE - 1,
        ide_img ? "" : "[nessuna immagine: -i, ABRT su ogni comando]",
        SSD_BASE, SSD_BASE + DEV_PAGE - 1,
        ssd1963_state()->w, ssd1963_state()->h, SSD_BASE + 1, SSD_BASE + 2,
        XPT_BASE, XPT_BASE + DEV_PAGE - 1,
        pen_irq_desc,
        timer_level ? "100 Hz (ogni 10 ms), autovector" : "disabilitato");
    if (ide_img)
        fprintf(stderr, "      immagine disco: %s (%llu settori, %.1f MB)\n\n",
                disk_path, (unsigned long long)ide_img_sectors, ide_img_sectors * (double)ATA_SECSZ / (1<<20));

    /* Controllo di sanita' sui vettori di reset (i primi 8 byte della ROM) */
    {
        uint32_t ssp = ((uint32_t)rom[0] << 24) | (rom[1] << 16) | (rom[2] << 8) | rom[3];
        uint32_t pc  = ((uint32_t)rom[4] << 24) | (rom[5] << 16) | (rom[6] << 8) | rom[7];
        uint8_t *b; uint32_t o;
        rkind_t ks = decode((ssp - 1) & ADDR_MASK, &b, &o);
        rkind_t kp = decode(pc & ADDR_MASK, &b, &o);
        fprintf(stderr, "Reset: SSP=%08X PC=%08X\n", ssp, pc);
        if (ssp & 1 || (ks != R_SRAM && ks != R_DRAM))
            fprintf(stderr, "[warn] SSP non valido (dispari o non in SRAM/DRAM)\n");
        if (pc & 1 || (kp != R_ROM && kp != R_SRAM && kp != R_DRAM))
            fprintf(stderr, "[warn] PC iniziale non valido (dispari o non in ROM/RAM)\n");
        if (pc > ADDR_MASK || ssp > ADDR_MASK)
            fprintf(stderr, "[warn] i vettori superano i 24 bit: gli indirizzi vengono troncati\n");
        fprintf(stderr, "\n");
    }

    setup_tty();

    if (!opt_nogui) {
        if (panel_init(&pan) == 0) panel_set_key_sink(scc_push_rx);
        else fprintf(stderr, "[panel] niente finestra: l'SSD1963 e' emulato "
                             "comunque (usa -S per salvare lo schermo)\n\n");
    }

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68010);
    m68k_pulse_reset();

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t total = 0, acc = 0, next_info = CPU_HZ / 2;
    unsigned slice = 0;

    while (!quit_flag) {
        /* La fetta si accorcia se l'SCC ha un evento prima della fine:
         * cosi' gli interrupt seriali cadono nel ciclo giusto anche a
         * 115200 baud, dove un carattere dura meno di un decimo di fetta. */
        int budget = SLICE_CYCLES;
        if (scc_irq_level) {
            uint64_t ev = z8530_next_event(cycle_base);
            if (ev < (uint64_t)budget) budget = (int)ev + 1;
        }
        in_execute = 1;
        int ran = m68k_execute(budget);
        in_execute = 0;
        total += (uint64_t)ran;
        cycle_base = total;
        acc   += (uint64_t)ran;
        poll_stdin();
        poll_chb();
        z8530_tick(cycle_base);
        if (scc_irq_level) {
            if (z8530_irq()) irq_assert(scc_irq_level);
            else             irq_release(scc_irq_level);
        }
        if (tscript_pos < tscript_n) run_touch_script(total / (CPU_HZ / 1000));
        if (timer_level && acc >= TICK_CYCLES) {
            acc -= TICK_CYCLES;
            irq_assert(timer_level);
        }
        if (pen_level) {
            /* /PENIRQ e' una linea di livello: sta bassa mentre il dito e'
             * appoggiato e si rialza durante la conversione. */
            if (xpt2046_penirq()) irq_assert(pen_level);
            else                  irq_release(pen_level);
        }
        /* In modo -f una fetta dura molto meno di 1 ms reale: non serve
         * passare da SDL a ogni giro. */
        if (!opt_fast || (++slice & 7) == 0) {
            if (panel_pump()) quit_flag = 1;
        }
        if (total >= next_info) {
            next_info = total + CPU_HZ / 2;
            if (panel_available()) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                double el = (double)(now.tv_sec - t0.tv_sec)
                          + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
                double em = (double)total / CPU_HZ;
                const ssd1963_state_t *ds = ssd1963_state();
                char b[96];
                snprintf(b, sizeof b, "%.1fs emu (%.0f%% rt)  cmd %llu  px %u",
                         em, el > 0 ? em / el * 100.0 : 0.0, ds->cmds,
                         ds->px_written);
                panel_set_info(b);
            }
        }
        if (max_cycles && total >= max_cycles) break;
        if (!opt_fast) pace(total, &t0);
    }

    if (shot_path) {
        if (ssd1963_save_ppm(shot_path) == 0)
            fprintf(stderr, "\n[fine] schermo salvato in %s\n", shot_path);
        else
            fprintf(stderr, "\n[fine] %s: impossibile salvare lo schermo\n", shot_path);
    }
    panel_shutdown();

    fprintf(stderr, "\n[fine] %llu cicli (%.3f s emulati)\n",
            (unsigned long long)total, (double)total / CPU_HZ);
    {
        const ssd1963_state_t *ds = ssd1963_state();
        const xpt2046_state_t *ts = xpt2046_state();
        const z8530_info_t *sa = z8530_info(SCC_A), *sb = z8530_info(SCC_B);
        fprintf(stderr, "       SSD1963: %llu comandi, %u pixel scritti, display %s\n"
                        "       XPT2046: %llu trasferimenti SPI\n"
                        "       SCC A:   %llu byte tx, %llu rx, %llu persi in tx, "
                        "%llu overrun (%u baud)\n"
                        "       SCC B:   %llu byte tx, %llu rx, %llu persi in tx, "
                        "%llu overrun (%u baud)\n",
                ds->cmds, ds->px_written, ds->display_on ? "ON" : "off", ts->xfers,
                sa->tx_bytes, sa->rx_bytes, sa->lost_tx, sa->overruns, sa->baud,
                sb->tx_bytes, sb->rx_bytes, sb->lost_tx, sb->overruns, sb->baud);
    }
    dump_regs();
    return 0;
}
