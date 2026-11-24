// Bluetooth bring-up smoke test for Redmi 7 Remote OS (Qualcomm WCNSS/Pronto, HCI over SMD).
// 1) mount modem firmware, 2) boot the wcnss subsystem, 3) HCI Reset + Read BD_ADDR + Read Local Version on /dev/smd3.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <termios.h>

static void hex(const char *tag, const unsigned char *b, int n) { printf("%s", tag); for (int i = 0; i < n; i++) printf(" %02x", b[i]); printf("\n"); }

static int cmd(int fd, unsigned short opcode, const unsigned char *params, int plen, unsigned char *ev, int evcap) {
    unsigned char pkt[64] = {opcode & 0xFF, opcode >> 8, (unsigned char)plen};
    memcpy(pkt + 3, params, plen);
    if (write(fd, pkt, 3 + plen) < 0) { printf("write failed %d\n", errno); return -1; }
    struct pollfd p = {fd, POLLIN, 0};
    for (int tries = 0; tries < 10; tries++) {
        if (poll(&p, 1, 1000) <= 0) { printf("  (timeout waiting for event)\n"); return -1; }
        int n = read(fd, ev, evcap);
        if (n <= 0) return -1;
        hex("  event:", ev, n);
        if (ev[0] == 0x0E && n >= 6 && (ev[3] | ev[4] << 8) == opcode) return n;   // Command Complete for us
    }
    return -1;
}

int main(int argc, char **argv) {   // optional argv[1] = BD_ADDR to program, e.g. 02:11:22:33:44:55
    mkdir("/vendor", 0755); mkdir("/vendor/firmware_mnt", 0755);
    if (mount("/dev/block/by-name/modem", "/vendor/firmware_mnt", "vfat", MS_RDONLY, "shortname=lower") && errno != EBUSY)
        printf("modem mount failed: %s\n", strerror(errno));
    if (access("/vendor/firmware_mnt/image/wcnss.mdt", F_OK)) printf("WARNING: wcnss.mdt not found\n");
    else printf("firmware present\n");

    int sub = open("/dev/subsys_wcnss", O_RDONLY);      // holding this open keeps the subsystem powered
    printf("subsys_wcnss open: %s\n", sub >= 0 ? "ok" : strerror(errno));
    for (int i = 0; i < 20; i++) {
        char st[32] = ""; FILE *f;
        for (int k = 0; k < 8; k++) {
            char p[96]; snprintf(p, sizeof p, "/sys/bus/msm_subsys/devices/subsys%d/name", k);
            char nm[32] = ""; if ((f = fopen(p, "r"))) { fgets(nm, sizeof nm, f); fclose(f); }
            if (!strncmp(nm, "wcnss", 5)) { snprintf(p, sizeof p, "/sys/bus/msm_subsys/devices/subsys%d/state", k);
                if ((f = fopen(p, "r"))) { fgets(st, sizeof st, f); fclose(f); } }
        }
        printf("wcnss state: %s", st);
        if (!strncmp(st, "ONLINE", 6)) break;
        sleep(1);
    }
    int fd = -1;
    for (int i = 0; i < 10 && fd < 0; i++) { fd = open("/dev/smd3", O_RDWR | O_NOCTTY); if (fd < 0) sleep(1); }
    if (fd < 0) { printf("open smd3 failed: %s\n", strerror(errno)); return 1; }
    struct termios tio; if (!tcgetattr(fd, &tio)) { cfmakeraw(&tio); tcsetattr(fd, TCSANOW, &tio); }
    printf("smd3 open (raw)\n");
    unsigned char ev[260];
    printf("HCI_Reset:\n"); cmd(fd, 0x0C03, NULL, 0, ev, sizeof ev);
    printf("Read_BD_ADDR:\n");
    int n = cmd(fd, 0x1009, NULL, 0, ev, sizeof ev);
    if (n >= 12) printf("  BD_ADDR = %02X:%02X:%02X:%02X:%02X:%02X\n", ev[11], ev[10], ev[9], ev[8], ev[7], ev[6]);
    unsigned a[6];
    if (argc > 1 && sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
        unsigned char setaddr[9] = {0x01, 0x02, 0x06};           // NVM set, tag 2 = BD_ADDR, 6 bytes, LSB first
        for (int i = 0; i < 6; i++) setaddr[3 + i] = (unsigned char)a[5 - i];
        printf("Vendor 0xFC0B set BD_ADDR %s:\n", argv[1]); cmd(fd, 0xFC0B, setaddr, 9, ev, sizeof ev);
    }
    printf("HCI_Reset:\n"); cmd(fd, 0x0C03, NULL, 0, ev, sizeof ev);
    printf("Read_BD_ADDR again:\n");
    n = cmd(fd, 0x1009, NULL, 0, ev, sizeof ev);
    if (n >= 12) printf("  BD_ADDR = %02X:%02X:%02X:%02X:%02X:%02X\n", ev[11], ev[10], ev[9], ev[8], ev[7], ev[6]);
    printf("Read_Local_Version:\n"); cmd(fd, 0x1001, NULL, 0, ev, sizeof ev);
    close(fd);
    return 0;
}
