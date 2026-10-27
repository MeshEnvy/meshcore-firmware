#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ed_25519.h"

#ifndef BENCH_COMPACT
#define BENCH_COMPACT 0
#endif
#define SAMPLES 9
#define REG32(a) (*(volatile uint32_t *)(a))
struct Case {
  uint8_t public_key[32], shared_secret[32], signatures[3][64];
  uint32_t cycles[5][SAMPLES];
};
struct Report {
  uint32_t magic, schema, compact, cpu_hz, samples, complete, cpuid, clock_stat;
  uint32_t overhead[SAMPLES];
  uint32_t wdt_run, wdt_config, wdt_crv;
  struct Case cases[2];
};
__attribute__((used)) volatile struct Report report;
static const uint8_t seeds[2][32] = {
 {0x9d,0x61,0xb1,0x9d,0xef,0xfd,0x5a,0x60,0xba,0x84,0x4a,0xf4,0x92,0xec,0x2c,0xc4,0x44,0x49,0xc5,0x69,0x7b,0x32,0x69,0x19,0x70,0x3b,0xac,0x03,0x1c,0xae,0x7f,0x60},
 {0x4c,0xcd,0x08,0x9b,0x28,0xff,0x96,0xda,0x9d,0xb6,0xc3,0x46,0xec,0x11,0x4e,0x0f,0x5b,0x8a,0x31,0x9f,0x35,0xab,0xa6,0x24,0xda,0x8c,0xf6,0xed,0x4f,0xb8,0xa6,0xfb}
};
static uint8_t public_keys[2][32], private_keys[2][64], message[256];
static void feed_watchdog(void) {
  if (REG32(0x40010400)) {
    uint32_t enabled = REG32(0x40010508);
    for (unsigned n = 0; n < 8; ++n)
      if (enabled & (1U << n)) REG32(0x40010600 + n*4) = 0x6E524635;
  }
}
static inline void barrier(void) { __asm volatile("dsb\n isb" ::: "memory"); }
static inline uint32_t ticks(void) { barrier(); return REG32(0xE0001004); }
__attribute__((noinline,used,externally_visible)) void benchmark(void) {
  /* Reset has disabled the SoftDevice. Mask all interrupts, including USB. */
  __asm volatile("cpsid i" ::: "memory");
  REG32(0xE000E010) = 0;
  REG32(0xE000ED88) |= 0xF << 20;
  feed_watchdog();
  REG32(0x40000000) = 1; /* HFCLKSTART */
  while ((REG32(0x4000040C) & 0x10001) != 0x10001) { feed_watchdog(); }
  REG32(0xE000EDFC) |= 1 << 24; /* TRCENA */
  REG32(0xE0001004) = 0;
  REG32(0xE0001000) |= 1;
  barrier();
  memset((void *)&report, 0, sizeof(report));
  report.magic = 0x52414B43;
  report.schema = 1;
  report.compact = BENCH_COMPACT;
  report.cpu_hz = 64000000;
  report.samples = SAMPLES;
  report.cpuid = REG32(0xE000ED00);
  report.clock_stat = REG32(0x4000040C);
  report.wdt_run = REG32(0x40010400);
  report.wdt_config = REG32(0x4001050C);
  report.wdt_crv = REG32(0x40010504);
  for (unsigned i = 0; i < sizeof(message); ++i) message[i] = (i * 73 + 19) & 255;
  for (unsigned i = 0; i < SAMPLES; ++i) {
    uint32_t start = ticks();
    uint32_t end = ticks();
    report.overhead[i] = end - start;
  }
  for (unsigned v = 0; v < 2; ++v) ed25519_create_keypair(public_keys[v], private_keys[v], seeds[v]);
  for (unsigned v = 0; v < 2; ++v) {
    for (unsigned operation = 0; operation < 5; ++operation) {
      for (unsigned n = 0; n < SAMPLES + 1; ++n) {
        feed_watchdog();
        uint32_t start = ticks();
        if (operation == 0) {
          ed25519_create_keypair(public_keys[v], private_keys[v], seeds[v]);
        } else if (operation == 4) {
          ed25519_key_exchange((uint8_t *)report.cases[v].shared_secret, public_keys[1-v], private_keys[v]);
        } else {
          unsigned length = operation == 1 ? 0 : (operation == 2 ? 96 : 256);
          ed25519_sign((uint8_t *)report.cases[v].signatures[operation-1], message, length, public_keys[v], private_keys[v]);
        }
        uint32_t end = ticks();
        if (n) report.cases[v].cycles[operation][n-1] = end-start;
      }
    }
    memcpy((void *)report.cases[v].public_key, public_keys[v], 32);
  }
  report.complete = 0xC0DEF17E;
  barrier();
  __asm volatile("bkpt #0");
  for (;;) {}
}
__attribute__((naked,used,section(".entry"))) void bench_start(void) {
  __asm volatile("cpsid i\n ldr sp, =0x20040000\n movs r0, #0\n msr control, r0\n isb\n bl benchmark\n bkpt #0\n b .");
}
