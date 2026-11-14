/* The same disc as a .chd and as a cue sheet with its tracks (chdman extractcd's, or what
 * Setup up to 1.1 unpacked a .chd to) has to read the same through disc.h: the same tracks
 * at the same places, and every sector the same.
 *
 * usage: chd_test <disc.chd> <disc.cue>   (needs the disc: not run by ctest) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pce/disc.h"
#include "pce/discid.h"

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: chd_test <disc.chd> <disc.cue>\n"); return 2; }
    static Disc a, b;
    char err[256];
    if (!disc_open(&a, argv[1], err, sizeof err)) { printf("%s: %s\n", argv[1], err); return 1; }
    if (!disc_open(&b, argv[2], err, sizeof err)) { printf("%s: %s\n", argv[2], err); return 1; }
    int bad = 0;
    if (a.n_tracks != b.n_tracks) { printf("tracks: %d, %d\n", a.n_tracks, b.n_tracks); return 1; }
    long sectors = 0;
    double chd_time = 0;
    uint8_t sa[2352], sb[2352];
    for (int i = 0; i < a.n_tracks; i++) {
        const DiscTrack *x = &a.tracks[i], *y = &b.tracks[i];
        if (x->number != y->number || x->data != y->data || x->lba != y->lba || x->sectors != y->sectors) {
            printf("track %d: %s at %d, %d sectors; %s at %d, %d sectors\n", y->number, x->data ? "data" : "audio",
                   x->lba, x->sectors, y->data ? "data" : "audio", y->lba, y->sectors);
            bad++;
            continue;
        }
        int size = x->data ? 2048 : 2352, differ = 0;
        for (int s = x->lba; s < x->lba + x->sectors; s++) {
            clock_t t0 = clock();
            bool ra = x->data ? disc_read_data(&a, s, 1, sa) : disc_read_audio(&a, s, 1, sa);
            chd_time += (double)(clock() - t0) / CLOCKS_PER_SEC;
            bool rb = x->data ? disc_read_data(&b, s, 1, sb) : disc_read_audio(&b, s, 1, sb);
            if (!ra || !rb) {
                printf("track %d, sector %d: %s\n", x->number, s, !ra ? "the .chd cannot be read" : "the cue cannot be read");
                return 1;
            }
            if (memcmp(sa, sb, (size_t)size)) differ++;
            sectors++;
        }
        if (differ) { printf("track %d: %d of %d sectors differ\n", x->number, differ, x->sectors); bad++; }
    }
    /* many sectors at once, as the game's loads read them */
    const DiscTrack *d = NULL;
    for (int i = 0; i < a.n_tracks && !d; i++) if (a.tracks[i].data) d = &a.tracks[i];
    if (d && d->sectors >= 352) {
        uint8_t *ma = malloc(352 * 2048), *mb = malloc(352 * 2048);
        if (!disc_read_data(&a, d->lba + 16, 352, ma) || !disc_read_data(&b, d->lba + 16, 352, mb) ||
            memcmp(ma, mb, 352 * 2048)) {
            printf("352 sectors at once: they differ\n");
            bad++;
        }
        free(ma);
        free(mb);
    }
    uint64_t fa = 0, fb = 0;
    if (discid_identify(argv[1], &fa) != discid_identify(argv[2], &fb) || fa != fb ||
        discid_language(argv[1]) != discid_language(argv[2])) {
        printf("identified differently: %016llX, %016llX\n", (unsigned long long)fa, (unsigned long long)fb);
        bad++;
    }
    disc_close(&a);
    disc_close(&b);
    printf("%d tracks, %ld sectors: %s (the .chd read in %.1f s, %.0f sectors a second)\n", a.n_tracks, sectors,
           bad ? "DIFFERENT" : "the same", chd_time, chd_time > 0 ? sectors / chd_time : 0.0);
    return bad ? 1 : 0;
}
