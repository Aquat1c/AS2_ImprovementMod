#include "ui/palette_ramps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void Check(bool ok) {
    if (!ok) {
        puts("palette editor check failed");
        exit(1);
    }
}
int main() {
    unsigned char bank[1024]{};
    for (int i = 1; i <= 4; ++i) {
        bank[i * 4 + 2] = (unsigned char)(i * 50);
        bank[i * 4 + 3] = 255;
    }
    bank[20] = 180;
    bank[23] = 255;
    PaletteRamps::Group groups[256]{};
    int n = PaletteRamps::Build(bank, groups);
    Check(n == 2 && groups[0].first == 1 && groups[0].last == 4 && groups[1].first == 5);
    unsigned char zeroReserved[1024];
    memcpy(zeroReserved, bank, 1024);
    for (int i = 0; i < 256; ++i)
        zeroReserved[i * 4 + 3] = 0;
    Check(PaletteRamps::Build(zeroReserved, groups) == 2);
    Check(groups[1].last == 5);
    unsigned char edited[1024];
    memcpy(edited, bank, 1024);
    PaletteRamps::Recolor(bank, edited, groups[0], 0, 200, 0);
    Check(edited[0] == 0 && edited[3] == 0 && edited[20] == 180);
    for (int i = 1; i <= 4; ++i) {
        Check(edited[i * 4 + 3] == 255 && edited[i * 4 + 2] == 0);
        if (i > 1)
            Check(edited[i * 4 + 1] >= edited[(i - 1) * 4 + 1]);
    }
    unsigned char identity[1024];
    memcpy(identity, bank, 1024);
    int key = groups[0].key;
    PaletteRamps::Recolor(bank, identity, groups[0], bank[key * 4 + 2], bank[key * 4 + 1], bank[key * 4]);
    Check(memcmp(identity, bank, 1024) == 0);
    bank[2 * 4 + 3] = 128;
    n = PaletteRamps::Build(bank, groups);
    Check(n == 4);
    memset(bank, 0, 1024);
    Check(PaletteRamps::Build(bank, groups) == 0);
    for (int i = 1; i < 256; ++i) {
        bank[i * 4 + (i % 3)] = 255;
        bank[i * 4 + 3] = 255;
    }
    Check(PaletteRamps::Build(bank, groups) == 255);
    puts("Palette grouping, recolour, identity, alpha and bounds checks passed");
}
