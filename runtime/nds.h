/* DQIX PC runtime: shared state for the emulated DS hardware (both CPUs). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

/* ---- running inside another program (hosting.c) ---- */
typedef struct {
    const char* const* env;               /* "NAME=value" settings for this console, NULL-terminated */
    void (*yield)(void);                  /* waiting: let the other consoles run */
    void (*exit_console)(int code);       /* this console stops (never returns) */
    const uint8_t* rom; size_t rom_size;  /* the ROM, already loaded */
} CoreHost;
int rt_hosted(void);
char* rt_getenv(const char* name);        /* getenv, or the host's settings for this console */
_Noreturn void rt_exit(int code);
int rt_yield(void);                       /* 1 if the host ran others meanwhile */
int rt_host_rom(const uint8_t** rom, size_t* size);
#define getenv(name) rt_getenv(name)

/* ---- logging ---- */
extern int g_log_level;
void rt_log(int level, const char* fmt, ...);
#define LOG_WARN(...)  rt_log(1, __VA_ARGS__)
#define LOG_INFO(...)  rt_log(2, __VA_ARGS__)
#define LOG_DEBUG(...) rt_log(3, __VA_ARGS__)
_Noreturn void rt_fatal(const char* fmt, ...);

/* ---- cartridge ---- */
extern uint8_t* g_rom;
extern size_t g_rom_size;
/* The European version (game code YDQP): its main ARM9 code has 16 bytes more
 * (the language choice), so the code after them is 0x10 further on. */
static inline int rom_is_eur(void) {
    return g_rom && g_rom_size >= 16 && g_rom[12] == 'Y' && g_rom[13] == 'D' && g_rom[14] == 'Q' && g_rom[15] == 'P';
}

/* ---- memory ---- */
#define MAIN_RAM_SIZE (4u << 20)
#define ITCM_SIZE     0x8000u
#define DTCM_SIZE     0x4000u
#define SWRAM_SIZE    0x8000u
#define WRAM7_SIZE    0x10000u
extern uint8_t g_main_ram[MAIN_RAM_SIZE];
extern uint8_t g_itcm[ITCM_SIZE];
extern uint8_t g_dtcm[DTCM_SIZE];
extern uint8_t g_swram[SWRAM_SIZE];      /* shared WRAM, split by WRAMCNT */
extern uint8_t g_wram7[WRAM7_SIZE];      /* ARM7-only WRAM */
extern uint8_t g_palette[0x800];
extern uint8_t g_oam[0x800];
extern uint8_t g_vram[0xA4000];          /* banks A..I back to back */
extern uint32_t g_dtcm_base;
extern int g_itcm_enabled, g_dtcm_enabled;
void vram_remap(void);
uint8_t* vram_ptr(uint32_t addr);        /* ARM9 view; NULL when unmapped */
uint8_t* vram7_ptr(uint32_t addr);       /* ARM7 view (banks C/D) */
uint8_t* vram_bank(int b);               /* bank A..I = 0..8 */
extern uint8_t* g_tex_slot[4];           /* 3D texture image slots (128 KB each), NULL if unmapped */
extern uint8_t* g_texpal_slot[6];        /* 3D texture palette slots (16 KB each) */
void gpu3d_vblank(void);                 /* latch registers, swap the polygon list */
void gpu3d_render_start(void);           /* scanline 215: draw the 3D frame */
uint32_t gx_read(uint32_t off);
int gx_irq_line(void);
int64_t rtc_base(void);                  /* clock start (local time, seconds) */
void rtc_set_base(int64_t base);         /* sessions: everyone uses the host's */
int64_t rtc_utc_offset(void);             /* local time - UTC, seconds (DQIX_UTC_OFFSET) */
void rtc_set_utc_offset(int64_t off);
/* reports (report.c) */
void report_write(const char* reason);
int report_zip(const char* dir, int with_saves, char* out, size_t outn);   /* the report as one .zip (path in out) */
void report_log_line(const char* line);
/* sound (sound.c) */
void sound_advance(uint32_t cycles);
void sound_cnt_written(int n, uint32_t old);
int sound_chan_busy(int n);
uint32_t sound_read(int16_t* dst, uint32_t frames);
uint32_t sound_buffered(void);
void sound_discard(void);                   /* GXFIFO interrupt condition (level) */          /* 3D registers 0x600-0x6A3 */
void gx_write(uint32_t off, uint32_t v); /* geometry FIFO / command ports 0x400-0x5FF, GXSTAT */
uint32_t vram_bank_size(int b);
uint8_t  vram_bank_cnt(int b);           /* its VRAMCNT value */

/* ---- graphics ---- */
void gpu_render_line(int line);          /* both 2D engines, one scanline */
void gpu_frame_start(void);              /* line 0: latch per-frame state */
void gpu_frame_done(void);               /* VBlank: frame complete */
void gpu_affine_written(int engine, int bg);
extern uint32_t g_screen[2][256 * 192];  /* top, bottom: 0x00RRGGBB */
/* Writes into the overlay area bump this so overlay detection re-checks. */
extern uint32_t g_overlay_area_start, g_overlay_area_end, g_overlay_gen;

/* ARM7 memory (mem7.c); the ARM9's mem_* functions are declared in cpu.h */
uint8_t  mem7_read8(uint32_t a);
uint16_t mem7_read16(uint32_t a);
uint32_t mem7_read32(uint32_t a);
void     mem7_write8(uint32_t a, uint8_t v);
void     mem7_write16(uint32_t a, uint16_t v);
void     mem7_write32(uint32_t a, uint32_t v);

/* A CPU's view of memory plus its function dispatcher. */
typedef struct Bus {
    uint8_t  (*r8)(uint32_t);
    uint16_t (*r16)(uint32_t);
    uint32_t (*r32)(uint32_t);
    void (*w8)(uint32_t, uint8_t);
    void (*w16)(uint32_t, uint16_t);
    void (*w32)(uint32_t, uint32_t);
    void (*call)(Cpu*, uint32_t, uint32_t);
} Bus;

/* ---- I/O ---- */
uint32_t io9_read(uint32_t addr, int size);
void     io9_write(uint32_t addr, uint32_t val, int size);
uint32_t io7_read(uint32_t addr, int size);
void     io7_write(uint32_t addr, uint32_t val, int size);
extern uint8_t g_io[0x2000];             /* ARM9 registers 0x04000000-0x04001FFF (also card regs) */
extern uint8_t g_io7[0x1000];            /* ARM7 registers 0x04000000-0x04000FFF */
static inline uint32_t IO32(uint32_t off) { uint32_t v; memcpy(&v, &g_io[off], 4); return v; }
static inline uint16_t IO16(uint32_t off) { uint16_t v; memcpy(&v, &g_io[off], 2); return v; }
static inline uint32_t IO7_32(uint32_t off) { uint32_t v; memcpy(&v, &g_io7[off], 4); return v; }
static inline uint16_t IO7_16(uint32_t off) { uint16_t v; memcpy(&v, &g_io7[off], 2); return v; }

/* ---- per-CPU state ---- */
enum {
    IRQ_VBLANK = 0, IRQ_HBLANK = 1, IRQ_VCOUNT = 2, IRQ_TIMER0 = 3, IRQ_RTC = 7, IRQ_DMA0 = 8, IRQ_KEYPAD = 12,
    IRQ_IPCSYNC = 16, IRQ_IPC_SEND_EMPTY = 17, IRQ_IPC_RECV_NOT_EMPTY = 18,
    IRQ_CARD_DONE = 19, IRQ_GXFIFO = 21, IRQ_SPI = 23, IRQ_WIFI = 24,
};
enum { DMA_IMMEDIATE = 0, DMA_VBLANK = 1, DMA_HBLANK = 2, DMA_CARD = 5, DMA_GXFIFO = 7, DMA_NONE = -1 };

typedef struct Timer { uint16_t reload, ctl; uint64_t start; uint64_t overflows_seen; uint16_t latched; } Timer;
typedef struct Dma { uint32_t src, dst, ctl; int pending; } Dma;
struct ThreadSys;

typedef struct Core {
    int id;                      /* 0 = ARM9, 1 = ARM7 */
    const char* name;
    Cpu* cpu;
    const Bus* bus;
    uint32_t ime, ie, iflags;
    int irq_depth;
    Timer timer[4];
    Dma dma[4];
    uint8_t* io;                 /* register backing store */
    struct ThreadSys* threads;
    uint32_t tick;               /* function-call budget for forward progress */
} Core;
extern Core g_c9, g_c7;
extern Cpu g_arm9, g_arm7;
static inline Core* core_of(Cpu* c) { return c == &g_arm7 ? &g_c7 : &g_c9; }

/* interrupt vector / IntrWait flag locations, as the BIOS uses them */
uint32_t core_irq_handler_slot(Core* k);
uint32_t core_irq_check_slot(Core* k);

/* ---- interrupts / timing ---- */
extern uint64_t g_frame;                 /* frames since boot */
extern uint32_t g_vcount;                /* current scanline 0..262 */
extern uint64_t g_frame_limit;           /* stop after this many frames (0 = never) */
void irq_raise(Core* k, int bit);
void irq_service(Cpu* c);                /* deliver pending IRQs if enabled */
void time_advance_lines(uint32_t lines);
void time_poll(Core* k);                 /* a status-register read: let time pass a little */
void time_spend(uint32_t cycles);        /* the ARM9 burned this many cycles */
uint64_t time_cycles(void);              /* 33.5 MHz bus cycles since boot */
uint16_t timer_read(Core* k, int n);
void timer_write(Core* k, int n, uint32_t off_in_reg, uint32_t val, int size);

/* ---- DMA, card, math ---- */
void dma_write_ctl(Core* k, int ch);
void dma_on_event(Core* k, int timing);
uint32_t card_read(Core* k, uint32_t off, int size);       /* 0x1A0-0x1AF */
void card_write(Core* k, uint32_t off, uint32_t v, int size);
uint32_t card_read_data(Core* k);
void backup_init(const char* save_path);
void backup_flush(void);
const uint8_t* backup_image(uint32_t* size);   /* the save as the game will see it */
int backup_unlock_dlc(void);              /* downloadable quests + special guests (see card.c) */
void math_div(void);
void math_sqrt(void);

/* ---- IPC ---- */
uint16_t ipc_sync_read(Core* k);
void     ipc_sync_write(Core* k, uint16_t v);
uint16_t ipc_fifocnt_read(Core* k);
void     ipc_fifocnt_write(Core* k, uint16_t v);
void     ipc_fifo_send(Core* k, uint32_t v);
uint32_t ipc_fifo_recv(Core* k);

/* ---- the ARM7 as a second CPU ---- */
void arm7_boot(void);
void arm7_run(void);                     /* ARM9 side: let the ARM7 run until it waits */
void arm7_yield(void);                   /* ARM7 side: give control back to the ARM9 */
int  arm7_is_running(void);

/* ---- CPU helpers ---- */
int  cpu_mode_index(uint32_t mode);
void cpu_set_mode(Cpu* c, uint32_t mode);

/* ---- dispatch / overlays / tracing ---- */
void dispatch_init(void);
void dispatch7_call(Cpu* c, uint32_t target, uint32_t from);
const char* recomp_func_name(uint32_t addr);
const char* recomp7_func_name(uint32_t addr);
void overlays_init(const char* extract_dir);
void trace_dump(FILE* f, int n);

/* ---- game threads (both CPUs) ---- */
void threads_init(void);
void threads_after_irq(Cpu* c);
void* threads_current_coro(Core* k);
void threads_start_arm7(uint32_t entry);
extern unsigned g_thread_switches;

/* ---- BIOS ---- */
void bios7_swi(Cpu* c, uint32_t n);
void cpu7_unhandled(Cpu* c, uint32_t addr, uint32_t insn);
void bios_intr_wait(Core* k, int discard, uint32_t mask);
void bios_halt(Core* k);

/* ---- input (input.c) ---- */
void input_update(void);
uint16_t input_keyinput(void);
uint16_t input_extkeyin(void);
uint16_t input_touch_adc(int channel);

/* ---- devices on the ARM7 side ---- */
void spi_write_cnt(uint16_t v);
uint16_t spi_read_data(void);
void spi_write_data(uint16_t v);
void firmware_init(void);
const uint8_t* firmware_image(uint32_t* size);
int firmware_load_image(const char* path);
void firmware_image_set_mac(uint8_t* img, const uint8_t mac[6]);
/* wifi.c: the wireless chip */
void wifi_reset(const uint8_t* firmware);
void firmware_load_wireless(void);
void firmware_set_ap(int on, const char* dns);
void firmware_set_mac(const uint8_t mac[6]);
void firmware_write_wireless(FILE* f);
void net_deliver(const uint8_t* frame, int len);    /* a frame to the DS (the built-in server) */
void wfc_enable(int on, const char* dir);           /* the built-in WFC server (wfc.c) */
int wfc_enabled(void);
void wfc_frame(void);
void wfc_poll(void);
int wfc_input(const uint8_t* frame, int len);               /* the Wi-Fi settings area (reports) */
void net_enable(int on);
void net_seal(int on);                       /* sessions: only the built-in server, in game time */
typedef struct { int online; char dns[32]; uint8_t mac[6]; int mac_set; int builtin; } WirelessConfig;   /* builtin: wfc.c answers */
extern WirelessConfig g_wireless;                  /* set by the frontend before boot */
/* the DS's language (firmware user settings): 1 English, 2 French, 3 German, 4 Italian,
 * 5 Spanish. The European version plays in it; the USA version is always in English. */
extern int g_fw_language;                  /* set by the frontend before boot */
void wifi_set_power(int on);
void wifi_advance(uint32_t cycles);
uint16_t wifi_read16(uint32_t addr);
void wifi_write16(uint32_t addr, uint16_t val);
const uint8_t* firmware_user_settings(void);   /* 0x70 bytes, copied to 0x027FFC80 at boot */
uint32_t firmware_user_offset(void);
uint8_t rtc_io(uint8_t v, int write);
void rtc_write_reg(uint16_t v);
uint16_t rtc_read_reg(void);
