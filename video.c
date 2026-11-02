#include <string.h>
#include <stdio.h>
#include <stdlib.h>
/* janela SDL2 + teclado (compilar com -DUSE_SDL) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef USE_SDL
#include <SDL.h>
void gpu_render(uint32_t*, int*, int*);
void pad_set(uint32_t); void pad_set2(uint32_t); void pad_has2(int);
static SDL_Window* win; static SDL_Renderer* ren; static SDL_Texture* tex; static int tw, th;
static uint32_t fb[640 * 480];
static uint32_t keys, keys2; static SDL_AudioDeviceID adev; static SDL_GameController* gcs[2];
static void gc_open(void) {
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) {
        SDL_GameController* c = SDL_GameControllerOpen(i); if (!c) continue;
        int dup = 0; for (int k = 0; k < 2; k++) if (gcs[k] == c) dup = 1;
        if (dup) continue;
        for (int k = 0; k < 2; k++) if (!gcs[k]) { gcs[k] = c; fprintf(stderr, "[pad] controle %d: %s\n", k + 1, SDL_GameControllerName(c)); break; }
    }
}
static uint32_t gc_bits(SDL_GameController* gc) {
    if (!gc) return 0; uint32_t m = 0;
#define B(btn, bit) if (SDL_GameControllerGetButton(gc, btn)) m |= 1u << (bit)
    B(SDL_CONTROLLER_BUTTON_BACK, 0); B(SDL_CONTROLLER_BUTTON_LEFTSTICK, 1); B(SDL_CONTROLLER_BUTTON_RIGHTSTICK, 2); B(SDL_CONTROLLER_BUTTON_START, 3);
    B(SDL_CONTROLLER_BUTTON_DPAD_UP, 4); B(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 5); B(SDL_CONTROLLER_BUTTON_DPAD_DOWN, 6); B(SDL_CONTROLLER_BUTTON_DPAD_LEFT, 7);
    B(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 10); B(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 11);
    B(SDL_CONTROLLER_BUTTON_Y, 12); B(SDL_CONTROLLER_BUTTON_B, 13); B(SDL_CONTROLLER_BUTTON_A, 14); B(SDL_CONTROLLER_BUTTON_X, 15);
#undef B
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) m |= 1u << 8;
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) m |= 1u << 9;
    int ax = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
    if (ay < -16000) m |= 1u << 4; if (ax > 16000) m |= 1u << 5; if (ay > 16000) m |= 1u << 6; if (ax < -16000) m |= 1u << 7;
    return m;
}
int video_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 0; }
    win = SDL_CreateWindow("Dragon Ball GT Final Bout (recomp)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720, SDL_WINDOW_RESIZABLE);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { SDL_AudioSpec w = { 0 }, h; w.freq = 44100; w.format = AUDIO_S16SYS; w.channels = 2; w.samples = 1024; adev = SDL_OpenAudioDevice(NULL, 0, &w, &h, 0); if (adev) SDL_PauseAudioDevice(adev, 0); }
    gc_open();
    return win && ren;
}
static uint32_t keybit2(SDL_Keycode k) {   /* jogador 2: I J K L, U O Y H (botoes), Enter do teclado numerico = Start */
    switch (k) {
    case SDLK_KP_ENTER: return 1u << 3;  case SDLK_RCTRL: return 1u << 0;
    case SDLK_i: return 1u << 4;  case SDLK_l: return 1u << 5;  case SDLK_k: return 1u << 6;  case SDLK_j: return 1u << 7;
    case SDLK_p: return 1u << 12; case SDLK_o: return 1u << 13; case SDLK_u: return 1u << 14; case SDLK_y: return 1u << 15;
    case SDLK_t: return 1u << 10; case SDLK_g: return 1u << 11; case SDLK_h: return 1u << 8; case SDLK_n: return 1u << 9;
    }
    return 0;
}
static uint32_t keybit(SDL_Keycode k) {
    switch (k) {
    case SDLK_RETURN: return 1u << 3;  case SDLK_BACKSPACE: case SDLK_RSHIFT: return 1u << 0;
    case SDLK_UP: return 1u << 4;      case SDLK_RIGHT: return 1u << 5;
    case SDLK_DOWN: return 1u << 6;    case SDLK_LEFT: return 1u << 7;
    case SDLK_e: return 1u << 8;       case SDLK_r: return 1u << 9;
    case SDLK_q: return 1u << 10;      case SDLK_w: return 1u << 11;
    case SDLK_s: return 1u << 12;      case SDLK_x: return 1u << 13;   /* Triangulo, Circulo */
    case SDLK_z: return 1u << 14;      case SDLK_a: return 1u << 15;   /* Cross, Quadrado */
    }
    return 0;
}
int audio_queued(void) { return adev ? (int)SDL_GetQueuedAudioSize(adev) / 4 : -1; }
void audio_push(const int16_t* s, int n) { if (adev) SDL_QueueAudio(adev, s, n * 4); }
static int show_fps; static float fps_val;
static void fps_draw(uint32_t* px, int w, int h, float v) {
    static const uint16_t g[10] = { 0x7b6f, 0x2c97, 0x73e7, 0x73cf, 0x5bc9, 0x79cf, 0x79ef, 0x7249, 0x7bef, 0x7bcf };  /* 3x5, linha a linha, bit14 = canto sup. esq. */
    char t[16]; snprintf(t, sizeof t, "%d FPS", (int)(v + 0.5f));
    const uint16_t F = 0x79a4, P = 0x7be4, S = 0x79cf;
    int sc = 2, x0 = 4, y0 = 4, n = (int)strlen(t);
    for (int j = 0; j < 7 * sc + 2; j++) for (int i = 0; i < (n * 4 + 1) * sc + 2; i++) if (x0 - 1 + i < w && y0 - 1 + j < h) px[(y0 - 1 + j) * w + x0 - 1 + i] = 0xff000000;
    for (int c = 0; c < n; c++) {
        uint16_t gl = t[c] >= '0' && t[c] <= '9' ? g[t[c] - '0'] : t[c] == 'F' ? F : t[c] == 'P' ? P : t[c] == 'S' ? S : 0;
        for (int r = 0; r < 5; r++) for (int q = 0; q < 3; q++) if (gl & (1 << (14 - r * 3 - q)))
            for (int a = 0; a < sc; a++) for (int b = 0; b < sc; b++) { int X = x0 + (c * 4 + q) * sc + b, Y = y0 + r * sc + a; if (X < w && Y < h) px[Y * w + X] = 0xff00ff00; }
    }
}
void video_present(void) {
    SDL_Event e;
    static uint32_t t0, nf; if (!t0) { t0 = SDL_GetTicks(); show_fps = getenv("FPS") != NULL; } nf++;
    { uint32_t now = SDL_GetTicks(); if (now - t0 >= 500) { fps_val = nf * 1000.0f / (now - t0); nf = 0; t0 = now; } }
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) exit(0);
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F3 && !e.key.repeat) show_fps = !show_fps;
        if (e.type == SDL_KEYDOWN && !e.key.repeat && (e.key.keysym.sym == SDLK_F11 || (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)))) {
            SDL_Window* w = SDL_RenderGetWindow(ren); { int fs = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0; SDL_SetWindowFullscreen(w, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP); SDL_ShowCursor(fs ? SDL_ENABLE : SDL_DISABLE); }
            continue;
        }
        if (e.type == SDL_KEYDOWN && !e.key.repeat) { keys |= keybit(e.key.keysym.sym); keys2 |= keybit2(e.key.keysym.sym); }
        if (e.type == SDL_KEYUP) { keys &= ~keybit(e.key.keysym.sym); keys2 &= ~keybit2(e.key.keysym.sym); }
        if (e.type == SDL_CONTROLLERDEVICEADDED) gc_open();
        if (e.type == SDL_CONTROLLERDEVICEREMOVED) { for (int k = 0; k < 2; k++) if (gcs[k] && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gcs[k]))) { SDL_GameControllerClose(gcs[k]); gcs[k] = NULL; } gc_open(); }
    }
    pad_set(keys | gc_bits(gcs[0]));
    pad_has2(1); pad_set2(keys2 | gc_bits(gcs[1]));
    int w, h; gpu_render(fb, &w, &h);
    if (!tex || w != tw || h != th) { if (tex) SDL_DestroyTexture(tex); tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h); tw = w; th = h; }
    if (show_fps) fps_draw(fb, w, h, fps_val);
    SDL_UpdateTexture(tex, NULL, fb, w * 4);
    SDL_RenderClear(ren);
    int ow, oh; SDL_GetRendererOutputSize(ren, &ow, &oh);
    float s = (float)ow / (w * (h < 480 ? 1 : 1)); float s2 = (float)oh / (h < 300 ? 240 : h) * (h < 300 ? 1 : 1);
    /* mantem proporcao 4:3 */
    int dw = ow, dh = ow * 3 / 4; if (dh > oh) { dh = oh; dw = oh * 4 / 3; }
    (void)s; (void)s2;
    SDL_Rect dst = { (ow - dw) / 2, (oh - dh) / 2, dw, dh };
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_RenderPresent(ren);
}
#else
int video_init(void) { return 0; }
void video_present(void) {}
int audio_queued(void) { return -1; }
void audio_push(const int16_t* s, int n) {}
#endif
