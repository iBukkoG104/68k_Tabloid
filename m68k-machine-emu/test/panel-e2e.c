/* panel-e2e.c - prova end-to-end della mappatura mouse -> pannello.
 *
 * Apre la finestra vera, muove il puntatore con SDL_WarpMouseInWindow e
 * controlla dove finisce il dito sul vetro. Serve a prendere la classe di
 * bug che l'autotest matematico non vede: se il renderer ha una logical
 * size, SDL converte da solo le coordinate degli eventi e il pannello
 * raddoppia la conversione, rendendo irraggiungibile tutto tranne
 * l'angolo in alto a sinistra.
 *
 * Esce 77 se non c'e' un display (il chiamante lo tratta come "saltato").
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "panel.h"
#include "ssd1963.h"
#include "xpt2046.h"

#define SCALE 2

static int fail;

static void pump(void)
{
    for (int i = 0; i < 12; i++) { panel_pump(); SDL_Delay(8); }
}

/* Aspetta che il gestore finestre abbia davvero mappato la finestra e che
 * lo spostamento del puntatore funzioni. Senza questa attesa il risultato
 * dipende da quanto e' svelto il WM. Ritorna 0 se il driver non sa
 * spostare il puntatore (Wayland) o se la finestra non prende il mouse. */
static int wait_ready(SDL_Window *w, int x, int y)
{
    for (int i = 0; i < 60; i++) {           /* fino a ~3 s */
        SDL_WarpMouseInWindow(w, x, y);
        for (int k = 0; k < 6; k++) { panel_pump(); SDL_Delay(8); }
        int mx = -1, my = -1;
        SDL_GetMouseState(&mx, &my);
        if (SDL_GetMouseFocus() == w && mx == x && my == y) return 1;
    }
    return 0;
}

static void probe(SDL_Window *w, int wx, int wy, int want_x, int want_y, int tol)
{
    SDL_WarpMouseInWindow(w, wx, wy);
    pump();
    const xpt2046_state_t *t = xpt2046_state();
    int ok = t->pen_down && abs((int)t->px - want_x) <= tol
                         && abs((int)t->py - want_y) <= tol;
    printf("  finestra (%4d,%4d) -> pannello (%3u,%3u)  atteso (%3d,%3d)  %s\n",
           wx, wy, t->px, t->py, want_x, want_y, ok ? "ok" : "FAIL");
    if (!ok) fail = 1;
}

int main(void)
{
    ssd1963_init();
    xpt2046_init();

    panel_opts_t o = { SCALE, 0, 1, 0 };
    if (panel_init(&o) != 0) {
        fprintf(stderr, "panel-e2e: nessun display, prova saltata\n");
        return 77;
    }
    SDL_Window *w = (SDL_Window *)panel_window();
    SDL_RaiseWindow(w);
    pump();

    /* invariante: niente logical size, o SDL converte gli eventi da solo */
    SDL_Renderer *r = SDL_GetRenderer(w);
    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(r, &lw, &lh);
    printf("  logical size del renderer: %dx%d (deve essere 0x0)\n", lw, lh);
    if (lw || lh) fail = 1;

    /* Il driver sa spostare il puntatore? Wayland non lo permette, e senza
     * quello non si puo' simulare il dito. SDL_GetMouseState da' sempre
     * coordinate grezze della finestra, quindi il controllo vale anche se
     * qualcuno rimette una logical size. */
    int pw = ssd1963_state()->w, ph = ssd1963_state()->h;
    if (!wait_ready(w, 10, 10) || !wait_ready(w, pw * SCALE - 10, ph * SCALE - 10)) {
        fprintf(stderr, "panel-e2e: con il driver video \"%s\" non si riesce a "
                "spostare il puntatore dentro la finestra: prova saltata\n",
                SDL_GetCurrentVideoDriver());
        panel_shutdown();
        return 77;
    }

    /* dito giu' al centro, poi trascinamento: e' il caso che si rompeva */
    SDL_WarpMouseInWindow(w, pw * SCALE / 2, ph * SCALE / 2);
    pump();
    SDL_Event down = { 0 };
    down.type = SDL_MOUSEBUTTONDOWN;
    down.button.windowID = SDL_GetWindowID(w);
    down.button.button = SDL_BUTTON_LEFT;
    down.button.state = SDL_PRESSED;
    down.button.x = pw * SCALE / 2;
    down.button.y = ph * SCALE / 2;
    SDL_PushEvent(&down);
    pump();


    printf("  finestra %dx%d (scala %d)\n", pw * SCALE, ph * SCALE, SCALE);
    probe(w, 0, 0, 0, 0, 1);
    probe(w, pw * SCALE / 2, ph * SCALE / 2, pw / 2, ph / 2, 1);
    probe(w, pw * SCALE - 1, ph * SCALE - 1, pw - 1, ph - 1, 1);
    probe(w, pw * SCALE - 1, 0, pw - 1, 0, 1);
    probe(w, 0, ph * SCALE - 1, 0, ph - 1, 1);

    /* --- schermo intero: il caso che si rompeva su monitor piu' grandi
     * del pannello, dove compaiono le bande laterali --------------- */
    SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP);
    pump(); pump();
    int ww = 0, wh = 0;
    SDL_GetWindowSize(w, &ww, &wh);
    int sc = ww / pw; if (wh / ph < sc) sc = wh / ph;
    if (sc < 1) sc = 1;
    int ox = (ww - pw * sc) / 2, oy = (wh - ph * sc) / 2;
    printf("  schermo intero %dx%d (scala %d, bande %d,%d)\n", ww, wh, sc, ox, oy);
    probe(w, ox, oy, 0, 0, 1);
    probe(w, ox + pw * sc / 2, oy + ph * sc / 2, pw / 2, ph / 2, 1);
    probe(w, ox + pw * sc - 1, oy + ph * sc - 1, pw - 1, ph - 1, 1);
    probe(w, ox + pw * sc - 1, oy, pw - 1, 0, 1);
    probe(w, ox, oy + ph * sc - 1, 0, ph - 1, 1);

    /* un punto nella cornice non e' vetro: il dito si stacca */
    if (oy >= 2) {
        SDL_WarpMouseInWindow(w, ww / 2, oy / 2);
        pump();
        int up = !xpt2046_state()->pen_down;
        printf("  punto nella cornice (%d,%d) -> dito %s  %s\n",
               ww / 2, oy / 2, up ? "staccato" : "ancora appoggiato",
               up ? "ok" : "FAIL");
        if (!up) fail = 1;
    }

    panel_shutdown();
    printf(fail ? "panel-e2e: FALLITO\n" : "panel-e2e: ok\n");
    return fail;
}
