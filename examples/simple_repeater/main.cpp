#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "MyMesh.h"

#ifdef ESP32
#include <WiFi.h>
#include <LotatoConfig.h>
#include <LotatoIngestor.h>
#include <lofi/Lofi.h>
#endif
#include <lolog/LoLog.h>
#include <loserial/LoSerial.h>
#include <lofs/LoFS.h>

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() {
  while (1) ;
}

// Must fit `lotato endpoint ` + LotatoConfig ingest URL (257) with terminator.
static char command[288];

// For power saving
unsigned long lastActive = 0; // mark last active time
unsigned long nextSleepinSecs = 120; // next sleep in seconds. The first sleep (if enabled) is after 2 minutes from boot

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
static unsigned long userBtnDownAt = 0;
#define USER_BTN_HOLD_OFF_MILLIS 1500
#endif

void setup() {
#if defined(ARDUINO_ARCH_ESP32)
  Serial.setTxBufferSize(1024);
#endif
  Serial.begin(115200);
  delay(1000);
  ::loserial::LoSerial::begin(Serial);

#ifdef ESP32
  // VFS logs [E] for every fopen of a missing file; LoDB/LoSettings treat "missing" as normal
  // (get returns NOT_FOUND). Silence the noise — real errors elsewhere still log.
  esp_log_level_set("vfs_api", ESP_LOG_NONE);
#endif

  board.begin();

#if defined(MESH_DEBUG) && defined(NRF52_PLATFORM)
  // give some extra time for serial to settle so
  // boot debug messages can be seen on terminal
  delay(5000);
#endif

  // For power saving
  lastActive = millis(); // mark last active time since boot

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) {
    MESH_DEBUG_PRINTLN("Radio init failed!");
    halt();
  }

  fast_rng.begin(radio_get_rng_seed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  LittleFS.begin(true);
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif

  LoFS::mountDefaults();
  ::lolog::LoLog::registerConfigSchema();
  ::lolog::LoLog::loadFromSettings();
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  ::loserial::LoSerial::printf("Repeater ID: ");
  mesh::Utils::printHex(::loserial::LoSerial::stream(), the_mesh.self_id.pub_key, PUB_KEY_SIZE);
  ::loserial::LoSerial::printLine("");

  command[0] = 0;

  sensors.begin();

#ifdef ESP32
  // Load config before begin() so debug flag is active during node store init.
  LotatoConfig::instance().load();
#endif

  the_mesh.begin(fs);

#ifdef ESP32
  lotato_register_sta_dns_override();
  {
    auto& pm_cfg = LotatoConfig::instance();
    if (pm_cfg.ssid()[0] != '\0') {
      // Don't call WiFi.begin here — `Lofi::begin()` already kicked the boot scan, and its
      // completion path invokes `resumeStaSavedCredentials()`. Touching the radio here would
      // race the scan state machine.
      board.setInhibitSleep(true);
      if (lofi::Lofi::instance().knownWifiCount() >= 2) WiFi.setAutoReconnect(false);
    }
  }
#endif

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif
}

void loop() {
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    // LF-only line endings (common from web serial / Unix terminals) were ignored before,
    // so the command buffer never completed and the CLI looked hung.
    if (c == '\n') {
      Serial.print('\n');
      if (len > 0) {
        command[len++] = '\r';
        command[len] = 0;
      }
      break;
    }
    command[len++] = c;
    command[len] = 0;
    Serial.print(c);
    if (c == '\r') {
      if (Serial.available() && Serial.peek() == '\n') {
        Serial.read();
      }
      break;
    }
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    Serial.print('\n');
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[MyMesh::kCliReplyCap];
    reply[0] = '\0';
    char cmd_snap[sizeof(command)];
    strncpy(cmd_snap, command, sizeof(cmd_snap) - 1);
    cmd_snap[sizeof(cmd_snap) - 1] = '\0';
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
    if (::lolog::LoLog::isVerbose()) {
      ::lolog::LoLog::debug("lotato.cli", "serial cmd: %.200s", cmd_snap);
      ::lolog::LoLog::debug("lotato.cli", "serial reply: %.200s", reply);
    }
    if (reply[0]) {
      ::loserial::LoSerial::printMeshCliReply(reply);
    }

    command[0] = 0;  // reset command buffer
  }

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
  // Hold the user button to power off the SenseCAP Solar repeater.
  int btnState = digitalRead(PIN_USER_BTN);
  if (btnState == LOW) {
    if (userBtnDownAt == 0) {
      userBtnDownAt = millis();
    } else if ((unsigned long)(millis() - userBtnDownAt) >= USER_BTN_HOLD_OFF_MILLIS) {
      ::loserial::LoSerial::printLine("Powering off...");
      board.powerOff();  // does not return
    }
  } else {
    userBtnDownAt = 0;
  }
#endif

  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();

  if (the_mesh.getNodePrefs()->powersaving_enabled && !the_mesh.hasPendingWork()) {
    #if defined(NRF52_PLATFORM)
    board.sleep(1800); // nrf ignores seconds param, sleeps whenever possible
    #else
    if (the_mesh.millisHasNowPassed(lastActive + nextSleepinSecs * 1000)) { // To check if it is time to sleep
      board.sleep(1800);             // To sleep. Wake up after 30 minutes or when receiving a LoRa packet
      lastActive = millis();
      nextSleepinSecs = 5;  // Default: To work for 5s and sleep again
    } else {
      nextSleepinSecs += 5; // When there is pending work, to work another 5s
    }
    #endif
  }
}
