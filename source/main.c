#include <nds.h>
#include <stdio.h>

int main(void) {
    consoleDemoInit();
    iprintf("ndsrecomp scaffold\nSTART: exit\n");

    while (pmMainLoop()) {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_START) {
            break;
        }
    }

    return 0;
}

