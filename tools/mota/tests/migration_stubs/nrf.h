#pragma once
#include <stdint.h>

#define ACL_ACL_PERM_READ_Msk  4u
#define ACL_ACL_PERM_WRITE_Msk 2u
#define NRF_SUCCESS            0u

struct MigrationTestPower {
  uint32_t GPREGRET, GPREGRET2;
};
struct MigrationTestFicr {
  struct {
    uint32_t PART;
  } INFO;
  uint32_t CODEPAGESIZE, CODESIZE;
};
struct MigrationTestAcl {
  struct {
    uint32_t ADDR, SIZE, PERM;
  } ACL[8];
};
struct MigrationTestUicr {
  uint32_t NRFFW[2];
};
struct MigrationTestWdt {
  uint32_t RUNSTATUS, CRV, RREN, RR[8];
};

extern MigrationTestPower *NRF_POWER;
extern MigrationTestFicr *NRF_FICR;
extern MigrationTestAcl *NRF_ACL;
extern MigrationTestUicr *NRF_UICR;
extern MigrationTestWdt *NRF_WDT;

void NVIC_SystemReset();
