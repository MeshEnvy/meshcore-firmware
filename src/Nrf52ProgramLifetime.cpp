#if defined(NRF52_PLATFORM) && defined(MESH_NRF52_NO_PROGRAM_EXIT) && MESH_NRF52_NO_PROGRAM_EXIT

// The qualified Adafruit nRF52 Arduino main never exits: the scheduler runs
// forever and its only return path invokes NVIC_SystemReset(). Device reboot
// and update paths also reset the MCU rather than terminating a C++ process.
// Do not retain registrations for global process-exit cleanup which cannot
// run. Automatic RAII destructors, explicit close/shutdown and dynamic delete
// remain ordinary C++ operations and are unaffected by this ABI entry point.
// nrf52_flash_trim.py enables this only for the exact reviewed core main hash.
extern "C" __attribute__((visibility("hidden"))) int __aeabi_atexit(
    void*, void (*)(void*), void*) {
  return 0;
}

#endif
