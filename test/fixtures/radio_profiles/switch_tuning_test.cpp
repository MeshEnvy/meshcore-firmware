// Exercise the production shared parser and persistence with deterministic
// radio admission and storage failures. The physical driver has its own tests.
#define main existing_cli_main
#include "cli_test.cpp"
#undef main

static_assert(sizeof(mesh::RadioSwitchTuning) == 2, "SPI 9 must not enlarge saved policy");
static_assert(sizeof(mesh::RadioSwitchCapabilities) == 2, "SPI 9 must reuse a capability bit");

struct TuningRadio : Radio {
  mesh::RadioSwitchCapabilities caps;
  mesh::RadioSwitchTuning tuning;
  mesh::RadioParamApplyResult admission = mesh::RadioParamApplyResult::APPLIED;
  uint32_t default_spi_hz = 8888888;
  unsigned boot_configurations = 0, attempts = 0, changes = 0, persists = 0;
  TuningRadio() {
    caps.flags = 127;
    caps.spi_clocks = 0x1e | mesh::RadioSwitchCapabilities::Spi9;
    tuning.flags = caps.flags;
  }
  mesh::RadioSwitchCapabilities switchTuningCapabilities() const override { return caps; }
  mesh::RadioSwitchTuning switchTuning() const override { return tuning; }
  uint32_t switchSpiHz() const override {
    if (tuning.spi_mhz == 9) return 8888888;
    return tuning.spi_mhz ? uint32_t(tuning.spi_mhz) * 1000000UL : default_spi_hz;
  }
  bool configureBootSwitchTuning(const mesh::RadioSwitchTuning& value) override {
    ++boot_configurations;
    if (!mesh::validRadioSwitchTuning(value, caps)) return false;
    tuning = value;
    return true;
  }
  mesh::RadioParamApplyResult trySetSwitchTuning(const mesh::RadioSwitchTuning& value,
      bool (*persist)(void*, const mesh::RadioSwitchTuning&) = nullptr,
      void* context = nullptr) override {
    ++attempts;
    if (!mesh::validRadioSwitchTuning(value, caps)) return mesh::RadioParamApplyResult::FAILED;
    if (admission != mesh::RadioParamApplyResult::APPLIED) return admission;
    const auto previous = tuning;
    tuning = value;
    if (persist) {
      ++persists;
      if (!persist(context, value)) {
        tuning = previous;
        return mesh::RadioParamApplyResult::FAILED;
      }
    }
    if (value != previous) {
      ++changes;
      p.resetSwitchTest();
    }
    return mesh::RadioParamApplyResult::APPLIED;
  }
};

struct TuningFixture {
  MemoryFS fs;
  Clock clock;
  TuningRadio radio;
  mesh::RadioProfileCLI cli;
  char reply[160];
  TuningFixture() { g_mock_millis = 0; cli.begin(&fs, &radio, &clock); }
  const char* cmd(const char* text, bool success = true, bool remote = false) {
    memset(reply, 0x55, sizeof(reply));
    assert(cli.handle(text, reply, sizeof(reply), remote));
    assert(memchr(reply, 0, sizeof(reply)));
    if (!strncmp(text, "set ", 4)) {
      if ((!strncmp(reply, "OK", 2)) != success) {
        fprintf(stderr, "%s -> %s\n", text, reply);
        assert(false);
      }
    }
    return reply;
  }
  void advance(uint32_t milliseconds) { g_mock_millis += milliseconds; cli.loop(); }
};

static void crc(std::vector<uint8_t>& image) {
  assert(image.size() == 24);
  uint32_t value = 0xffffffffU;
  for (size_t i = 0; i < image.size() - 4; ++i) {
    value ^= image[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0);
  }
  memcpy(image.data() + image.size() - 4, &value, 4);
}

static void controlsAndPersistence() {
  TuningFixture f;
  assert(f.radio.tuning.flags == 127 && !f.radio.tuning.spi_mhz);
  assert(f.fs.files.empty());
  const char* features[] = {"warm", "fs", "packet", "iq", "modem", "batch", "buffer"};
  for (unsigned i = 0; i < 7; ++i) {
    char command[48];
    snprintf(command, sizeof(command), "get radio.fast.%s", features[i]);
    assert(!strcmp(f.cmd(command), "> on"));
    snprintf(command, sizeof(command), "set radio.fast.%s off", features[i]);
    f.cmd(command);
    assert(!(f.radio.tuning.flags & (1U << i)));
    const auto expected = f.radio.tuning;
    TuningRadio reboot;
    mesh::RadioProfileCLI restored;
    restored.begin(&f.fs, &reboot, &f.clock);
    assert(reboot.tuning == expected);
    // An absent radio2 record must not prevent independent fast-policy restore.
    assert(!f.fs.files.count("/radio_profiles"));
    snprintf(command, sizeof(command), "set radio.fast.%s on", features[i]);
    f.cmd(command);
    assert(f.radio.tuning.flags & (1U << i));
  }
  for (unsigned mhz : {2, 4, 8, 9, 16, 0}) {
    char command[48];
    snprintf(command, sizeof(command), "set radio.fast.spi %u", mhz);
    f.cmd(command);
    assert(f.radio.tuning.spi_mhz == mhz);
    TuningRadio reboot;
    mesh::RadioProfileCLI restored;
    restored.begin(&f.fs, &reboot, &f.clock);
    assert(reboot.tuning == f.radio.tuning);
    assert(reboot.switchSpiHz() == (mhz == 9 || !mhz ? 8888888 : mhz * 1000000UL));
    const auto& image = f.fs.files["/radio_fast"];
    assert(image.size() == 24 && image[0] == 'R' && image[1] == '2' && image[2] == 1);
    assert(image[3] == f.radio.tuning.flags && image[4] == mhz);
    for (size_t i = 5; i < 20; ++i) assert(image[i] == 0);
  }
  assert(strstr(f.cmd("get radio.fast"), "8888888"));
  assert(!strcmp(f.cmd("get radio.fast.caps"),
      "> features: warm fs packet iq modem batch buffer; spi=0,2,4,8,9,16"));
  f.cmd("set radio.fast off");
  assert(!f.radio.tuning.flags && f.radio.tuning.spi_mhz == 2);
  f.cmd("set radio.fast on");
  assert(f.radio.tuning.flags == f.radio.caps.flags && !f.radio.tuning.spi_mhz);
  for (size_t size = 1; size <= sizeof(f.reply); ++size) {
    char buffer[161];
    memset(buffer, 0x55, sizeof(buffer));
    for (const char* command : {"get radio.fast", "get radio.fast.caps"}) {
      memset(buffer, 0x55, sizeof(buffer));
      assert(f.cli.handle(command, buffer, size));
      assert(memchr(buffer, 0, size) && buffer[size] == 0x55);
    }
  }
  puts("Fast tuning: independent features, board clocks, saved restore and bounded output passed");
}

static void strictInputsAndUnsupportedFeatures() {
  TuningFixture f;
  const auto original = f.radio.tuning;
  for (const char* command : {
      "set radio.fast", "set radio.fast maybe", "set radio.fast on extra",
      "set radio.fast.fs", "set radio.fast.fs ON", "set radio.fast.fs on extra",
      "set radio.fast.spi", "set radio.fast.spi -2", "set radio.fast.spi +8",
      "set radio.fast.spi 8mhz", "set radio.fast.spi 3", "set radio.fast.spi 32",
      "set radio.fast.spi 8.889", "set radio.fast.spi 8.888888", "set radio.fast.spi 9.0",
      "set radio.fast.spi +9", "set radio.fast.spi -9", "set radio.fast.spi 09",
      "set radio.fast.spi 9mhz", "set radio.fast.spi 9 0",
      "set radio.fast.spi 256", "set radio.fast.spi 8 16", "set radio.fast.caps on"}) {
    f.cmd(command, false);
    assert(f.radio.tuning == original && f.fs.files.empty());
  }
  for (const char* command : {"get radio.fast on", "get radio.fast.fs extra",
                              "get radio.fast.spi extra", "get radio.fast.caps extra"}) {
    assert(strstr(f.cmd(command), "Error"));
    assert(f.radio.tuning == original && f.fs.files.empty());
  }
  f.radio.caps.flags &= ~mesh::RadioSwitchTuning::Buffer;
  f.radio.tuning.flags &= f.radio.caps.flags;
  f.radio.caps.spi_clocks &= ~(1U << 4);
  const auto limited = f.radio.tuning;
  f.cmd("set radio.fast.buffer on", false);
  f.cmd("set radio.fast.spi 16", false);
  assert(f.radio.tuning == limited && f.fs.files.empty());
  assert(strstr(f.cmd("get radio.fast.buffer"), "unsupported"));
  assert(!strstr(f.cmd("get radio.fast.caps"), "buffer"));
  assert(strstr(f.reply, "; spi=0,2,4,8,9") && !strstr(f.reply, ",16"));
  f.cmd("set radio.fast off");
  f.cmd("set radio.fast on");
  assert(f.radio.tuning.flags == f.radio.caps.flags);
  f.radio.caps.spi_clocks = 0;
  assert(strstr(f.cmd("get radio.fast.caps"), "; spi=na"));
  assert(strstr(f.cmd("get radio.fast"), "> spi=na"));
  f.cmd("set radio.fast.spi 0", false);
  const auto no_clock_control = f.radio.tuning;
  f.cmd("set radio.fast off");
  assert(!f.radio.tuning.flags && !f.radio.tuning.spi_mhz);
  f.cmd("set radio.fast on");
  assert(f.radio.tuning == no_clock_control);
  puts("Fast tuning: strict parsing and unsupported controls passed");
}

static void roundedClockRequiresCapabilityAndBootAdmission() {
  TuningFixture f;
  // The integer token is deliberately an alias for the real divider rate,
  // independent of the board default rather than a request for 9000000 Hz.
  f.radio.default_spi_hz = 8000000;
  f.cmd("set radio.fast.spi 9");
  assert(f.radio.tuning.spi_mhz == 9 && f.radio.switchSpiHz() == 8888888);
  assert(strstr(f.cmd("get radio.fast.spi"), "> 9 (8888888Hz"));
  assert(strstr(f.cmd("get radio.fast"), "> spi=9 (8888888Hz)"));
  const auto saved = f.fs.files;
  TuningRadio reboot;
  reboot.default_spi_hz = 4000000;
  mesh::RadioProfileCLI::configureBootTuning(&f.fs, reboot);
  assert(reboot.tuning.spi_mhz == 9 && reboot.switchSpiHz() == 8888888);
  assert(reboot.boot_configurations == 1 && !reboot.attempts && !reboot.persists);
  assert(f.fs.files == saved);

  // nRF/RP controller caps may support 16 MHz while lacking this fractional
  // divider. Rejection must precede both driver admission and storage writes.
  f.cmd("set radio.fast.spi 8");
  f.radio.caps.spi_clocks &= ~mesh::RadioSwitchCapabilities::Spi9;
  const auto original = f.radio.tuning;
  const auto committed = f.fs.files;
  const auto attempts = f.radio.attempts;
  const auto persists = f.radio.persists;
  f.cmd("set radio.fast.spi 9", false);
  assert(f.radio.tuning == original && f.fs.files == committed);
  assert(f.radio.attempts == attempts && f.radio.persists == persists);
  assert(!strstr(f.cmd("get radio.fast.caps"), ",9"));

  // A saved 9 from a capable image remains protected when booted on a build
  // which cannot generate it; it must neither fall back to 9 MHz nor rewrite.
  f.fs.files = saved;
  TuningRadio unsupported;
  unsupported.caps.spi_clocks &= ~mesh::RadioSwitchCapabilities::Spi9;
  unsupported.default_spi_hz = 8000000;
  const auto defaults = unsupported.tuning;
  mesh::RadioProfileCLI::configureBootTuning(&f.fs, unsupported);
  assert(unsupported.tuning == defaults && unsupported.switchSpiHz() == 8000000);
  assert(!unsupported.boot_configurations && !unsupported.attempts && !unsupported.persists);
  assert(f.fs.files == saved);
  puts("Fast tuning: rounded SPI 9 uses exact divider Hz, persists unchanged and rejects unsupported clocks");
}

static void clockOnlyLegacyHalKeepsFeaturesUnavailable() {
  TuningFixture f;
  f.radio.caps.flags = f.radio.tuning.flags = 0;
  f.radio.caps.spi_clocks = 2 | 4 | 8 | mesh::RadioSwitchCapabilities::Spi9;
  f.radio.default_spi_hz = 2000000;
  assert(!strcmp(f.cmd("get radio.fast.caps"), "> features:; spi=0,2,4,8,9"));
  assert(!strcmp(f.cmd("get radio.fast"),
      "> spi=0 (2000000Hz) warm=na fs=na packet=na iq=na modem=na batch=na buffer=na"));
  f.cmd("set radio.fast.spi 9");
  assert(!f.radio.tuning.flags && f.radio.tuning.spi_mhz == 9);
  assert(f.radio.switchSpiHz() == 8888888);
  const auto saved = f.fs.files;
  TuningRadio reboot;
  reboot.caps = f.radio.caps;
  reboot.tuning.flags = 0;
  reboot.default_spi_hz = 2000000;
  mesh::RadioProfileCLI::configureBootTuning(&f.fs, reboot);
  assert(!reboot.tuning.flags && reboot.tuning.spi_mhz == 9);
  assert(reboot.switchSpiHz() == 8888888 && reboot.boot_configurations == 1);
  mesh::RadioProfileCLI restored;
  restored.begin(&f.fs, &reboot, &f.clock);
  assert(reboot.tuning == f.radio.tuning && f.fs.files == saved);

  const auto original = f.radio.tuning;
  const auto attempts = f.radio.attempts;
  const auto persists = f.radio.persists;
  for (const char* feature : {"warm", "fs", "packet", "iq", "modem", "batch", "buffer"}) {
    char command[48];
    snprintf(command, sizeof(command), "set radio.fast.%s on", feature);
    f.cmd(command, false);
    assert(f.radio.tuning == original && f.fs.files == saved);
    assert(f.radio.attempts == attempts && f.radio.persists == persists);
  }
  f.cmd("set radio.fast off");
  assert(!f.radio.tuning.flags && f.radio.tuning.spi_mhz == 2);
  assert(f.radio.switchSpiHz() == 2000000);
  f.cmd("set radio.fast on");
  assert(!f.radio.tuning.flags && !f.radio.tuning.spi_mhz);
  assert(f.radio.switchSpiHz() == 2000000);
  puts("Fast tuning: clock-only legacy HAL saves SPI 9 without exposing cached feature controls");
}

static void localOnlyAndBusyTransactions() {
  TuningFixture f;
  f.cmd("set radio.fast.fs off");
  const auto original = f.radio.tuning;
  const auto saved = f.fs.files["/radio_fast"];
  const unsigned persists = f.radio.persists;
  f.radio.admission = mesh::RadioParamApplyResult::FAILED;
  f.cmd("set radio.fast.fs on", false);
  assert(f.radio.tuning == original && f.radio.persists == persists);
  assert(f.fs.files["/radio_fast"] == saved);
  f.radio.admission = mesh::RadioParamApplyResult::APPLIED;
  assert(strstr(f.cmd("set radio.fast.fs on", false, true), "local"));
  f.cli.beginReplyCommand();
  assert(strstr(f.cmd("set radio.fast.fs on", false), "local"));
  assert(strstr(f.cmd("set radio.fast.fs on", false, true), "local"));
  f.cli.endReplyCommand();
  assert(!f.cli.hasReplyMutation());
  assert(f.radio.tuning == original && f.fs.files["/radio_fast"] == saved);
  assert(!strcmp(f.cmd("get radio.fast.fs", true, true), "> off"));
  puts("Fast tuning: busy admission and local-only changes passed");
}

static void busyTargetsComposeAndApplyOnce() {
  TuningFixture f;
  const auto original = f.radio.tuning;
  f.radio.admission = mesh::RadioParamApplyResult::BUSY;
  assert(strstr(f.cmd("set radio.fast.warm off"), "pending"));
  assert(f.cli.hasPendingTuning());
  assert(strstr(f.cmd("set radio.fast.spi 9"), "pending"));
  const auto committed = f.fs.files["/radio_fast"];
  assert(f.radio.tuning == original);
  // This pending feature must survive a rejected replacement save.
  f.fs.fail_write = true;
  f.cmd("set radio.fast.packet off", false);
  f.fs.fail_write = false;
  assert(f.fs.files["/radio_fast"] == committed);
  const auto saved = f.fs.files;
  for (unsigned i = 0; i < 40; ++i) f.advance(1000);
  assert(f.cli.hasPendingTuning() && f.fs.files == saved);
  // Completion hooks call this before the next queued packet starts.
  f.radio.admission = mesh::RadioParamApplyResult::APPLIED;
  f.cli.servicePendingTuning();
  assert(!f.cli.hasPendingTuning());
  assert(!(f.radio.tuning.flags & mesh::RadioSwitchTuning::Warm));
  assert(f.radio.tuning.flags & mesh::RadioSwitchTuning::Packet);
  assert(f.radio.tuning.spi_mhz == 9 && f.fs.files == saved);
  const auto attempts = f.radio.attempts;
  f.cli.servicePendingTuning();
  assert(f.radio.attempts == attempts);
  TuningRadio reboot;
  mesh::RadioProfileCLI restored;
  restored.begin(&f.fs, &reboot, &f.clock);
  assert(reboot.tuning == f.radio.tuning);
  puts("Fast tuning: BUSY targets compose, survive failed save and apply once without a retry command");
}

static void failedPreambleCompensationRepairsDurably() {
  for (bool rollover : {false, true}) for (bool explicit_save : {false, true}) {
    TuningFixture f;
    assert(f.cli.savePrimaryPreamble(32));
    const auto old = f.fs.files["/radio_profiles"];
    assert(f.cli.savePrimaryPreamble(64));
    const auto candidate = f.fs.files["/radio_profiles"];
    assert(candidate != old);
    const auto physical = f.radio.p.primary;
    f.fs.fail_write = true;
    assert(!f.cli.restorePrimaryPreamble(32));
    assert(f.cli.primaryPreamble() == 32 && f.cli.hasPendingTuning());
    assert(f.fs.files["/radio_profiles"] == candidate);
    // RAM equality is insufficient evidence of a durable compensation.
    assert(!f.cli.savePrimaryPreamble(32));
    assert(f.cli.hasPendingTuning());
    g_mock_millis = rollover ? UINT32_MAX - 500 : 100;
    f.cli.servicePendingTuning();
    f.fs.fail_write = false;
    if (explicit_save) {
      assert(f.cli.savePrimaryPreamble(32));
    } else {
      f.advance(999);
      assert(f.cli.hasPendingTuning() && f.fs.files["/radio_profiles"] == candidate);
      f.advance(1);
    }
    assert(!f.cli.hasPendingTuning() && f.fs.files["/radio_profiles"] == old);
    assert(f.radio.p.primary == physical); // Persistence repair never retunes RX.
    TuningRadio reboot; mesh::RadioProfileCLI restored;
    restored.begin(&f.fs, &reboot, &f.clock);
    assert(restored.primaryPreamble() == 32);
    assert(!f.cli.restorePrimaryPreamble(7));
    assert(f.cli.primaryPreamble() == 32 && !f.cli.hasPendingTuning());
  }
  puts("Preamble compensation: old runtime target, checked identical save, delayed durable repair and rollover passed");
}

static void storageFailureKeepsCommittedPolicy() {
  for (unsigned failure = 0; failure < 6; ++failure) {
    TuningFixture f;
    f.cmd("set radio.fast.fs off");
    const auto original = f.radio.tuning;
    const auto saved = f.fs.files["/radio_fast"];
    if (failure == 0) f.fs.fail_write = true;
    else if (failure == 1) f.fs.fail_write_after = 7;
    else if (failure == 2) f.fs.fail_read_after = 7;
    else if (failure == 3) f.fs.fail_rename = 1;
    else if (failure == 4) f.fs.fail_rename = 2;
    else {
      f.fs.files["/radio_fast.tmp"] = {0};
      f.fs.fail_remove = true;
    }
    f.cmd("set radio.fast.fs on", false);
    assert(f.radio.tuning == original);
    f.fs.fail_write = f.fs.fail_remove = false;
    f.fs.fail_write_after = f.fs.fail_read_after = -1;
    f.fs.fail_rename = 0;
    TuningRadio reboot;
    mesh::RadioProfileCLI restored;
    restored.begin(&f.fs, &reboot, &f.clock);
    assert(reboot.tuning == original);
    assert(f.fs.files["/radio_fast"] == saved);
  }
  puts("Fast tuning: short writes, readback and atomic rename failures passed");
}

static void bootRecoveryAndDeferredAdmission() {
  for (unsigned damaged_primary = 0; damaged_primary < 4; ++damaged_primary) {
    TuningFixture f;
    f.cmd("set radio.fast.modem off");
    const auto expected = f.radio.tuning;
    const auto saved = f.fs.files["/radio_fast"];
    assert(f.fs.rename("/radio_fast", "/radio_fast.bak"));
    if (damaged_primary == 1) f.fs.files["/radio_fast"] = {0};
    else if (damaged_primary >= 2) {
      auto& invalid = f.fs.files["/radio_fast"] = saved;
      invalid[damaged_primary == 2 ? 3 : 4] = damaged_primary == 2 ? 128 : 3;
      crc(invalid);
    }
    TuningRadio reboot;
    mesh::RadioProfileCLI restored;
    restored.begin(&f.fs, &reboot, &f.clock);
    assert(reboot.tuning == expected);
    assert(f.fs.files["/radio_fast"] == saved);
  }
  {
    TuningFixture f;
    f.cmd("set radio.fast.packet off");
    const auto expected = f.radio.tuning;
    const auto saved = f.fs.files;
    TuningRadio reboot;
    reboot.admission = mesh::RadioParamApplyResult::BUSY;
    mesh::RadioProfileCLI restored;
    restored.begin(&f.fs, &reboot, &f.clock);
    assert(reboot.tuning != expected && f.fs.files == saved);
    restored.loop();
    assert(reboot.tuning != expected);
    reboot.admission = mesh::RadioParamApplyResult::APPLIED;
    g_mock_millis += 1000;
    restored.loop();
    assert(reboot.tuning == expected && f.fs.files == saved);
  }
  {
    TuningFixture f;
    f.cmd("set radio.fast.iq off");
    const auto expected = f.radio.tuning;
    f.fs.files["/radio_profiles"] = {0};
    TuningRadio reboot;
    mesh::RadioProfileCLI restored;
    restored.begin(&f.fs, &reboot, &f.clock);
    assert(reboot.tuning == expected); // A corrupt radio2 image is isolated.
  }
  puts("Fast tuning: interrupted commit, deferred boot admission and store isolation passed");
}

static void malformedAndUnreadableImagesStayProtected() {
  for (unsigned corruption = 0; corruption < 5; ++corruption) {
    TuningFixture f;
    f.cmd("set radio.fast.fs off");
    auto& image = f.fs.files["/radio_fast"];
    if (corruption == 0) image[0] ^= 1;
    else if (corruption == 1) { image[3] = 128; crc(image); }
    else if (corruption == 2) { image[4] = 3; crc(image); }
    else if (corruption == 3) { image[5] = 1; crc(image); }
    else { image[2] = 2; crc(image); }
    const auto damaged = image;
    TuningRadio reboot;
    mesh::RadioProfileCLI restored;
    restored.begin(&f.fs, &reboot, &f.clock);
    assert(reboot.tuning.flags == 127 && !reboot.tuning.spi_mhz);
    assert(f.fs.files["/radio_fast"] == damaged);
  }
  {
    TuningFixture f;
    f.cmd("set radio.fast.fs off");
    const auto saved = f.fs.files["/radio_fast"];
    f.fs.fail_read_open = true;
    TuningRadio reboot;
    mesh::RadioProfileCLI restored;
    restored.begin(&f.fs, &reboot, &f.clock);
    f.fs.fail_read_open = false;
    assert(restored.handle("set radio.fast.fs off", f.reply, sizeof(f.reply)));
    assert(strstr(f.reply, "Error"));
    assert(f.fs.files["/radio_fast"] == saved);
  }
  puts("Fast tuning: corrupt, unsupported, reserved and unreadable images protected");
}

static void bootRetriesAreBoundedAcrossRollover() {
  for (bool rollover : {false, true}) {
    for (const auto admission : {mesh::RadioParamApplyResult::BUSY,
                                 mesh::RadioParamApplyResult::FAILED}) {
      TuningFixture f;
      f.cmd("set radio.fast.fs off");
      const auto expected = f.radio.tuning;
      const auto saved = f.fs.files;
      TuningRadio reboot;
      reboot.admission = admission;
      mesh::RadioProfileCLI restored;
      g_mock_millis = rollover ? UINT32_MAX - 500 : 100;
      restored.begin(&f.fs, &reboot, &f.clock);
      assert(reboot.attempts == 1);
      // Hundreds of main-loop visits must not keep trying a rejected clock or
      // RX policy. Millisecond rollover does not make its deadline due early.
      for (unsigned i = 0; i < 999; ++i) {
        ++g_mock_millis;
        restored.loop();
        assert(reboot.attempts == 1);
      }
      ++g_mock_millis;
      restored.loop();
      assert(reboot.attempts == 2 && reboot.tuning != expected);
      restored.loop();
      // BUSY is just an admission check: service can use the next TX gap.
      // Real hardware failures retain the one-second retry backoff.
      const unsigned before = admission == mesh::RadioParamApplyResult::BUSY ? 3 : 2;
      assert(reboot.attempts == before);
      reboot.admission = mesh::RadioParamApplyResult::APPLIED;
      g_mock_millis += 1000;
      restored.loop();
      assert(reboot.attempts == before + 1 && reboot.tuning == expected);
      assert(f.fs.files == saved && !reboot.persists);
    }
  }
  puts("Fast tuning: failed/busy boot retries bounded across timer rollover");
}

static void savedSlowClockPrecedesRadioInitialization() {
  for (bool backup : {false, true}) {
    TuningFixture f;
    f.cmd("set radio.fast off");
    const auto expected = f.radio.tuning;
    assert(!expected.flags && expected.spi_mhz == 2);
    if (backup) {
      assert(f.fs.rename("/radio_fast", "/radio_fast.bak"));
      f.fs.files["/radio_fast"] = {0};
    }
    TuningRadio reboot;
    reboot.admission = mesh::RadioParamApplyResult::FAILED;
    assert(reboot.switchSpiHz() == 8888888);
    mesh::RadioProfileCLI::configureBootTuning(&f.fs, reboot);
    // This is the policy used by the chip's first initialization transaction.
    // No runtime admission, RX restart, or persistence callback is permitted
    // before that initialization; a runtime failure cannot veto boot policy.
    assert(reboot.tuning == expected && reboot.switchSpiHz() == 2000000);
    assert(reboot.boot_configurations == 1 && !reboot.attempts);
    assert(!reboot.changes && !reboot.persists);
    reboot.admission = mesh::RadioParamApplyResult::APPLIED;
    mesh::RadioProfileCLI restored;
    restored.begin(&f.fs, &reboot, &f.clock);
    assert(reboot.tuning == expected);
  }
  for (unsigned unavailable = 0; unavailable < 4; ++unavailable) {
    TuningFixture f;
    if (unavailable != 0) f.cmd("set radio.fast off");
    if (unavailable == 1) f.fs.files["/radio_fast"][0] ^= 1;
    if (unavailable == 2) f.fs.fail_read_open = true;
    TuningRadio reboot;
    const auto defaults = reboot.tuning;
    mesh::RadioProfileCLI::configureBootTuning(unavailable == 3 ? nullptr : &f.fs, reboot);
    assert(reboot.tuning == defaults && reboot.switchSpiHz() == 8888888);
    assert(!reboot.boot_configurations && !reboot.attempts && !reboot.persists);
  }
  puts("Fast tuning: saved fallback clock configured before first radio initialization");
}

int main() {
  controlsAndPersistence();
  strictInputsAndUnsupportedFeatures();
  roundedClockRequiresCapabilityAndBootAdmission();
  clockOnlyLegacyHalKeepsFeaturesUnavailable();
  localOnlyAndBusyTransactions();
  busyTargetsComposeAndApplyOnce();
  failedPreambleCompensationRepairsDurably();
  storageFailureKeepsCommittedPolicy();
  bootRecoveryAndDeferredAdmission();
  malformedAndUnreadableImagesStayProtected();
  bootRetriesAreBoundedAcrossRollover();
  savedSlowClockPrecedesRadioInitialization();
}
