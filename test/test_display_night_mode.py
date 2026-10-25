"""Run the real night schedule, saved CLI and redraw policy on the host."""
from datetime import datetime, timezone, timedelta
from pathlib import Path
import math
import os
import subprocess
import tempfile
import unittest

from test_display_power import FS_STUB

ROOT = Path(__file__).resolve().parents[1]


def compile_run(source, files=(), defines=(), stdin=""):
    with tempfile.TemporaryDirectory(prefix="mesh-night-") as directory:
        work = Path(directory)
        (work / "helpers").mkdir()
        (work / "helpers/IdentityStore.h").write_text(
            FS_STUB.replace("/display_prefs", "/display_night"), encoding="ascii")
        (work / "Arduino.h").write_text(
            '#pragma once\n#include <stdint.h>\ninline uint32_t millis() { return 0; }\n', encoding="ascii")
        (work / "test.cpp").write_text(source, encoding="ascii")
        command = [os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                   "-I" + str(work), "-I" + str(ROOT / "src"), *defines,
                   str(work / "test.cpp"), *(str(ROOT / path) for path in files), "-o", str(work / "test")]
        built = subprocess.run(command, capture_output=True, text=True)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)
        ran = subprocess.run([str(work / "test")], input=stdin, capture_output=True, text=True)
        if ran.returncode:
            raise AssertionError(ran.stdout + ran.stderr)
        return ran.stdout


class DisplayNightModeTests(unittest.TestCase):
    def test_small_integer_schedule_against_independent_solar_reference(self):
        # NOAA's general solar equations provide an independent, more precise
        # test oracle, not a firmware dependency:
        # https://gml.noaa.gov/grad/solcalc/solareqns.PDF
        samples = []
        for latitude, longitude in ((47.6, -122.3), (0, 0), (-33.9, 151.2),
                                    (51.5, 0), (-45, 170), (60, 25),
                                    (35, 179.8), (35, -179.8)):
            for month in (1, 3, 6, 9, 12):
                date = datetime(2028, month, 21, tzinfo=timezone.utc)
                gamma = 2 * math.pi / 365 * (date.timetuple().tm_yday - 1)
                eqtime = 229.18 * (0.000075 + 0.001868 * math.cos(gamma)
                    - 0.032077 * math.sin(gamma) - 0.014615 * math.cos(2 * gamma)
                    - 0.040849 * math.sin(2 * gamma))
                decl = (0.006918 - 0.399912 * math.cos(gamma) + 0.070257 * math.sin(gamma)
                    - 0.006758 * math.cos(2 * gamma) + 0.000907 * math.sin(2 * gamma)
                    - 0.002697 * math.cos(3 * gamma) + 0.001480 * math.sin(3 * gamma))
                lat = math.radians(latitude)
                hour = math.degrees(math.acos((math.cos(math.radians(90.833))
                    - math.sin(lat) * math.sin(decl)) / (math.cos(lat) * math.cos(decl))))
                dawn = 720 - 4 * longitude - eqtime - 4 * hour
                dusk = 720 - 4 * longitude - eqtime + 4 * hour
                for delay in (-120, 0, 60, 180):
                    # One hour either side of the reference transition: broad
                    # deliberately, matching this approximate dusk feature.
                    for minute, expected in ((dawn - 60, True), (dawn + 60, False),
                                             (dusk + delay - 60, False), (dusk + delay + 60, True)):
                        # Near midsummer at 60N a three-hour dusk delay can
                        # consume all of the short night. Test ordinary-night
                        # samples here and exercise that boundary separately.
                        if 1440 - 8 * hour - delay <= 120 or 8 * hour + delay <= 120:
                            continue
                        utc = int((date + timedelta(minutes=minute)).timestamp())
                        samples.append((utc, round(latitude * 1e6), round(longitude * 1e6), delay, expected))
        source = r'''
#include <helpers/ui/DisplayNightPolicy.h>
#include <cstdio>
int main() {
  unsigned utc; int lat, lon, delay;
  while (scanf("%u %d %d %d", &utc, &lat, &lon, &delay) == 4)
    printf("%d\n", mesh::ui::night_policy::active(utc, lat, lon, delay));
}
'''
        output = compile_run(source, stdin="".join(
            f"{utc} {lat} {lon} {delay}\n" for utc, lat, lon, delay, _ in samples))
        for sample, actual in zip(samples, output.splitlines()):
            self.assertEqual(actual == "1", sample[-1], sample)
        self.assertEqual(len(output.splitlines()), len(samples))

    def test_polar_invalid_and_midnight_delay_boundaries(self):
        compile_run(r'''
#include <helpers/ui/DisplayNightPolicy.h>
#include <cassert>
using namespace mesh::ui::night_policy;
int main() {
  const uint32_t june = 1782000000; // 2026-06-21 midnight UTC
  const uint32_t december = 1797811200; // 2026-12-21 midnight UTC
  assert(!active(june, 85000000, 0, 60));
  assert(active(december, 85000000, 0, 60));
  assert(active(june, -85000000, 0, 60));
  assert(!active(december, -85000000, 0, 60));
  assert(!active(0, 47000000, -122000000, 60));
  assert(!active(june, 91000000, 0, 60));
  assert(!active(june, 0, -181000000, 60));
  // Longitude arithmetic must remain bounded at the uint32 RTC ceiling.
  assert(active(UINT32_MAX, 0, 180000000, 60)
      == active(UINT32_MAX - 86400, 0, 180000000, 60));
  // At 60N a long positive delay crosses midnight. It must not activate
  // before that previous sunset's delay, nor remain red after sunrise.
  assert(!active(june, 60000000, 0, 180));
  assert(active(june + 3600, 60000000, 0, 180));
  assert(!active(june + 6 * 3600, 60000000, 0, 180));
}
''')

    def test_real_cli_persistence_failed_saves_and_immediate_manual_mode(self):
        source = r'''
#include <helpers/ui/DisplayNightSettings.h>
#include <helpers/ui/DisplayPowerSettings.h>
#include <cassert>
#include <string>
#if defined(NRF52_PLATFORM)
#define ATOMIC_FILE_WRITER_IMPLEMENTATION
#include <helpers/AtomicFileWriter.h>
#endif
using namespace mesh::ui;
int main() {
  FakeFS fs;
  char reply[160];
  loadDisplayPowerSettings(&fs, true);
  assert(displayNightMode() == NightAuto && displayNightDelayMinutes() == 60);
  assert(!displayNightActive());
  assert(handleDisplayPowerCommand("set display.night on", reply, sizeof(reply)));
  assert(!strcmp(reply, "OK") && displayNightActive()); // no clock/GPS needed
  assert(fs.files["/display_night"].size() == 8);
  assert(handleDisplayPowerCommand("get display.night", reply, sizeof(reply)));
  assert(!strcmp(reply, "> on"));
  assert(handleDisplayPowerCommand("set display.night.delay -180", reply, sizeof(reply)));
  assert(!strcmp(reply, "OK") && displayNightDelayMinutes() == -180);
  assert(handleDisplayPowerCommand("set display.night.delay 0", reply, sizeof(reply)));
  assert(!strcmp(reply, "OK") && displayNightDelayMinutes() == 0);
  assert(handleDisplayPowerCommand("set display.night off", reply, sizeof(reply)));
  assert(!strcmp(reply, "OK") && !displayNightActive());
  loadDisplayNightSettings(&fs);
  assert(displayNightMode() == NightOff && displayNightDelayMinutes() == 0);
  for (const char* cmd : {"set display.night AUTO", "set display.night.delay 181",
      "set display.night.delay -181", "set display.night.delay 1junk",
      "set display.night.delay ", "get display.night 1"}) {
    assert(handleDisplayPowerCommand(cmd, reply, sizeof(reply)));
    assert(std::string(reply).find("Error:") == 0);
    assert(displayNightMode() == NightOff && displayNightDelayMinutes() == 0);
  }
  fs.fail_write = true;
  assert(handleDisplayPowerCommand("set display.night on", reply, sizeof(reply)));
  assert(std::string(reply).find("Error:") == 0 && !displayNightActive());
  fs.fail_write = false;
  assert(handleDisplayPowerCommand("set display.night auto", reply, sizeof(reply)));
  updateDisplayNightMode(1797811200, true, 85000000, 0);
  assert(displayNightActive());
  updateDisplayNightMode(1797811201, false);
  assert(!displayNightActive()); // lost/expired location is not permanent GPS
  updateDisplayNightMode(1797811202, true, 85000000, 0);
  assert(displayNightActive());
  fs.files["/display_night"][7] ^= 1;
  loadDisplayNightSettings(&fs);
  assert(displayNightMode() == NightAuto && displayNightDelayMinutes() == 60);
  // Recovery followed by a failed update must preserve the recovered record.
  assert(handleDisplayPowerCommand("set display.night on", reply, sizeof(reply)));
  fs.files["/display_night.bak"] = fs.files["/display_night"];
  fs.files["/display_night"][0] ^= 1;
  loadDisplayNightSettings(&fs);
  assert(displayNightMode() == NightOn);
  fs.fail_publish = true;
  handleDisplayPowerCommand("set display.night off", reply, sizeof(reply));
  assert(std::string(reply).find("Error:") == 0);
  fs.fail_publish = false;
  loadDisplayNightSettings(&fs);
  assert(displayNightMode() == NightOn && displayNightActive());
}
'''
        files = ("src/helpers/ui/DisplayNightMode.cpp", "src/helpers/ui/DisplayPowerSettings.cpp")
        for platform in ((), ("-DNRF52_PLATFORM",)):
            with self.subTest(platform=platform):
                compile_run(source, files, ("-DDISPLAY_CLASS=ST7735Display", *platform))

    def test_monochrome_has_no_night_runtime_or_store(self):
        compile_run(r'''
#include <helpers/ui/DisplayNightSettings.h>
#include <cassert>
int main() {
  FakeFS fs;
  mesh::ui::loadDisplayNightSettings(&fs);
  mesh::ui::updateDisplayNightMode(1797811200, true, 85000000, 0);
  assert(!mesh::ui::displayNightActive() && fs.calls == 0);
}
''', ("src/helpers/ui/DisplayNightMode.cpp",), ("-DDISPLAY_CLASS=SSD1306Display",))

    def test_palette_transition_invalidates_cached_ui_without_power_cycling(self):
        compile_run(r'''
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/DisplayNightSettings.h>
#include <cassert>
struct Screen : DisplayDriver {
  Screen() : DisplayDriver(160, 80) {}
  bool on = true;
  unsigned starts = 0, stops = 0;
  bool isOn() override { return on; }
  void turnOn() override { on = true; ++starts; }
  void turnOff() override { on = false; ++stops; }
  void clear() override {}
  void startFrame(ColorVal) override {}
  void endFrame() override {}
  void setTextSize(int) override {}
  void setColor(ColorVal) override {}
  void setCursor(int, int) override {}
  void print(const char*) override {}
  void fillRect(int, int, int, int) override {}
  void drawRect(int, int, int, int) override {}
  void drawXbm(int, int, const uint8_t*, int, int) override {}
  uint16_t getTextWidth(const char*) override { return 6; }
};
int main() {
  FakeFS fs;
  mesh::ui::loadDisplayNightSettings(&fs);
  mesh::ui::displayPowerPrefs().battery.mode = mesh::ui::DisplayMode::On;
  Screen display;
  assert(!display.servicePower(false));
  char reply[160];
  mesh::ui::handleDisplayNightCommand("set display.night on", reply, sizeof(reply));
  assert(display.servicePower(false)); // existing UI power hook forces redraw
  assert(!display.servicePower(false));
  mesh::ui::handleDisplayNightCommand("set display.night off", reply, sizeof(reply));
  assert(display.servicePower(false));
  assert(!display.servicePower(false));
  assert(display.starts == 0 && display.stops == 0);
}
''', ("src/helpers/ui/DisplayNightMode.cpp", "src/helpers/ui/DisplayDriver.cpp"),
            ("-DDISPLAY_CLASS=ST7735Display",))


if __name__ == "__main__":
    unittest.main()
