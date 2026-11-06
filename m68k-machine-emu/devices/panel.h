/*
 * panel.h - finestra di simulazione del pannello 7" 800x480 + touch.
 *
 * Mostra il frame buffer dell'SSD1963 (vedi ssd1963.c) in una finestra SDL2
 * e trasforma il mouse / il touch dell'host in letture dell'XPT2046.
 * Se SDL2 non c'e' al momento della compilazione (make NOGUI=1) oppure non
 * c'e' un display a runtime, tutte le funzioni diventano no-op e l'harness
 * continua a girare headless: l'emulazione del chip e' comunque attiva e si
 * puo' salvare lo schermo con -S.
 */
#ifndef PANEL_H
#define PANEL_H

typedef struct {
    int   scale;        /* 1..4 ingrandimento iniziale (0 = automatico) */
    int   fullscreen;
    int   show_touch;   /* disegna il mirino dove si tocca              */
    int   vsync;
} panel_opts_t;

/* 0 = finestra aperta, <0 = non disponibile (si continua headless). */
int  panel_init(const panel_opts_t *opts);
void panel_shutdown(void);
int  panel_available(void);

/* Eventi + presentazione (throttle interno a ~60 Hz).
 * Ritorna 1 se l'utente ha chiuso la finestra o premuto Ctrl-Q. */
int  panel_pump(void);

/* I tasti premuti nella finestra finiscono qui (console SCC canale A). */
void panel_set_key_sink(void (*fn)(unsigned char c));

/* Testo extra nella barra del titolo (es. MHz emulati). */
void panel_set_info(const char *s);

/* Autotest della geometria finestra <-> pannello: verifica centratura,
 * scala intera e mappatura del mouse (compreso il caso schermo intero su
 * monitor piu' grandi del pannello). Non apre nessuna finestra.
 * 0 = tutto a posto. */
int  panel_selftest(void);

/* La SDL_Window del pannello (NULL se non c'e' finestra), come void* per
 * non tirare dentro gli header di SDL. Serve ai test end-to-end. */
void *panel_window(void);

#endif
