/* CD-DA (trilhas de audio): le o .cue ao lado do .bin, monta a TOC e toca as faixas pelo mixer da SPU */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>

#define MAXT 99
typedef struct { char path[1024]; FILE* f; long nsec; long file_begin; long idx1; } Trk;   /* posicoes em setores absolutos (inclui os 150 do lead-in) */
static Trk trk[MAXT + 1]; static int ntrk, leadout, loaded;
static int playing, ended_flag; static long play_pos;      /* setor absoluto atual */
static int samp_in_sec;                                    /* amostra dentro do setor (0..587) */
static int16_t secbuf[588 * 2]; static long secbuf_lba = -1;
static int stop_at_track_end;                              /* AutoPause */
static int cur_track_of(long lba) { int t = 1; for (int i = 1; i <= ntrk; i++) if (lba >= trk[i].idx1) t = i; return t; }

static int cmp_ci(const char* a, const char* b) { for (; *a && *b; a++, b++) { char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 1; } return *a != *b; }

/* chamado por hw_set_bin com o caminho do .bin da trilha 1 */
void cdda_init(const char* binpath, long track1_sectors) {
    char dir[1024]; strncpy(dir, binpath, sizeof dir - 1); dir[sizeof dir - 1] = 0;
    char* sl = strrchr(dir, '/'); char* bs = strrchr(dir, '\\'); if (bs > sl) sl = bs;
    if (sl) *sl = 0; else strcpy(dir, ".");
    char cue[1100] = ""; DIR* d = opendir(dir);
    if (d) { struct dirent* e; while ((e = readdir(d))) { size_t n = strlen(e->d_name); if (n > 4 && !cmp_ci(e->d_name + n - 4, ".cue")) { snprintf(cue, sizeof cue, "%s/%s", dir, e->d_name); break; } } closedir(d); }
    ntrk = 0; loaded = 0;
    FILE* f = cue[0] ? fopen(cue, "r") : NULL;
    if (!f) { fprintf(stderr, "[cdda] sem .cue ao lado do .bin: trilhas de audio indisponiveis\n"); return; }
    char line[1200]; int cur = 0; char fname[1024] = "";
    long cum = 150;                                         /* inicio do arquivo corrente (absoluto) */
    long idx0 = -1, idx1 = -1; int first_file = 1;
    while (fgets(line, sizeof line, f)) {
        char* p = line; while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "FILE", 4)) { char* q = strchr(p, '"'); if (q) { char* r = strchr(q + 1, '"'); if (r) { *r = 0; strncpy(fname, q + 1, sizeof fname - 1); } } }
        else if (!strncmp(p, "TRACK", 5)) { cur = atoi(p + 5); if (cur < 1 || cur > MAXT) cur = 0; else { if (cur > ntrk) ntrk = cur; snprintf(trk[cur].path, sizeof trk[cur].path, "%s/%s", dir, fname);
                long ns = 0;
                if (cur == 1) ns = track1_sectors; else { FILE* t = fopen(trk[cur].path, "rb"); if (t) { fseek(t, 0, SEEK_END); ns = ftell(t) / 2352; fclose(t); } else ns = 75 * 180; }
                trk[cur].nsec = ns; trk[cur].file_begin = cum; cum += ns; idx0 = idx1 = -1; (void)first_file; } }
        else if (!strncmp(p, "INDEX", 5) && cur) { int n, mm, ss, ff; if (sscanf(p + 5, "%d %d:%d:%d", &n, &mm, &ss, &ff) == 4) { long o = (mm * 60 + ss) * 75 + ff; if (n == 1) trk[cur].idx1 = trk[cur].file_begin + o; (void)idx0; (void)idx1; } }
    }
    fclose(f); leadout = (int)cum; loaded = ntrk > 0;
    fprintf(stderr, "[cdda] %d faixas; fim do disco em setor %d\n", ntrk, leadout);
}
int cdda_ntracks(void) { return loaded ? ntrk : 1; }
long cdda_track_start(int t) { if (!loaded) return t ? 150 : 150 + 75 * 60; if (t == 0 || t > ntrk) return leadout; return trk[t].idx1; }

static void load_sector(long lba) {
    if (lba == secbuf_lba) return; secbuf_lba = lba; memset(secbuf, 0, sizeof secbuf);
    for (int i = 1; i <= ntrk; i++) if (lba >= trk[i].file_begin && lba < trk[i].file_begin + trk[i].nsec) {
        if (i == 1) return;                                 /* trilha de dados: silencio */
        if (!trk[i].f) trk[i].f = fopen(trk[i].path, "rb");
        if (trk[i].f && fseek(trk[i].f, (lba - trk[i].file_begin) * 2352L, SEEK_SET) == 0) { size_t n = fread(secbuf, 1, 2352, trk[i].f); (void)n; }
        return;
    }
}
/* track: 0 = tocar a partir do setor 'lba' absoluto; >0 = faixa */
void cdda_play(int track, long lba, int autopause) {
    if (!loaded) return;
    play_pos = track > 0 && track <= ntrk ? trk[track].idx1 : lba; samp_in_sec = 0; secbuf_lba = -1; playing = 1; ended_flag = 0; stop_at_track_end = autopause;
    fprintf(stderr, "[cdda] tocando faixa %d (setor %ld)\n", cur_track_of(play_pos), play_pos);
}
void cdda_stop(void) { playing = 0; }
int cdda_playing(void) { return playing; }
long cdda_pos(void) { return play_pos; }
int cdda_track_now(void) { return cur_track_of(play_pos); }
int cdda_take_end(void) { int e = ended_flag; ended_flag = 0; return e; }

int cdda_mix(int* l, int* r) {                              /* uma amostra a 44100 Hz */
    if (!playing) return 0;
    if (play_pos >= leadout) { playing = 0; ended_flag = 1; return 0; }
    load_sector(play_pos);
    *l = secbuf[samp_in_sec * 2]; *r = secbuf[samp_in_sec * 2 + 1];
    if (++samp_in_sec >= 588) { samp_in_sec = 0; int t0 = cur_track_of(play_pos); play_pos++; if (stop_at_track_end && play_pos < leadout && cur_track_of(play_pos) != t0) { playing = 0; ended_flag = 1; } }
    return 1;
}
