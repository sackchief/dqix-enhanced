/* ARM7 I/O registers and the devices behind them: SPI (power management,
 * firmware flash, touch screen), RTC, sound registers, wireless (stubbed). */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nds.h"
#include "session.h"
#include "frontend.h"

uint8_t g_io7[0x1000];
uint32_t dispstat_read(Core* k);

/* ================================================================ SPI */
static uint8_t g_fw[0x20000];             /* firmware flash image (128 KB) */
static int g_fw_nosave;                    /* a console of a session: its firmware is someone else's */
static uint8_t g_fw_status;               /* flash status register (bit 1: write enabled) */
static uint8_t g_pm_regs[8] = { 0x0D, 0x00, 0x01, 0x00, 0x03 };  /* power management */
static int g_spi_dev = -1, g_spi_pos;
static uint8_t g_spi_cmd, g_spi_out;
static uint32_t g_fw_addr;
static uint8_t g_tsc_cmd;
static uint16_t g_tsc_val;

static uint16_t crc16_buf(uint16_t crc, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) { int c = crc & 1; crc >>= 1; if (c) crc ^= 0xA001; }  /* CRC-16, same as the BIOS */
    }
    return crc;
}

/* The firmware flash image, laid out like the one melonDS generates when no
 * dump is supplied (so the game sees the same bytes as on the reference):
 * a 128 KB flash with the Wi-Fi calibration header (+CRC), three Wi-Fi
 * access-point slots (+CRC each) and two copies of the user settings (+CRC).
 * The user settings hold the same values main.c places at 0x027FFC80. */
#define FW_SIZE 0x20000u
#define FW_USER 0x1FE00u                     /* user settings: last 512 bytes */
#define FW_AP   (FW_USER - 0x400)            /* three 256-byte access points */
static const uint8_t fw_bbinit[0x69] = {
    0x03,0x17,0x40,0x00,0x1B,0x6C,0x48,0x80,0x38,0x00,0x35,0x07,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xB0,0x00,0x00,0x00,0x00,0x00,0x00,0xC7,0xBB,0x01,0x24,0x7F,
    0x5A,0x01,0x3F,0x01,0x3F,0x36,0x1D,0x00,0x78,0x35,0x55,0x12,0x34,0x1C,0x00,0x01,
    0x0E,0x38,0x03,0x70,0xC5,0x2A,0x0A,0x08,0x04,0x01,0x00,0x00,0x00,0xFF,0xFF,0xFE,
    0xFE,0xFE,0xFE,0xFC,0xFC,0xFA,0xFA,0xFA,0xFA,0xFA,0xF8,0xF8,0xF6,0x00,0x12,0x14,
    0x12,0x41,0x23,0x03,0x04,0x70,0x35,0x0E,0x2C,0x2C,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x0E,0x00,0x00,0x12,0x28,0x1C };
static const uint8_t fw_rfinit[0x29] = {
    0x31,0x4C,0x4F,0x21,0x00,0x10,0xB0,0x08,0xFA,0x15,0x26,0xE6,0xC1,0x01,0x0E,0x50,
    0x05,0x00,0x6D,0x12,0x00,0x00,0x01,0xFF,0x0E,0x00,0x02,0x00,0x00,0x00,0x00,0x06,
    0x06,0x00,0x00,0x00,0x18,0x00,0x02,0x00,0x00 };
static const uint8_t fw_chandata[0x3C] = {
    0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0E,0x0E,0x0E,0x0E,0x0E,0x0E,0x0E,0x16,
    0x26,0x1C,0x1C,0x1C,0x1D,0x1D,0x1D,0x1E,0x1E,0x1E,0x1E,0x1F,0x1E,0x1F,0x18,
    0x01,0x4B,0x4B,0x4B,0x4B,0x4C,0x4C,0x4C,0x4C,0x4C,0x4C,0x4C,0x4D,0x4D,0x4D,
    0x02,0x6C,0x71,0x76,0x5B,0x40,0x45,0x4A,0x2F,0x34,0x39,0x3E,0x03,0x08,0x14 };

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

void firmware_init(void) {
    if (getenv("DQIX_LANGUAGE")) g_fw_language = atoi(getenv("DQIX_LANGUAGE"));   /* tests: 1-5 */
    if (g_fw_language < 1 || g_fw_language > 5) g_fw_language = 1;
    memset(g_fw, 0xFF, sizeof g_fw);
    /* header + Wi-Fi calibration (0x000-0x1FF) */
    uint8_t* h = g_fw;
    memset(h, 0, 0x200);
    memcpy(&h[0x08], "MELN", 4);             /* identifier of a generated image */
    h[0x1D] = 0x20;                          /* console type: DS Lite */
    put16(&h[0x20], (uint16_t)(FW_USER >> 3));
    put16(&h[0x2C], 0x138);                  /* Wi-Fi config length */
    h[0x2F] = 6;                             /* Wi-Fi version W006 */
    memset(&h[0x30], 0xFF, 5);               /* unused (0xFF x5, 0x00) */
    static const uint8_t mac[6] = { 0x00, 0x09, 0xBF, 0x11, 0x22, 0x33 };
    memcpy(&h[0x36], mac, 6);
    put16(&h[0x3C], 0x3FFE);                 /* enabled channels 1-13 */
    h[0x3E] = 0xFF; h[0x3F] = 0xFF;
    h[0x40] = 3; h[0x41] = 0x94; h[0x42] = 0x29; h[0x43] = 0x02;  /* RF chip type 3 */
    static const uint16_t iv[16] = { 0x0002, 0x0017, 0x0026, 0x1818, 0x0048, 0x4840, 0x0058, 0x0042,
                                     0x0146, 0x8064, 0xE6E6, 0x2443, 0x000E, 0x0001, 0x0001, 0x0402 };
    for (int i = 0; i < 16; i++) put16(&h[0x44 + 2 * i], iv[i]);
    memcpy(&h[0x64], fw_bbinit, sizeof fw_bbinit);
    memcpy(&h[0xCE], fw_rfinit, sizeof fw_rfinit);
    h[0xF7] = 0x02;                          /* BB indices per channel */
    memcpy(&h[0xF8], fw_chandata, sizeof fw_chandata);
    memset(&h[0x134], 0xFF, 46);             /* type-3 config: unused */
    put16(&h[0x2A], crc16_buf(0x0000, &h[0x2C], 0x138));
    h[0x2FF] = 0x80;

    /* Wi-Fi access points: all three empty, like a console that never went
     * online (DQIX_FW_REFAP=1 configures slot 1 the way melonDS does, for
     * comparisons with the reference emulator) */
    int refap = getenv("DQIX_FW_REFAP") != NULL;
    for (int k = 0; k < 3; k++) {
        uint8_t* a = &g_fw[FW_AP + 0x100 * k];
        memset(a, 0, 0x100);
        if (k == 0 && refap) { memcpy(&a[0x40], "melonAP", 7); a[0xE7] = 0x00; }
        else a[0xE7] = 0xFF;                 /* status: not configured */
        a[0xEF] = 0x01;                      /* connection configured */
        put16(&a[0xFE], crc16_buf(0x0000, a, 0xFE));
    }

    /* user settings, two copies (DQIX_FW_REF=1: melonDS's defaults instead,
     * for exact comparisons with the reference emulator) */
    int ref = getenv("DQIX_FW_REF") != NULL;
    for (int copy = 0; copy < 2; copy++) {
        uint8_t* u = &g_fw[FW_USER + 0x100 * copy];
        memset(u, 0, 0x100);
        if (ref) {
            u[0x00] = 5; u[0x03] = 1; u[0x04] = 1;
            const char* nick = "melonDS";
            for (int i = 0; nick[i]; i++) u[0x06 + 2 * i] = (uint8_t)nick[i];
            u[0x1A] = 7;
            put16(&u[0x5E], 0xFF0); put16(&u[0x60], 0xBF0); u[0x62] = 0xFF; u[0x63] = 0xBF;
            u[0x64] = 0x31;                  /* English, backlight max */
            put16(&u[0x72], crc16_buf(0xFFFF, u, 0x70));
            continue;
        }
        u[0x00] = 5;                         /* version */
        u[0x02] = 4;                         /* favourite colour */
        u[0x03] = 1; u[0x04] = 1;            /* birthday */
        const char* nick = "Player";
        for (int i = 0; nick[i]; i++) { u[0x06 + 2 * i] = (uint8_t)nick[i]; }
        u[0x1A] = (uint8_t)strlen(nick);
        /* touch calibration: ADC (0x200,0x200)->(0x20,0x20), (0xE00,0xE00)->(0xE0,0xA0) */
        /* touch calibration: x ADC = 16 * pixel, y ADC = 0x200 + 16 * pixel, so
         * every pixel (0-255, 0-191) is inside the 12-bit ADC range like a real
         * panel, and the steps are a power of two so the game's conversion back
         * (fixed-point, rounding down) is exact */
        put16(&u[0x58], 0x200); put16(&u[0x5A], 0x400); u[0x5C] = 0x20; u[0x5D] = 0x20;
        put16(&u[0x5E], 0xE00); put16(&u[0x60], 0xC00); u[0x62] = 0xE0; u[0x63] = 0xA0;
        uint16_t flags = (uint16_t)(g_fw_language | 0x0400);   /* language, backlight */
        memcpy(&u[0x64], &flags, 2);
        uint16_t count = (uint16_t)copy;
        memcpy(&u[0x70], &count, 2);
        uint16_t crc = crc16_buf(0xFFFF, u, 0x70);
        memcpy(&u[0x72], &crc, 2);
    }
    wifi_reset(g_fw);
    /* the frontend's wireless choices (set before boot); DQIX_ONLINE=dns for tests */
    if (!g_wireless.online && getenv("DQIX_ONLINE")) {               /* tests: an address, or "builtin" */
        g_wireless.online = 1;
        g_wireless.builtin = !strcmp(getenv("DQIX_ONLINE"), "builtin");
        snprintf(g_wireless.dns, sizeof g_wireless.dns, "%s", g_wireless.builtin ? "10.64.0.2" : getenv("DQIX_ONLINE"));
    }
    wfc_enable(g_wireless.online && g_wireless.builtin, session_requested() ? "session/wfc" : getenv("DQIX_WFC_DIR"));   /* sessions: the host's files */
    const char* m = getenv("DQIX_MAC");                          /* replaying a report: its mac line */
    unsigned b[6];
    if (m && sscanf(m, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        for (int i = 0; i < 6; i++) g_wireless.mac[i] = (uint8_t)b[i];
        g_wireless.mac_set = 1;
    }
    firmware_load_wireless();
    if (session_requested()) g_fw_nosave = 1;                  /* a session's settings are for this session only */
    if (g_wireless.mac_set) firmware_set_mac(g_wireless.mac);
    firmware_set_ap(g_wireless.online, g_wireless.dns);
    net_enable(g_wireless.online);
    const char* img = getenv("DQIX_FW_FILE");                   /* a session: another player's console */
    if (img) firmware_load_image(img);
}

/* another MAC address in a copy of the image (tests: several consoles from one PC) */
void firmware_image_set_mac(uint8_t* img, const uint8_t mac[6]) {
    memcpy(&img[0x36], mac, 6);
    put16(&img[0x2A], crc16_buf(0x0000, &img[0x2C], 0x138));
}

/* The whole flash image (sessions: the other PCs run this console with it). */
const uint8_t* firmware_image(uint32_t* size) { *size = FW_SIZE; return g_fw; }
int firmware_load_image(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { LOG_WARN("firmware: cannot read %s\n", path); return 0; }
    size_t n = fread(g_fw, 1, FW_SIZE, f);
    fclose(f);
    g_fw_nosave = 1;
    wifi_reset(g_fw);
    return n == FW_SIZE;
}
WirelessConfig g_wireless;
int g_fw_language = 1;

/* What the firmware copies to RAM at boot: the user settings (newest valid
 * copy; both are the same here) and the header values the SDK reads. */
const uint8_t* firmware_user_settings(void) { return &g_fw[FW_USER + 0x100]; }
uint32_t firmware_user_offset(void) { return FW_USER; }

/* ---------------------------------------------------------------- wireless settings
 * The DS keeps three Wi-Fi connections in its firmware (256 bytes each, CRC
 * at 0xFE). Slot 1 can be pointed at the virtual access point (wifi.c) with
 * a fixed DNS server - how players reach community Nintendo Wi-Fi
 * Connection servers on real hardware. What the game writes there (its
 * Nintendo WFC ID) is kept in wireless.bin next to the game. */
static const char WFC_FILE[] = "wireless.bin";
static int g_fw_ap_dirty;

static void ap_crc(uint8_t* a) { put16(&a[0xFE], crc16_buf(0x0000, a, 0xFE)); }

void firmware_load_wireless(void) {
    FILE* f = fopen(WFC_FILE, "rb");
    if (!f) return;
    uint8_t buf[0x300];
    if (fread(buf, 1, sizeof buf, f) == sizeof buf) memcpy(&g_fw[FW_AP], buf, sizeof buf);
    fclose(f);
}
static void firmware_save_wireless(void) {
    if (!g_fw_ap_dirty || g_fw_nosave) return;
    g_fw_ap_dirty = 0;
    FILE* f = fopen(WFC_FILE, "wb");
    if (!f) return;
    fwrite(&g_fw[FW_AP], 1, 0x300, f);
    fclose(f);
}

void firmware_write_wireless(FILE* f) { fwrite(&g_fw[FW_AP], 1, 0x300, f); }

/* on: slot 1 = the virtual access point "DQIX-PC" (open, address by DHCP, DNS = dns) */
void firmware_set_ap(int on, const char* dns) {
    uint8_t* a = &g_fw[FW_AP];
    int ours = !memcmp(&a[0x40], "DQIX-PC", 8);
    if (!on && !ours) return;
    if (on) {
        if (!ours) { memset(a, 0, 0x100); memcpy(&a[0x40], "DQIX-PC", 7); }
        unsigned d[4] = { 0 };
        if (sscanf(dns ? dns : "", "%u.%u.%u.%u", &d[0], &d[1], &d[2], &d[3]) == 4)
            for (int i = 0; i < 4; i++) a[0xC8 + i] = (uint8_t)d[i];       /* primary DNS */
        memset(&a[0xCC], 0, 4);                                            /* no secondary */
        a[0xE6] = 0;                                                       /* no WEP */
        a[0xE7] = 0;                                                       /* status: normal */
        a[0xE8] = 7;                                                       /* SSID length */
        a[0xEF] = 1;                                                       /* connection configured */
    } else if (ours) {
        a[0xE7] = 0xFF;                                                    /* not configured */
    }
    ap_crc(a);
    g_fw_ap_dirty = 1;
    firmware_save_wireless();
}

/* the console's MAC address (firmware header, under the Wi-Fi config CRC) */
void firmware_set_mac(const uint8_t mac[6]) {
    memcpy(&g_fw[0x36], mac, 6);
    put16(&g_fw[0x2A], crc16_buf(0x0000, &g_fw[0x2C], 0x138));
    wifi_reset(g_fw);
}

static uint8_t spi_xfer(int dev, uint8_t in, int first) {
    switch (dev) {
    case 0: {   /* power management: [r/w|index] [data] */
        if (first) { g_spi_cmd = in; return 0; }
        int idx = g_spi_cmd & 7;
        if (g_spi_cmd & 0x80) return g_pm_regs[idx];
        g_pm_regs[idx] = in;
        return 0;
    }
    case 1: {   /* firmware flash */
        if (first) {
            firmware_save_wireless();                             /* the previous command may have written settings */
            g_spi_cmd = in; g_spi_pos = 0; g_fw_addr = 0;
            if (in == 0x04) g_fw_status &= (uint8_t)~2;           /* write disable */
            if (in == 0x06) g_fw_status |= 2;                     /* write enable */
            return 0;
        }
        int n = g_spi_pos++;
        if (g_spi_cmd == 0x03 || g_spi_cmd == 0x0B) {
            if (n < 3) { g_fw_addr = (g_fw_addr << 8) | in; return 0; }
            if (g_spi_cmd == 0x0B && n == 3) return 0;
            return g_fw[g_fw_addr++ % sizeof g_fw];
        }
        if (g_spi_cmd == 0x0A) {                                  /* page write (kept in memory) */
            if (n < 3) { g_fw_addr = (g_fw_addr << 8) | in; return 0; }
            uint32_t wa = g_fw_addr++ % sizeof g_fw;
            g_fw[wa] = in;
            if (wa >= FW_AP && wa < FW_AP + 0x300) g_fw_ap_dirty = 1;
            return in;
        }
        if (g_spi_cmd == 0x05) return g_fw_status;                /* status: ready, write-enable latch */
        if (g_spi_cmd == 0x9F) return n == 0 ? 0x20 : n == 1 ? 0x40 : 0x12;
        return 0xFF;
    }
    case 2: {   /* touch screen controller (input.c supplies the readings) */
        /* TSC2046: after a command byte, the next two bytes clock out the 12-bit
         * result (MSB first, one bit late). The SDK sends the next command as the
         * second of those bytes, so output first, then latch a new command. */
        uint16_t v = (uint16_t)(g_tsc_val << 3);
        uint8_t out = g_spi_pos == 0 ? (uint8_t)(v >> 8) : g_spi_pos == 1 ? (uint8_t)v : 0;
        g_spi_pos++;
        if (in & 0x80) {
            g_tsc_cmd = in;
            int ch = (in >> 4) & 7;
            g_tsc_val = input_touch_adc(ch);
            g_spi_pos = 0;
            static int tlog = -1; if (tlog < 0) tlog = getenv("DQIX_TSCLOG") != NULL;
            if (tlog && g_touch_down) fprintf(stderr, "[f%llu l%u] tsc cmd %02x ch %d -> %u (touch %d,%d)\n", (unsigned long long)g_frame, g_vcount, in, ch, g_tsc_val, g_touch_x, g_touch_y);
        }
        return out;
    }
    default: return 0xFF;
    }
}

void spi_write_data(uint16_t v) {
    uint16_t cnt = IO7_16(0x1C0);
    if (!(cnt & 0x8000)) return;
    int dev = (cnt >> 8) & 3;
    int first = g_spi_dev != dev;
    g_spi_out = spi_xfer(dev, (uint8_t)v, first);
    g_spi_dev = (cnt & 0x0800) ? dev : -1;                       /* chip select held? */
    if (cnt & 0x4000) irq_raise(&g_c7, IRQ_SPI);
}
uint16_t spi_read_data(void) { return g_spi_out; }
void spi_write_cnt(uint16_t v) {
    memcpy(&g_io7[0x1C0], &v, 2);
    if (!(v & 0x8000)) g_spi_dev = -1;
}

/* ================================================================ RTC (S-3511 over a 3-wire bus) */
static uint8_t g_rtc_bits, g_rtc_nbits, g_rtc_cmd, g_rtc_len, g_rtc_idx;
static int g_rtc_have_cmd, g_rtc_cs, g_rtc_sck;
static uint8_t g_rtc_buf[8], g_rtc_status1 = 0x02, g_rtc_status2 = 0x00;
static uint16_t g_rtc_reg;

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void rtc_begin_command(uint8_t raw) {
    /* fixed pattern 0110 in the high nibble when read MSB-first, else reversed */
    uint8_t c = raw;
    if ((c & 0xF0) != 0x60) {
        uint8_t r = 0;
        for (int i = 0; i < 8; i++) if (raw & (1 << i)) r |= 0x80 >> i;
        c = r;
    }
    g_rtc_cmd = c;
    int reg = (c >> 1) & 7, rd = c & 1;
    static const uint8_t lens[8] = { 1, 1, 7, 3, 3, 3, 1, 1 };
    g_rtc_len = lens[reg];
    g_rtc_idx = 0;
    if (rd) {
        time_t now = (time_t)(rtc_base() + (int64_t)(g_frame / 60));
        struct tm* t = gmtime(&now);                               /* base is already local time */
        memset(g_rtc_buf, 0, sizeof g_rtc_buf);
        switch (reg) {
        case 0: g_rtc_buf[0] = g_rtc_status1; break;
        case 1: g_rtc_buf[0] = g_rtc_status2; break;
        case 2:
            g_rtc_buf[0] = bcd(t->tm_year % 100); g_rtc_buf[1] = bcd(t->tm_mon + 1); g_rtc_buf[2] = bcd(t->tm_mday);
            g_rtc_buf[3] = bcd(t->tm_wday); g_rtc_buf[4] = bcd(t->tm_hour) | (t->tm_hour >= 12 ? 0x40 : 0);
            g_rtc_buf[5] = bcd(t->tm_min); g_rtc_buf[6] = bcd(t->tm_sec);
            break;
        case 3:
            g_rtc_buf[0] = bcd(t->tm_hour) | (t->tm_hour >= 12 ? 0x40 : 0); g_rtc_buf[1] = bcd(t->tm_min); g_rtc_buf[2] = bcd(t->tm_sec);
            break;
        default: break;
        }
    }
}

void rtc_write_reg(uint16_t v) {
    /* bit0 data, bit1 clock, bit2 select; bits 4-6: those pins are outputs */
    uint16_t dir = v & 0x70;
    int cs = (v >> 2) & 1, sck = (v >> 1) & 1, sio = v & 1;
    if (!(dir & 0x40)) cs = g_rtc_cs;
    if (!(dir & 0x20)) sck = g_rtc_sck;
    if (cs && !g_rtc_cs) { g_rtc_have_cmd = 0; g_rtc_nbits = 0; g_rtc_bits = 0; }
    if (cs && !g_rtc_sck && sck) {             /* rising clock edge: one bit */
        if (!g_rtc_have_cmd) {
            g_rtc_bits = (uint8_t)((g_rtc_bits << 1) | sio);
            if (++g_rtc_nbits == 8) { rtc_begin_command(g_rtc_bits); g_rtc_have_cmd = 1; g_rtc_nbits = 0; g_rtc_bits = 0; }
        } else if (!(g_rtc_cmd & 1)) {         /* write data, LSB first */
            if (dir & 0x10) g_rtc_bits |= (uint8_t)(sio << g_rtc_nbits);
            if (++g_rtc_nbits == 8) {
                int reg = (g_rtc_cmd >> 1) & 7;
                if (reg == 0 && g_rtc_idx == 0) g_rtc_status1 = (g_rtc_bits & 0x0E) | (g_rtc_status1 & 0xF1);
                if (reg == 1 && g_rtc_idx == 0) g_rtc_status2 = g_rtc_bits;
                g_rtc_idx++; g_rtc_nbits = 0; g_rtc_bits = 0;
            }
        } else {                                /* read data, LSB first */
            if (++g_rtc_nbits == 8) { g_rtc_idx++; g_rtc_nbits = 0; }
        }
    }
    g_rtc_cs = cs; g_rtc_sck = sck;
    g_rtc_reg = (uint16_t)(dir | (cs << 2) | (sck << 1) | sio);
}

uint16_t rtc_read_reg(void) {
    uint16_t v = g_rtc_reg;
    if (g_rtc_have_cmd && (g_rtc_cmd & 1) && !(v & 0x10)) {
        uint8_t byte = g_rtc_idx < g_rtc_len ? g_rtc_buf[g_rtc_idx] : 0;
        v = (uint16_t)((v & ~1u) | ((byte >> g_rtc_nbits) & 1));
    }
    return v;
}

/* The DS clock = the PC's local time at launch + emulated time, so a run can
 * be replayed exactly. The base is "local time as if it were UTC" (seconds),
 * which keeps replays independent of the replaying machine's time zone.
 * DQIX_RTC=<that value> fixes it (replays, tests). */
static int64_t g_rtc_base = -1;
void rtc_set_base(int64_t base) { g_rtc_base = base; }
int64_t rtc_base(void) {
    if (g_rtc_base < 0) {
        const char* e = getenv("DQIX_RTC");
        if (e) g_rtc_base = strtoll(e, NULL, 10);
        else {
            time_t now = time(NULL);
            struct tm lt = *localtime(&now);
            struct tm gt = *gmtime(&now);
            lt.tm_isdst = 0; gt.tm_isdst = 0;
            g_rtc_base = (int64_t)now + (int64_t)difftime(mktime(&lt), mktime(&gt));
        }
    }
    return g_rtc_base;
}
/* how far local time (the DS's clock) is ahead of UTC, in seconds: things the
 * original services did at a fixed UTC time (DQVC's new stock at 08:00 UTC)
 * use it. Replays and sessions carry it along (DQIX_UTC_OFFSET), so they
 * don't depend on the time zone of the PC that runs them. */
static int64_t g_utc_off = INT64_MIN;
void rtc_set_utc_offset(int64_t off) { g_utc_off = off; }
int64_t rtc_utc_offset(void) {
    if (g_utc_off == INT64_MIN) {
        const char* e = getenv("DQIX_UTC_OFFSET");
        if (e) g_utc_off = strtoll(e, NULL, 10);
        else if (getenv("DQIX_RTC")) g_utc_off = 0;                    /* an older replay: no zone known */
        else {
            time_t now = time(NULL);
            struct tm lt = *localtime(&now);
            struct tm gt = *gmtime(&now);
            lt.tm_isdst = 0; gt.tm_isdst = 0;
            g_utc_off = (int64_t)difftime(mktime(&lt), mktime(&gt));
        }
    }
    return g_utc_off;
}

/* ================================================================ registers */
static uint32_t reg7_read32(uint32_t off) {
    Core* k = &g_c7;
    switch (off) {
    case 0x004: return dispstat_read(k);
    case 0x100: case 0x104: case 0x108: case 0x10C: {
        int n = (off - 0x100) / 4;
        return timer_read(k, n) | ((uint32_t)k->timer[n].ctl << 16);
    }
    case 0x130: return input_keyinput() | ((uint32_t)IO7_16(0x132) << 16);
    case 0x134: return IO7_16(0x134) | ((uint32_t)input_extkeyin() << 16);   /* EXTKEYIN: X, Y, pen, hinge open */
    case 0x138: return rtc_read_reg();
    case 0x180: time_poll(k); return ipc_sync_read(k);
    case 0x184: time_poll(k); return ipc_fifocnt_read(k);
    case 0x1A0: case 0x1A4: case 0x1A8: case 0x1AC: return card_read(k, off, 4);
    case 0x1C0: return IO7_16(0x1C0) | ((uint32_t)spi_read_data() << 16);  /* never busy */
    case 0x204: return IO16(0x204);                               /* EXMEMSTAT mirrors the ARM9 setting */
    case 0x208: return k->ime;
    case 0x210: return k->ie;
    case 0x214: return k->iflags;
    case 0x240: {                                                 /* VRAMSTAT + WRAMSTAT */
        uint32_t st = ((g_io[0x242] & 0x87) == 0x82) | (((g_io[0x243] & 0x87) == 0x82) << 1);
        return st | ((uint32_t)(g_io[0x247] & 3) << 8);
    }
    default:
        if (off >= 0x400 && off < 0x500 && (off & 0xF) == 0) {     /* SOUNDxCNT: busy while the channel plays */
            uint32_t v = IO7_32(off) & ~0x80000000u;
            if (sound_chan_busy((off - 0x400) / 16)) v |= 0x80000000u;
            return v;
        }
        return IO7_32(off);
    }
}

uint32_t io7_read(uint32_t a, int size) {
    if (a >= 0x04800000 && a < 0x04810000) {
        time_poll(&g_c7);                                         /* status polling: let time move */
        uint32_t v;
        if (size == 4) v = wifi_read16(a & ~3u) | (uint32_t)wifi_read16((a & ~3u) + 2) << 16;
        else { v = wifi_read16(a & ~1u); if (size == 1) v = (v >> ((a & 1) * 8)) & 0xFF; }
        { static int lg = -1; if (lg < 0) lg = getenv("DQIX_WIFILOG") != NULL;
          if (lg > 0 && lg++ < 200000) fprintf(stderr, "wifi rd f%llu %08x = %08x (%d)\n", (unsigned long long)g_frame, a, v, size); }
        return v;
    }
    if (a >= 0x04100000) {
        if ((a & ~3u) == 0x04100000) return ipc_fifo_recv(&g_c7);
        if ((a & ~3u) == 0x04100010) return card_read_data(&g_c7);
        LOG_WARN("io7 read %08x\n", a);
        return 0;
    }
    uint32_t off = a - 0x04000000u;
    if (off >= sizeof g_io7) { LOG_WARN("io7 read %08x\n", a); return 0; }
    uint32_t v = reg7_read32(off & ~3u) >> ((off & 3) * 8);
    return size == 1 ? (v & 0xFF) : size == 2 ? (v & 0xFFFF) : v;
}

void io7_write(uint32_t a, uint32_t v, int size) {
    Core* k = &g_c7;
    if (a >= 0x04800000 && a < 0x04810000) {
        { static int lg = -1; if (lg < 0) lg = getenv("DQIX_WIFILOG") != NULL;
          if (lg > 0 && lg++ < 200000) fprintf(stderr, "wifi wr f%llu %08x = %08x (%d)\n", (unsigned long long)g_frame, a, v, size); }
        if (size == 4) { wifi_write16(a, (uint16_t)v); wifi_write16(a + 2, (uint16_t)(v >> 16)); }
        else if (size == 2) wifi_write16(a, (uint16_t)v);
        else {                                                    /* 8-bit writes: merge into the halfword */
            uint16_t cur = wifi_read16(a & ~1u);
            cur = (a & 1) ? (uint16_t)((cur & 0x00FF) | (v & 0xFF) << 8) : (uint16_t)((cur & 0xFF00) | (v & 0xFF));
            wifi_write16(a & ~1u, cur);
        }
        return; }
    if (a >= 0x04100000) { LOG_WARN("io7 write %08x\n", a); return; }
    uint32_t off = a - 0x04000000u;
    if (off >= sizeof g_io7) { LOG_WARN("io7 write %08x = %x\n", a, v); return; }
    uint32_t base = off & ~3u;
    uint32_t sh = (off & 3) * 8;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu << sh : 0xFFu << sh;
    uint32_t val32 = size == 4 ? v : (v << sh) & mask;
    switch (base) {
    case 0x214: k->iflags &= ~val32; return;
    case 0x208: k->ime = ((k->ime & ~mask) | (val32 & mask)) & 1; irq_service(&g_arm7); return;
    case 0x210: k->ie = (k->ie & ~mask) | (val32 & mask); irq_service(&g_arm7); return;
    case 0x1A0: case 0x1A4: case 0x1A8: case 0x1AC: card_write(k, off, v, size); return;
    case 0x1C0:
        if (off == 0x1C0) spi_write_cnt(size == 1 ? (uint16_t)((IO7_16(0x1C0) & 0xFF00) | (v & 0xFF)) : (uint16_t)v);
        if (off == 0x1C1) spi_write_cnt((uint16_t)((IO7_16(0x1C0) & 0x00FF) | ((v & 0xFF) << 8)));
        if ((off == 0x1C0 && size == 4) || off == 0x1C2) spi_write_data((uint16_t)(size == 4 ? v >> 16 : v));
        return;
    case 0x138: rtc_write_reg((uint16_t)v); return;
    case 0x300:                                                   /* POSTFLG / HALTCNT */
        if (off <= 0x301 && off + size > 0x301) {
            uint8_t halt = (uint8_t)(size == 1 ? v : v >> 8);
            if ((halt & 0xC0) == 0x80) { bios_halt(k); return; }
            if ((halt & 0xC0) == 0xC0) { LOG_WARN("ARM7 requested sleep mode\n"); bios_halt(k); return; }
        }
        break;
    }
    uint32_t old_cnt = IO7_32(base);
    memcpy(&g_io7[off], &v, size);
    if (base >= 0x400 && base < 0x500 && (base & 0xF) == 0) sound_cnt_written((base - 0x400) / 16, old_cnt);
    if (base == 0x508) {                                          /* SOUNDCAP0/1CNT */
        void sound_capture_written(int i, uint8_t v);
        for (int i = 0; i < 2; i++) if (off <= 0x508u + i && off + (uint32_t)size > 0x508u + i) sound_capture_written(i, g_io7[0x508 + i]);
    }
    switch (base) {
    case 0x0B8: case 0x0C4: case 0x0D0: case 0x0DC: if ((off & 3) + size > 2) dma_write_ctl(k, (base - 0xB8) / 12); break;
    case 0x100: case 0x104: case 0x108: case 0x10C: timer_write(k, (base - 0x100) / 4, off - base, v, size); break;
    case 0x180: ipc_sync_write(k, IO7_16(0x180)); break;
    case 0x304: wifi_set_power((IO7_16(0x304) >> 1) & 1); break;      /* POWCNT2: wireless on/off */
    case 0x184: ipc_fifocnt_write(k, IO7_16(0x184)); break;
    case 0x188: ipc_fifo_send(k, IO7_32(0x188)); break;
    default: break;
    }
}
