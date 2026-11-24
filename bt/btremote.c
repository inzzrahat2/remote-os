// btremote — Bluetooth HID remote daemon for Remote OS (Redmi 7, Qualcomm WCNSS/Pronto).
// BTstack host stack with a custom HCI transport over SMD (/dev/smd3 = cmd/event, /dev/smd2 = ACL).
// Control: write lines to FIFO /tmp/btctl   ("on", "off", "pair", "connect", "kb <hex> <0|1>", "cc <hex> <0|1>")
// Status:  /tmp/btstate                       ("off", "starting", "idle", "pairing", "connecting", "connected", "error")
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "btstack.h"
#include "btstack_run_loop_posix.h"
#include "btstack_tlv_posix.h"
#include "classic/btstack_link_key_db_tlv.h"

#define LOGF "/tmp/btremote.log"
#define CTL "/tmp/btctl"
#define STATE "/tmp/btstate"
#define DIR_ "/cache/remoteos"
#define TLV_FILE DIR_ "/bt.tlv"
#define TV_FILE DIR_ "/bt_tv"        // paired TV address (written on first successful connection)
#define ADDR_FILE DIR_ "/bt_addr"    // our BD_ADDR; generated once (random, locally administered) if missing
static bd_addr_t OUR_ADDR;           // MSB first

static void logf_(const char *fmt, ...) {
    FILE *f = fopen(LOGF, "a"); if (!f) return;
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    fprintf(f, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f);
}
static void set_state(const char *s) {
    FILE *f = fopen(STATE ".tmp", "w"); if (!f) return; fprintf(f, "%s\n", s); fclose(f); rename(STATE ".tmp", STATE);
    logf_("state %s", s);
}

/* ======================================================================= WCNSS + SMD transport */
static int fd_sub = -1, fd_cmd = -1, fd_acl = -1;
static btstack_data_source_t ds_cmd, ds_acl;
static void (*pkt_handler)(uint8_t type, uint8_t *packet, uint16_t size);

static int copy_file(const char *from, const char *to) {
    int a = open(from, O_RDONLY); if (a < 0) return -1;
    int b = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (b < 0) { close(a); return -1; }
    char buf[8192]; int n; while ((n = (int)read(a, buf, sizeof buf)) > 0) if (write(b, buf, n) != n) break;
    close(a); close(b); return 0;
}
/* Start WCNSS the Android way: RF calibration (NV) where the kernel finds it, then open /dev/wcnss_wlan
   (kernel detects the iris RF chip, powers its regulators, boots the core, downloads NV). */
static int fd_wlan = -1;
static int wcnss_up(void) {
    mkdir("/vendor", 0755); mkdir("/vendor/firmware_mnt", 0755);
    if (mount("/dev/block/by-name/modem", "/vendor/firmware_mnt", "vfat", MS_RDONLY, "shortname=lower") && errno != EBUSY)
        logf_("modem mount: %s", strerror(errno));
    if (access("/lib/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin", F_OK)) {
        // vendor/firmware/wlan/prima/*.bin are symlinks to /mnt/vendor/persist (per-device RF calibration). Read-only!
        mkdir("/mnt", 0755); mkdir("/mnt/per", 0755);
        if (mount("/dev/block/by-name/persist", "/mnt/per", "ext4", MS_RDONLY, NULL) && errno != EBUSY) logf_("persist mount: %s", strerror(errno));
        mkdir("/lib", 0755); mkdir("/lib/firmware", 0755); mkdir("/lib/firmware/wlan", 0755); mkdir("/lib/firmware/wlan/prima", 0755);
        const char *f[] = {"WCNSS_qcom_wlan_nv.bin", "WCNSS_wlan_dictionary.dat"};
        for (int i = 0; i < 2; i++) {
            char a[160], b[160];
            snprintf(a, sizeof a, "/mnt/per/%s", f[i]); snprintf(b, sizeof b, "/lib/firmware/wlan/prima/%s", f[i]);
            if (copy_file(a, b)) logf_("copy %s failed", f[i]);
        }
        umount("/mnt/per");
        logf_("NV calibration staged: %s", access("/lib/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin", F_OK) ? "MISSING" : "ok");
    }
    if (fd_wlan < 0) fd_wlan = open("/dev/wcnss_wlan", O_RDWR);
    if (fd_wlan < 0) { logf_("wcnss_wlan: %s", strerror(errno)); return -1; }
    for (int i = 0; i < 60; i++) {
        int fd = open("/dev/smd3", O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0) { close(fd); return 0; }
        usleep(250000);
    }
    return -1;
}
static int open_raw(const char *dev) {
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios tio;
    if (!tcgetattr(fd, &tio)) { cfmakeraw(&tio); tcsetattr(fd, TCSANOW, &tio); }
    tcflush(fd, TCIOFLUSH);
    return fd;
}
static int write_all(int fd, const uint8_t *b, int n) {
    while (n > 0) {
        int w = (int)write(fd, b, n);
        if (w < 0) { if (errno == EAGAIN) { struct pollfd p = {fd, POLLOUT, 0}; poll(&p, 1, 100); continue; } return -1; }
        b += w; n -= w;
    }
    return 0;
}
/* Program our BD_ADDR before BTstack takes over (vendor NVM cmd; survives the HCI reset BTstack sends). */
static void set_bdaddr_raw(int fd) {
    uint8_t c[12] = {0x0B, 0xFC, 9, 0x01, 0x02, 0x06};
    for (int i = 0; i < 6; i++) c[6 + i] = OUR_ADDR[5 - i];
    write_all(fd, c, sizeof c);
    uint8_t junk[256]; struct pollfd p = {fd, POLLIN, 0};
    while (poll(&p, 1, 300) > 0 && read(fd, junk, sizeof junk) > 0) { }       // drain vendor event + completion
}

/* SMD is a byte stream: re-frame HCI packets by their length headers. */
typedef struct { uint8_t buf[HCI_INCOMING_PRE_BUFFER_SIZE + 2048]; int len; } rxq_t;
static rxq_t rx_evt, rx_acl;
static void drain(int fd, rxq_t *q, int is_acl) {
    uint8_t *data = q->buf + HCI_INCOMING_PRE_BUFFER_SIZE;
    int n = (int)read(fd, data + q->len, sizeof q->buf - HCI_INCOMING_PRE_BUFFER_SIZE - q->len);
    if (n <= 0) return;
    q->len += n;
    for (;;) {
        int need;
        if (is_acl) { if (q->len < 4) break; need = 4 + (data[2] | data[3] << 8); }
        else { if (q->len < 2) break; need = 2 + data[1]; }
        if (q->len < need) break;
        static uint8_t pkt[HCI_INCOMING_PRE_BUFFER_SIZE + 1100];
        memcpy(pkt + HCI_INCOMING_PRE_BUFFER_SIZE, data, need);
        memmove(data, data + need, q->len - need); q->len -= need;
        if (pkt_handler) pkt_handler(is_acl ? HCI_ACL_DATA_PACKET : HCI_EVENT_PACKET, pkt + HCI_INCOMING_PRE_BUFFER_SIZE, need);
    }
}
static void on_cmd(btstack_data_source_t *ds, btstack_data_source_callback_type_t t) { (void)ds; (void)t; drain(fd_cmd, &rx_evt, 0); }
static void on_acl(btstack_data_source_t *ds, btstack_data_source_callback_type_t t) { (void)ds; (void)t; drain(fd_acl, &rx_acl, 1); }

static void smd_init(const void *cfg) { (void)cfg; }
static int smd_open(void) {
    if (wcnss_up()) { set_state("error"); return -1; }
    // right after WCNSS boots the SMD channels exist but return EAGAIN until the remote side opens them
    for (int i = 0; i < 40 && (fd_cmd < 0 || fd_acl < 0); i++) {
        if (fd_cmd < 0) fd_cmd = open_raw("/dev/smd3");
        if (fd_acl < 0) fd_acl = open_raw("/dev/smd2");
        if (fd_cmd < 0 || fd_acl < 0) usleep(250000);
    }
    if (fd_cmd < 0 || fd_acl < 0) { logf_("smd open failed %s", strerror(errno)); return -1; }
    set_bdaddr_raw(fd_cmd);
    rx_evt.len = rx_acl.len = 0;
    btstack_run_loop_set_data_source_fd(&ds_cmd, fd_cmd); btstack_run_loop_set_data_source_handler(&ds_cmd, on_cmd);
    btstack_run_loop_enable_data_source_callbacks(&ds_cmd, DATA_SOURCE_CALLBACK_READ); btstack_run_loop_add_data_source(&ds_cmd);
    btstack_run_loop_set_data_source_fd(&ds_acl, fd_acl); btstack_run_loop_set_data_source_handler(&ds_acl, on_acl);
    btstack_run_loop_enable_data_source_callbacks(&ds_acl, DATA_SOURCE_CALLBACK_READ); btstack_run_loop_add_data_source(&ds_acl);
    logf_("transport open");
    return 0;
}
static int smd_close(void) {
    btstack_run_loop_remove_data_source(&ds_cmd); btstack_run_loop_remove_data_source(&ds_acl);
    if (fd_cmd >= 0) close(fd_cmd); if (fd_acl >= 0) close(fd_acl);
    fd_cmd = fd_acl = -1;
    // keep /dev/wcnss_wlan open: the kernel never re-triggers config on a second open
    logf_("transport closed");
    return 0;
}
static void smd_register(void (*h)(uint8_t, uint8_t *, uint16_t)) { pkt_handler = h; }
static int smd_send(uint8_t type, uint8_t *p, int n) {
    if (type == HCI_COMMAND_DATA_PACKET) return write_all(fd_cmd, p, n);
    if (type == HCI_ACL_DATA_PACKET) return write_all(fd_acl, p, n);
    return -1;
}
static const hci_transport_t transport_smd = {"SMD", smd_init, smd_open, smd_close, smd_register, NULL, smd_send, NULL, NULL, NULL};

/* ======================================================================= HID */
static const uint8_t hid_descriptor[] = {
    // Keyboard, report ID 1: [modifiers][reserved][6 keys]
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    // Consumer control, report ID 2: one 16-bit usage
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};
static uint8_t hid_service_buffer[300], devid_buffer[100];
static uint16_t hid_cid;
static bd_addr_t tv_addr;
static int tv_known;                 // set once a TV address is known (bt_tv file or first connection)
static int want_on, hci_up, connected, standby;
static btstack_packet_callback_registration_t hci_cb;
static btstack_tlv_posix_t tlv_ctx;
static btstack_timer_source_t reconnect_timer, pair_timer;

/* outgoing report queue (keys arrive faster than CAN_SEND_NOW) */
static uint8_t q[32][11]; static int q_len[32], q_head, q_tail;
static void queue_report(const uint8_t *r, int n) {
    int nx = (q_tail + 1) % 32; if (nx == q_head) return;
    memcpy(q[q_tail], r, n); q_len[q_tail] = n; q_tail = nx;
    if (connected) hid_device_request_can_send_now_event(hid_cid);
}

static void try_connect(btstack_timer_source_t *ts) {
    (void)ts;
    if (!want_on || !hci_up || connected || !tv_known) return;
    logf_("connecting to %s", bd_addr_to_str(tv_addr));
    set_state("connecting");
    hid_device_connect(tv_addr, &hid_cid);
}
static void schedule_reconnect(int ms) {
    btstack_run_loop_remove_timer(&reconnect_timer);
    btstack_run_loop_set_timer_handler(&reconnect_timer, try_connect);
    btstack_run_loop_set_timer(&reconnect_timer, ms);
    btstack_run_loop_add_timer(&reconnect_timer);
}
static void pair_end(btstack_timer_source_t *ts) { (void)ts; gap_discoverable_control(0); if (!connected && hci_up) set_state("idle"); }

static void packet_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            hci_up = 1; bd_addr_t a; gap_local_bd_addr(a);
            logf_("HCI working, local %s", bd_addr_to_str(a));
            set_state("idle");
            link_key_t k; link_key_type_t kt;
            if (tv_known && gap_get_link_key_for_bd_addr(tv_addr, k, &kt)) schedule_reconnect(200);   // bonded: reconnect
        } else if (btstack_event_state_get_state(packet) == HCI_STATE_OFF) { hci_up = 0; connected = 0; set_state("off"); }
        break;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
        bd_addr_t a; hci_event_user_confirmation_request_get_bd_addr(packet, a);
        logf_("SSP confirm %06u from %s -> accept", (unsigned)hci_event_user_confirmation_request_get_numeric_value(packet), bd_addr_to_str(a));
        gap_ssp_confirmation_response(a); break; }
    case GAP_EVENT_INQUIRY_RESULT: {
        bd_addr_t a; gap_event_inquiry_result_get_bd_addr(packet, a);
        logf_("found %s cod %06x rssi %d", bd_addr_to_str(a), (unsigned)gap_event_inquiry_result_get_class_of_device(packet),
              gap_event_inquiry_result_get_rssi_available(packet) ? (int8_t)gap_event_inquiry_result_get_rssi(packet) : 0);
        break; }
    case GAP_EVENT_INQUIRY_COMPLETE: logf_("inquiry complete"); break;
    case HCI_EVENT_CONNECTION_COMPLETE: logf_("acl connection complete status 0x%02x", hci_event_connection_complete_get_status(packet)); break;
    case HCI_EVENT_CONNECTION_REQUEST: { bd_addr_t a; hci_event_connection_request_get_bd_addr(packet, a); logf_("incoming connection from %s", bd_addr_to_str(a)); break; }
    case HCI_EVENT_AUTHENTICATION_COMPLETE: logf_("auth complete status 0x%02x", hci_event_authentication_complete_get_status(packet)); break;
    case L2CAP_EVENT_INCOMING_CONNECTION:
        logf_("incoming L2CAP psm 0x%04x", l2cap_event_incoming_connection_get_psm(packet));
        break;
    case HCI_EVENT_LINK_KEY_NOTIFICATION:      // BTstack has just stored the key: make it durable
        if (tlv_ctx.file) { fflush(tlv_ctx.file); fsync(fileno(tlv_ctx.file)); }
        logf_("link key stored + synced");
        break;
    case HCI_EVENT_PIN_CODE_REQUEST: { bd_addr_t a; hci_event_pin_code_request_get_bd_addr(packet, a); gap_pin_code_response(a, "0000"); break; }
    case HCI_EVENT_HID_META:
        switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_CONNECTION_OPENED:
            if (hid_subevent_connection_opened_get_status(packet) != ERROR_CODE_SUCCESS) {
                uint8_t st = hid_subevent_connection_opened_get_status(packet);
                logf_("hid open failed 0x%02x", st);
                connected = 0; set_state("idle");
                // 0x67 = TV refused because it is connecting to us itself: stay passive and let it come in
                if (want_on) schedule_reconnect(st == L2CAP_CONNECTION_RESPONSE_RESULT_REFUSED_RESOURCES ? 30000 : 8000);
                break;
            }
            hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            hid_subevent_connection_opened_get_bd_addr(packet, tv_addr);
            connected = 1; tv_known = 1; set_state("connected");
            { FILE *f = fopen(TV_FILE, "w"); if (f) { fprintf(f, "%s\n", bd_addr_to_str(tv_addr)); fclose(f); } }
            gap_discoverable_control(0);
            if (q_head != q_tail) hid_device_request_can_send_now_event(hid_cid);
            break;
        case HID_SUBEVENT_CONNECTION_CLOSED:
            connected = 0; hid_cid = 0; logf_("hid closed"); set_state(hci_up ? "idle" : "off");
            if (want_on) schedule_reconnect(5000);
            else if (standby) hci_power_control(HCI_POWER_OFF);       // link lost while screen off: radio off
            break;
        case HID_SUBEVENT_CAN_SEND_NOW:
            if (q_head != q_tail) {
                hid_device_send_interrupt_message(hid_cid, q[q_head], q_len[q_head]);
                q_head = (q_head + 1) % 32;
                if (q_head != q_tail) hid_device_request_can_send_now_event(hid_cid);
            }
            break;
        }
        break;
    }
}

/* ======================================================================= control FIFO */
static btstack_data_source_t ds_ctl;
static void handle_line(char *l) {
    unsigned u = 0; int d = 0;
    if (!strcmp(l, "on")) {
        want_on = 1; standby = 0;
        if (connected) set_state("connected");                       // link kept alive in standby: instant
        else if (!hci_up) { set_state("starting"); hci_power_control(HCI_POWER_ON); }
        else schedule_reconnect(0);
    }
    else if (!strcmp(l, "standby")) {                                 // screen off: keep the TV link (sniff), stop retrying
        want_on = 0; standby = 1; btstack_run_loop_remove_timer(&reconnect_timer);
        if (!connected) hci_power_control(HCI_POWER_OFF);             // nothing to keep: fully off
    }
    else if (!strcmp(l, "off")) { want_on = 0; btstack_run_loop_remove_timer(&reconnect_timer); if (connected) hid_device_disconnect(hid_cid); hci_power_control(HCI_POWER_OFF); }
    else if (!strcmp(l, "pair")) {
        want_on = 1; if (!hci_up) hci_power_control(HCI_POWER_ON);
        gap_discoverable_control(1); gap_connectable_control(1); set_state("pairing");
        btstack_run_loop_remove_timer(&pair_timer); btstack_run_loop_set_timer_handler(&pair_timer, pair_end);
        btstack_run_loop_set_timer(&pair_timer, 120000); btstack_run_loop_add_timer(&pair_timer);
        // known TV: page it (pairing dialog appears on the TV). Unknown TV: stay discoverable and pair from the
        // TV side (Settings > Remotes & Accessories > Add accessory > "Nova Remote").
        if (tv_known) schedule_reconnect(1500);
    }
    else if (!strcmp(l, "connect")) schedule_reconnect(10);
    else if (!strcmp(l, "scan")) { logf_("inquiry start"); gap_inquiry_start(8); }
    else if (sscanf(l, "kb %x %d", &u, &d) == 2) { uint8_t r[9] = {0xA1, 0x01, 0, 0, (uint8_t)(d ? u : 0), 0, 0, 0, 0}; queue_report(r, 9); }
    else if (sscanf(l, "cc %x %d", &u, &d) == 2) { uint16_t v = d ? u : 0; uint8_t r[4] = {0xA1, 0x02, (uint8_t)v, (uint8_t)(v >> 8)}; queue_report(r, 4); }
}
static void on_ctl(btstack_data_source_t *ds, btstack_data_source_callback_type_t t) {
    (void)t;
    static char buf[256]; static int len;
    int n = (int)read(ds->source.fd, buf + len, sizeof buf - 1 - len);
    if (n <= 0) return;
    len += n; buf[len] = 0;
    char *nl;
    while ((nl = strchr(buf, '\n'))) { *nl = 0; if (buf[0]) handle_line(buf); memmove(buf, nl + 1, strlen(nl + 1) + 1); len = (int)strlen(buf); }
}

static void load_addresses(void) {
    char s[40]; FILE *f;
    if ((f = fopen(ADDR_FILE, "r"))) { if (!fgets(s, sizeof s, f) || !sscanf_bd_addr(s, OUR_ADDR)) memset(OUR_ADDR, 0, 6); fclose(f); }
    if (!(OUR_ADDR[0] | OUR_ADDR[1] | OUR_ADDR[2] | OUR_ADDR[3] | OUR_ADDR[4] | OUR_ADDR[5])) {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0 || read(fd, OUR_ADDR, 6) != 6) { OUR_ADDR[5] = (uint8_t)time(NULL); }
        if (fd >= 0) close(fd);
        OUR_ADDR[0] = (OUR_ADDR[0] & 0xFC) | 0x02;           // locally administered, unicast
        if ((f = fopen(ADDR_FILE, "w"))) { fprintf(f, "%s\n", bd_addr_to_str(OUR_ADDR)); fflush(f); fsync(fileno(f)); fclose(f); }
        logf_("generated BD_ADDR %s", bd_addr_to_str(OUR_ADDR));
    }
    if ((f = fopen(TV_FILE, "r"))) { if (fgets(s, sizeof s, f) && sscanf_bd_addr(s, tv_addr)) tv_known = 1; fclose(f); }
    logf_("BD_ADDR %s, TV %s", bd_addr_to_str(OUR_ADDR), tv_known ? bd_addr_to_str(tv_addr) : "(not paired yet)");
}

int main(int argc, char **argv) {
    logf_("btremote start");
    // /cache is mounted by init (on fs) so keys never land in the ramdisk; mount here too as a fallback
    mkdir("/cache", 0755);
    if (mount("/dev/block/by-name/cache", "/cache", "ext4", MS_NOATIME, NULL) && errno != EBUSY) logf_("cache mount: %s", strerror(errno));
    mkdir(DIR_, 0755);
    load_addresses();

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_posix_get_instance());
    hci_init(&transport_smd, NULL);

    const btstack_tlv_t *tlv = btstack_tlv_posix_init_instance(&tlv_ctx, TLV_FILE);
    btstack_tlv_set_instance(tlv, &tlv_ctx);
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv, &tlv_ctx));

    gap_set_local_name("Nova Remote");
    gap_set_class_of_device(0x2540);                         // Peripheral / Keyboard
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_set_bondable_mode(1);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);
    gap_connectable_control(1);

    l2cap_init();
    sdp_init();
    hid_sdp_record_t p = {0x2540, 33, 0, 1, 1, true, false, 1600, 3200, 3200, hid_descriptor, sizeof hid_descriptor, "Nova Remote"};
    hid_create_sdp_record(hid_service_buffer, sdp_create_service_record_handle(), &p);
    sdp_register_service(hid_service_buffer);
    device_id_create_sdp_record(devid_buffer, sdp_create_service_record_handle(), DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH, BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH, 1, 1);
    sdp_register_service(devid_buffer);
    hid_device_init(false, sizeof hid_descriptor, hid_descriptor);

    hci_cb.callback = &packet_handler; hci_add_event_handler(&hci_cb);
    hid_device_register_packet_handler(&packet_handler);

    unlink(CTL); mkfifo(CTL, 0666);
    int cfd = open(CTL, O_RDWR | O_NONBLOCK);
    btstack_run_loop_set_data_source_fd(&ds_ctl, cfd); btstack_run_loop_set_data_source_handler(&ds_ctl, on_ctl);
    btstack_run_loop_enable_data_source_callbacks(&ds_ctl, DATA_SOURCE_CALLBACK_READ); btstack_run_loop_add_data_source(&ds_ctl);

    set_state("off");
    if (argc > 1 && !strcmp(argv[1], "on")) handle_line("on");
    btstack_run_loop_execute();
    return 0;
}
