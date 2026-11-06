/*
 * panel.c - finestra SDL2 per il pannello SSD1963 + ingresso touch XPT2046.
 */
#include "panel.h"
#include "ssd1963.h"
#include "xpt2046.h"

#include <stdio.h>
#include <string.h>

#ifndef HAVE_SDL2

int  panel_init(const panel_opts_t *o) { (void)o; return -1; }
void panel_shutdown(void) {}
int  panel_available(void) { return 0; }
int  panel_pump(void) { return 0; }
void panel_set_key_sink(void (*fn)(unsigned char)) { (void)fn; }
void panel_set_info(const char *s) { (void)s; }
int  panel_selftest(void) { return 0; }   /* senza SDL non c'e' geometria */
void *panel_window(void) { return 0; }

#else /* ---------------------------------------------------------------- */

#include <SDL.h>

#define PRESENT_MS 16                 /* ~60 Hz */

/* ------------------------------------------------------------------ */
/* Geometria: una sola verita' per disegno e per mouse                 */
/* ------------------------------------------------------------------ */
/* NON usiamo SDL_RenderSetLogicalSize. Da SDL 2.0.18 il renderer, quando
 * ha una logical size, converte da solo le coordinate degli eventi mouse:
 * chi poi chiama anche SDL_RenderWindowToLogical divide due volte e, con
 * la finestra ingrandita (p.es. a schermo intero, dove la scala e' 2),
 * raggiunge solo il quarto in alto a sinistra del pannello. Qui la
 * trasformazione e' nostra, calcolata una volta e usata in entrambi i
 * versi, quindi non dipende da cosa fa la versione di SDL con cui si
 * compila. */
typedef struct { int x, y, w, h; double s; } fit_t;

/* Pannello pan_w x pan_h centrato in un'area out_w x out_h: scala intera
 * finche' ci sta (pixel netti), altrimenti riduce per farcelo entrare. */
static fit_t fit_panel(int out_w, int out_h, int pan_w, int pan_h)
{
    fit_t f = { 0, 0, 0, 0, 0.0 };
    if (out_w <= 0 || out_h <= 0 || pan_w <= 0 || pan_h <= 0) return f;
    double sx = (double)out_w / pan_w;
    double sy = (double)out_h / pan_h;
    double sc = sx < sy ? sx : sy;
    if (sc >= 1.0) sc = (double)(int)sc;
    f.s = sc;
    f.w = (int)(pan_w * sc);
    f.h = (int)(pan_h * sc);
    f.x = (out_w - f.w) / 2;
    f.y = (out_h - f.h) / 2;
    return f;
}

/* Inversa: punto dell'area di disegno -> pixel del pannello.
 * Ritorna 0 se cade fuori dal vetro (nella cornice). */
static int unfit_panel(const fit_t *f, double ox, double oy,
                       int pan_w, int pan_h, int *px, int *py)
{
    if (f->s <= 0.0) return 0;
    double x = (ox - f->x) / f->s;
    double y = (oy - f->y) / f->s;
    if (x < 0 || y < 0 || x >= pan_w || y >= pan_h) return 0;
    *px = (int)x;
    *py = (int)y;
    return 1;
}

static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *tex;
static int   tex_w, tex_h;
static int   active;
static int   show_touch = 1;
static int   quit_req;
static unsigned last_seq = (unsigned)-1;
static Uint32 last_present;
static Uint32 last_title;
static unsigned frames;
static char   info[96];
static int    shot_n;
static int    touching;
static void (*key_sink)(unsigned char);

static void push_key(unsigned char c) { if (key_sink) key_sink(c); }

static void push_str(const char *s) { while (*s) push_key((unsigned char)*s++); }

static void make_texture(int w, int h)
{
    if (tex) SDL_DestroyTexture(tex);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
                            SDL_TEXTUREACCESS_STREAMING, w, h);
    tex_w = w; tex_h = h;
    last_seq = (unsigned)-1;
}

/* Rettangolo del pannello dentro l'area di disegno, in pixel del renderer. */
static fit_t panel_fit(void)
{
    const ssd1963_state_t *s = ssd1963_state();
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    return fit_panel(ow, oh, s->w, s->h);
}

/* Indice del monitor su cui sta il puntatore. Su un setup multi-monitor
 * serve perche' la finestra (e quindi il full screen, che SDL manda sul
 * display dove la finestra si trova) nasca dove l'utente sta guardando,
 * invece che sempre sul display 0. Se il driver non sa dire dov'e' il
 * puntatore (Wayland non lo espone) si ripiega sul display 0. */
static int display_under_cursor(void)
{
    int mx = 0, my = 0;
    SDL_GetGlobalMouseState(&mx, &my);
    if (mx == 0 && my == 0) return 0;
    int n = SDL_GetNumVideoDisplays();
    for (int i = 0; i < n; i++) {
        SDL_Rect b;
        if (SDL_GetDisplayBounds(i, &b) == 0 &&
            mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h)
            return i;
    }
    return 0;
}

int panel_init(const panel_opts_t *o)
{
    panel_opts_t def = { 0, 0, 1, 0 };
    if (!o) o = &def;
    show_touch = o->show_touch;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[panel] SDL_Init: %s (si continua headless)\n", SDL_GetError());
        return -1;
    }

    const ssd1963_state_t *s = ssd1963_state();
    int w = s->w, h = s->h;
    int disp = display_under_cursor();
    int scale = o->scale;
    if (scale <= 0) {
        SDL_DisplayMode dm;
        scale = 1;
        if (SDL_GetCurrentDisplayMode(disp, &dm) == 0) {
            while (scale < 3 && w * (scale + 1) <= dm.w - 100
                             && h * (scale + 1) <= dm.h - 150) scale++;
        }
    }
    if (scale > 4) scale = 4;

    Uint32 flags = SDL_WINDOW_RESIZABLE;
    if (o->fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    win = SDL_CreateWindow("m68k PDA - pannello 7\" 800x480 (SSD1963 + XPT2046)",
                           SDL_WINDOWPOS_CENTERED_DISPLAY(disp),
                           SDL_WINDOWPOS_CENTERED_DISPLAY(disp),
                           w * scale, h * scale, flags);
    if (!win) {
        fprintf(stderr, "[panel] SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }
    Uint32 rflags = SDL_RENDERER_ACCELERATED | (o->vsync ? SDL_RENDERER_PRESENTVSYNC : 0u);
    ren = SDL_CreateRenderer(win, -1, rflags);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        fprintf(stderr, "[panel] SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win); win = NULL; SDL_Quit();
        return -1;
    }
    make_texture(w, h);
    /* Invariante del modulo: il renderer NON deve avere una logical size.
     * Se ce l'ha, da SDL 2.0.18 in poi il renderer converte da solo le
     * coordinate degli eventi mouse, e la mappatura di touch_at() le
     * convertirebbe una seconda volta: a schermo intero resterebbe
     * raggiungibile solo l'angolo in alto a sinistra del pannello. */
    {
        int lw = 0, lh = 0;
        SDL_RenderGetLogicalSize(ren, &lw, &lh);
        if (lw || lh) {
            fprintf(stderr, "[panel] logical size %dx%d attiva: la tolgo, "
                            "altrimenti il mouse viene convertito due volte\n", lw, lh);
            SDL_RenderSetLogicalSize(ren, 0, 0);
        }
    }

    SDL_StartTextInput();
    active = 1;
    last_present = SDL_GetTicks();
    last_title   = last_present;

    fprintf(stderr,
        "[panel] finestra %dx%d (x%d) sul monitor %d di %d. F1 mirino touch,\n"
        "        F11 fullscreen, F12 screenshot PPM, Ctrl-Q esci.\n"
        "        La tastiera va sulla console SCC A.\n",
        w, h, scale, disp, SDL_GetNumVideoDisplays());
    return 0;
}

void panel_shutdown(void)
{
    if (!active) return;
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    tex = NULL; ren = NULL; win = NULL;
    SDL_Quit();
    active = 0;
}

int panel_available(void) { return active; }
void *panel_window(void) { return win; }
void panel_set_key_sink(void (*fn)(unsigned char)) { key_sink = fn; }
void panel_set_info(const char *s)
{
    snprintf(info, sizeof info, "%s", s ? s : "");
}

/* ------------------------------------------------------------------ */
static void screenshot(void)
{
    char path[64];
    snprintf(path, sizeof path, "screen%03d.ppm", shot_n++);
    if (ssd1963_save_ppm(path) == 0)
        fprintf(stderr, "[panel] screenshot: %s\n", path);
    else
        fprintf(stderr, "[panel] screenshot %s: errore\n", path);
}

/* wx,wy sono coordinate della finestra, come arrivano negli eventi SDL
 * (nessuna logical size, quindi SDL non le ha toccate). */
static void touch_at(int wx, int wy, int down)
{
    const ssd1963_state_t *s = ssd1963_state();
    int ww = 0, wh = 0, ow = 0, oh = 0, px, py;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    /* su schermi HiDPI l'area di disegno e' piu' grande della finestra */
    double ox = ww > 0 ? (double)wx * ow / ww : wx;
    double oy = wh > 0 ? (double)wy * oh / wh : wy;

    fit_t f = panel_fit();
    if (!unfit_panel(&f, ox, oy, s->w, s->h, &px, &py)) {
        /* fuori dal vetro: come togliere il dito */
        if (!down) return;
        xpt2046_set_touch(0, 0, 0, s->w, s->h);
        return;
    }
    xpt2046_set_touch(down, px, py, s->w, s->h);
}

static void on_key(const SDL_KeyboardEvent *k)
{
    SDL_Keycode c = k->keysym.sym;
    Uint16 mod = k->keysym.mod;

    if ((mod & KMOD_CTRL) && (c == SDLK_q)) { quit_req = 1; return; }

    switch (c) {
    case SDLK_F1:  show_touch = !show_touch; return;
    case SDLK_F11: {
        Uint32 f = SDL_GetWindowFlags(win);
        SDL_SetWindowFullscreen(win, (f & SDL_WINDOW_FULLSCREEN_DESKTOP)
                                     ? 0u : SDL_WINDOW_FULLSCREEN_DESKTOP);
        return;
    }
    case SDLK_F12: screenshot(); return;
    default: break;
    }

    /* tasti non stampabili verso la console SCC */
    switch (c) {
    case SDLK_RETURN: case SDLK_KP_ENTER: push_key('\r'); return;
    case SDLK_BACKSPACE: push_key(0x08); return;
    case SDLK_TAB:       push_key('\t'); return;
    case SDLK_ESCAPE:    push_key(0x1B); return;
    case SDLK_DELETE:    push_key(0x7F); return;
    case SDLK_UP:    push_str("\033[A"); return;
    case SDLK_DOWN:  push_str("\033[B"); return;
    case SDLK_RIGHT: push_str("\033[C"); return;
    case SDLK_LEFT:  push_str("\033[D"); return;
    case SDLK_HOME:  push_str("\033[H"); return;
    case SDLK_END:   push_str("\033[F"); return;
    default: break;
    }
    if ((mod & KMOD_CTRL) && c >= SDLK_a && c <= SDLK_z)
        push_key((unsigned char)(c - SDLK_a + 1));
}

static void events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: quit_req = 1; break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_CLOSE) quit_req = 1;
            break;
        case SDL_KEYDOWN: on_key(&e.key); break;
        case SDL_TEXTINPUT: {
            for (const char *p = e.text.text; *p; p++)
                if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7F)
                    push_key((unsigned char)*p);
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_LEFT) {
                touching = 1;
                touch_at(e.button.x, e.button.y, 1);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT) {
                touching = 0;
                const ssd1963_state_t *s = ssd1963_state();
                xpt2046_set_touch(0, (int)xpt2046_state()->px,
                                  (int)xpt2046_state()->py, s->w, s->h);
            }
            break;
        case SDL_MOUSEMOTION:
            if (touching) touch_at(e.motion.x, e.motion.y, 1);
            break;
        case SDL_FINGERDOWN: case SDL_FINGERMOTION: {
            int ww, wh;
            SDL_GetWindowSize(win, &ww, &wh);
            touching = 1;
            touch_at((int)(e.tfinger.x * ww), (int)(e.tfinger.y * wh), 1);
            break;
        }
        case SDL_FINGERUP: {
            const ssd1963_state_t *s = ssd1963_state();
            touching = 0;
            xpt2046_set_touch(0, (int)xpt2046_state()->px,
                              (int)xpt2046_state()->py, s->w, s->h);
            break;
        }
        default: break;
        }
    }
}

/* Il mirino si disegna in pixel del renderer, come il pannello. */
static void draw_touch_marker(const fit_t *f)
{
    const xpt2046_state_t *t = xpt2046_state();
    if (!show_touch || !t->pen_down || f->s <= 0.0) return;
    int x = f->x + (int)((t->px + 0.5) * f->s);
    int y = f->y + (int)((t->py + 0.5) * f->s);
    int in = (int)(3 * f->s); if (in < 3) in = 3;
    int out = (int)(12 * f->s); if (out < 12) out = 12;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 255, 64, 64, 200);
    SDL_RenderDrawLine(ren, x - out, y, x - in, y);
    SDL_RenderDrawLine(ren, x + in, y, x + out, y);
    SDL_RenderDrawLine(ren, x, y - out, x, y - in);
    SDL_RenderDrawLine(ren, x, y + in, x, y + out);
    int d = (int)f->s; if (d < 1) d = 1;
    SDL_Rect r = { x - d, y - d, 2 * d + 1, 2 * d + 1 };
    SDL_RenderFillRect(ren, &r);
}

static void present(void)
{
    const ssd1963_state_t *s = ssd1963_state();
    if (s->w != tex_w || s->h != tex_h) make_texture(s->w, s->h);

    if (s->frame_seq != last_seq) {
        void *pix; int pitch;
        if (SDL_LockTexture(tex, NULL, &pix, &pitch) == 0) {
            ssd1963_compose((unsigned short *)pix, pitch / 2);
            SDL_UnlockTexture(tex);
            last_seq = s->frame_seq;
        }
    }

    /* backlight PWM: il duty scala la luminosita' della finestra */
    unsigned bl = s->backlight;
    Uint8 m = (Uint8)(bl > 255 ? 255 : bl);
    SDL_SetTextureColorMod(tex, m, m, m);

    fit_t f = panel_fit();
    SDL_Rect dst = { f.x, f.y, f.w, f.h };

    SDL_SetRenderDrawColor(ren, 24, 24, 28, 255);     /* cornice */
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, &dst);
    draw_touch_marker(&f);
    SDL_RenderPresent(ren);
    frames++;
}

static void update_title(Uint32 now)
{
    const ssd1963_state_t *s = ssd1963_state();
    const xpt2046_state_t *t = xpt2046_state();
    char buf[256];
    unsigned ms = now - last_title ? now - last_title : 1;
    snprintf(buf, sizeof buf,
             "m68k PDA  %ux%u  display %s  bl %u%%  %s%s  |  %u fps%s%s",
             s->w, s->h, s->display_on ? "ON" : "off",
             s->backlight * 100u / 255u,
             t->pen_down ? "touch " : "",
             t->pen_down ? "" : "no touch",
             frames * 1000u / ms,
             info[0] ? "  |  " : "", info);
    if (t->pen_down) {
        char b2[64];
        snprintf(b2, sizeof b2, " @%u,%u (adc %u,%u)", t->px, t->py, t->x_adc, t->y_adc);
        strncat(buf, b2, sizeof buf - strlen(buf) - 1);
    }
    SDL_SetWindowTitle(win, buf);
    frames = 0;
    last_title = now;
}

/* ------------------------------------------------------------------ */
/* Autotest della geometria (non serve ne' finestra ne' video driver)  */
/* ------------------------------------------------------------------ */
int panel_selftest(void)
{
    int bad = 0;
    /* brx,bry = pixel del pannello sotto l'ultimo pixel del vetro */
    struct { int ow, oh, pw, ph; int x, y, w, h; double sc; int brx, bry; } c[] = {
        /* schermo intero 1080p: scala 2, pannello centrato con bande    */
        { 1920, 1080, 800, 480,  160,  60, 1600, 960, 2.0, 799, 479 },
        /* finestra a scala 1                                            */
        {  800,  480, 800, 480,    0,   0,  800, 480, 1.0, 799, 479 },
        /* finestra piu' piccola del pannello: riduce senza tagliare.    */
        /* a scala 0,5 un pixel di finestra ne copre due di pannello     */
        {  400,  240, 800, 480,    0,   0,  400, 240, 0.5, 798, 478 },
        /* pannello ruotato dal firmware                                 */
        { 1920, 1080, 480, 800,  720, 140,  480, 800, 1.0, 479, 799 },
        /* 4K: scala 4, non 4.5                                          */
        { 3840, 2160, 800, 480,  320, 120, 3200,1920, 4.0, 799, 479 },
    };

    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        fit_t f = fit_panel(c[i].ow, c[i].oh, c[i].pw, c[i].ph);
        if (f.x != c[i].x || f.y != c[i].y || f.w != c[i].w || f.h != c[i].h
            || f.s != c[i].sc) {
            fprintf(stderr, "[panel] FAIL fit %dx%d pannello %dx%d: "
                    "ho %d,%d %dx%d s=%.3f, mi aspetto %d,%d %dx%d s=%.3f\n",
                    c[i].ow, c[i].oh, c[i].pw, c[i].ph,
                    f.x, f.y, f.w, f.h, f.s,
                    c[i].x, c[i].y, c[i].w, c[i].h, c[i].sc);
            bad++;
            continue;
        }
        /* i quattro angoli del vetro devono mappare sui quattro angoli
         * del pannello, e la cornice deve restare fuori */
        int px, py;
        struct { int ox, oy, in, px, py; } pt[] = {
            { f.x,             f.y,             1, 0,          0          },
            { f.x + f.w - 1,   f.y,             1, c[i].brx,   0          },
            { f.x,             f.y + f.h - 1,   1, 0,          c[i].bry   },
            { f.x + f.w - 1,   f.y + f.h - 1,   1, c[i].brx,   c[i].bry   },
            { f.x - 1,         f.y,             0, 0, 0 },
            { f.x,             f.y - 1,         0, 0, 0 },
            { f.x + f.w,       f.y,             0, 0, 0 },
            { f.x,             f.y + f.h,       0, 0, 0 },
        };
        for (size_t k = 0; k < sizeof pt / sizeof pt[0]; k++) {
            if (pt[k].ox < 0 || pt[k].oy < 0) continue;   /* senza bande */
            px = py = -1;
            int in = unfit_panel(&f, pt[k].ox, pt[k].oy, c[i].pw, c[i].ph, &px, &py);
            if (in != pt[k].in || (in && (px != pt[k].px || py != pt[k].py))) {
                fprintf(stderr, "[panel] FAIL mappa %dx%d: punto (%d,%d) -> "
                        "dentro=%d (%d,%d), mi aspetto dentro=%d (%d,%d)\n",
                        c[i].ow, c[i].oh, pt[k].ox, pt[k].oy,
                        in, px, py, pt[k].in, pt[k].px, pt[k].py);
                bad++;
            }
        }
    }
    if (!bad)
        fprintf(stderr, "[panel] autotest geometria: ok "
                        "(scala intera, centratura, mappatura del mouse)\n");
    return bad ? -1 : 0;
}

int panel_pump(void)
{
    if (!active) return 0;
    events();
    Uint32 now = SDL_GetTicks();
    if (now - last_present >= PRESENT_MS) {
        last_present = now;
        present();
        if (now - last_title >= 500) update_title(now);
    }
    return quit_req;
}

#endif /* HAVE_SDL2 */
