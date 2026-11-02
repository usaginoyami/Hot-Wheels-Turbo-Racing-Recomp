/* CD-XA ADPCM: decodifica setores de audio e mistura na saida do SPU */
#include <stdint.h>
#include <string.h>

#define XR (1 << 17)
static int16_t ring[XR][2]; static uint32_t wpos, rpos; static double frac; static double step = 37800.0 / 44100.0;
static int old[2][2];
static const int f0[4] = { 0, 60, 115, 98 }, f1[4] = { 0, 0, -52, -55 };

static void push(int l, int r) {
    if (wpos - rpos >= XR - 1) rpos = wpos - XR / 2;          /* estouro: descarta o mais antigo */
    ring[wpos & (XR - 1)][0] = l; ring[wpos & (XR - 1)][1] = r; wpos++;
}
void xa_feed(const uint8_t* sec) {
    int coding = sec[19]; int stereo = coding & 1; int half = (coding >> 2) & 1;
    step = (half ? 18900.0 : 37800.0) / 44100.0;
    const uint8_t* d = sec + 24;
    for (int g = 0; g < 18; g++, d += 128) {
        int16_t out[8][28];
        for (int b = 0; b < 8; b++) {
            int hdr = d[4 + b], shift = hdr & 15, filter = (hdr >> 4) & 3; if (shift > 12) shift = 9;
            int ch = stereo ? (b & 1) : 0;
            for (int k = 0; k < 28; k++) {
                int byte = d[16 + k * 4 + (b >> 1)]; int nib = (b & 1) ? byte >> 4 : byte & 15;
                int s = ((int16_t)(nib << 12)) >> shift;
                s += (old[ch][0] * f0[filter] + old[ch][1] * f1[filter] + 32) >> 6;
                if (s > 32767) s = 32767; if (s < -32768) s = -32768;
                old[ch][1] = old[ch][0]; old[ch][0] = s; out[b][k] = s;
            }
        }
        if (stereo) { for (int p = 0; p < 4; p++) for (int k = 0; k < 28; k++) push(out[p * 2][k], out[p * 2 + 1][k]); }
        else { for (int b = 0; b < 8; b++) for (int k = 0; k < 28; k++) push(out[b][k], out[b][k]); }
    }
}
int xa_mix(int* l, int* r) {           /* chamado a cada amostra de saida (44100 Hz) */
    if (rpos >= wpos) return 0;
    *l = ring[rpos & (XR - 1)][0]; *r = ring[rpos & (XR - 1)][1];
    frac += step; while (frac >= 1.0) { frac -= 1.0; if (rpos < wpos) rpos++; }
    return 1;
}
void xa_reset(void) { rpos = wpos = 0; memset(old, 0, sizeof old); }
