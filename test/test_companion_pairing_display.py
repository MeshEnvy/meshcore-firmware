#!/usr/bin/env python3
"""Run production pairing UI methods and keep exactly one visible PIN."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_replay_reset_integration import extract_braced

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <Arduino.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/BluetoothPairingUiPolicy.h>
#include <helpers/ui/CompanionHomeLayout.h>
#include <algorithm>
#include <cassert>
#include <string>
#include <vector>
ColorVal UIColor::window_bkg = 0, UIColor::title_bkg = 0, UIColor::title_txt = 1;
ColorVal UIColor::primary_txt = 1, UIColor::secondary_txt = 1, UIColor::warning_txt = 1;
ColorVal UIColor::popup_bkg = 0, UIColor::popup_txt = 1, UIColor::corp_blue = 1;
constexpr unsigned long BLE_PAIRING_DISPLAY_MILLIS = 120000;
struct TestDisplay : DisplayDriver {
  bool on = false;
  std::vector<std::string> text;
  TestDisplay(int width, int height) : DisplayDriver(width, height) {}
  bool isOn() override { return on; }
  void turnOn() override { on = true; }
  void turnOff() override { on = false; }
  void clear() override {}
  void startFrame(ColorVal) override { text.clear(); }
  void setTextSize(int) override {}
  void setColor(ColorVal) override {}
  void setCursor(int, int) override {}
  void print(const char* value) override { text.emplace_back(value); }
  void fillRect(int, int, int, int) override {}
  void drawRect(int, int, int, int) override {}
  void drawXbm(int, int, const uint8_t*, int, int) override {}
  uint16_t getTextWidth(const char* value) override { return strlen(value) * 6; }
  void endFrame() override {}
};
struct Board {
  bool usb = true;
  bool isExternalPowered() const { return usb; }
  bool isUsbHostConnected() const { return usb; }
};
struct Interfaces {
  bool pending = false, enabled = true, usb = true, ble = false;
  bool takePairingRequest() { bool result = pending; pending = false; return result; }
};
struct UIScreen {};
struct HomeScreen : UIScreen {
  bool pairing_pin_visible = false;
  bool hasPairingPin() const { return pairing_pin_visible; }
  void showFirstPage() {}
  void flushPocketSettings() {}
};
struct MsgPreviewScreen : UIScreen { bool hasMessages() const { return false; } };
struct Mesh {
  uint32_t pin = 246810;
  uint32_t getBLEPin() const { return pin; }
  void notificationButton() {}
} the_mesh;
struct UITask {
  TestDisplay* _display;
  Board* _board;
  Interfaces* _interfaceManager;
  HomeScreen home_screen;
  MsgPreviewScreen preview_screen;
  UIScreen* home = &home_screen;
  UIScreen* msg_preview = &preview_screen;
  UIScreen* curr = home;
  unsigned long _pairing_screen_until = 0;
  uint32_t _next_refresh = 0;
  bool _pairing_from_button = false, _deferred_msg_preview = false;
  int _msgcount = 0;
  UITask(TestDisplay* display, Board* board, Interfaces* interfaces)
      : _display(display), _board(board), _interfaceManager(interfaces) {}
  bool isBluetoothEnabled() const { return _interfaceManager->enabled; }
  bool hasBluetoothConnection() const { return _interfaceManager->ble; }
  bool hasConnection() const { return _interfaceManager->ble || _interfaceManager->usb; }
  const char* connectedClientLabel() const {
    return _interfaceManager->ble ? "BLUETOOTH" : _interfaceManager->usb ? "USB" : nullptr;
  }
  void setCurrScreen(UIScreen* screen);
  void gotoHomeScreen() { setCurrScreen(home); }
  bool isPairingPromptActive() const { return isPairingScreenActive(); }
  bool isPairingScreenActive() const;
  void renderPairingBanner();
  void showPairingPin(bool from_button = false);
  void finishPairingScreen(bool timed_out);
  void servicePairingState();
  char checkDisplayOn(char c);
  void navigation(char c);
};
@METHODS@
static void homePairing(UITask* _task, TestDisplay& display) {
  bool& _pairing_pin_visible = _task->home_screen.pairing_pin_visible;
  _pairing_pin_visible = false;
#if !UI_POCKET_GUI
  const int home_content_top = 20;
  @HOME_SELECTION@
#endif
}
static void checkPairingPresentation(UITask& ui, TestDisplay& display) {
  display.text.clear();
  homePairing(&ui, display);
  const bool home_pin = std::count(display.text.begin(), display.text.end(), "246810") == 1;
  assert(ui.home_screen.hasPairingPin() == home_pin);
  ui.renderPairingBanner();
  if (home_pin) {
    assert(std::count(display.text.begin(), display.text.end(), "246810") == 1);
    assert(std::count(display.text.begin(), display.text.end(), "PAIR PIN 246810") == 0);
    // Another home page or asynchronous screen still needs the top PIN.
    ui.home_screen.pairing_pin_visible = false;
    display.text.clear();
    ui.renderPairingBanner();
    assert(display.text.back() == "PAIR PIN 246810");
    ui.home_screen.pairing_pin_visible = true;
    ui.curr = ui.msg_preview;
    display.text.clear();
    ui.renderPairingBanner();
    assert(display.text.back() == "PAIR PIN 246810");
    ui.curr = ui.home;
    return;
  }
  assert(display.text.back() == "PAIR PIN 246810");
}
int main(int argc, char** argv) {
  using namespace mesh::ui;
  g_mock_millis = 100;
  displayPowerPrefs() = DisplayPowerPrefs();
  displayPowerPrefs().usb.mode = DisplayMode::ButtonPairing;
  Board board;
  Interfaces interfaces;
  TestDisplay display(argc > 1 ? atoi(argv[1]) : 128,
                      argc > 2 ? atoi(argv[2]) : 64);
  UITask ui{&display, &board, &interfaces};
  ui.servicePairingState();
  assert(!display.isOn() && !ui.isPairingScreenActive());
  interfaces.pending = true; // Real PIN request, while USB remains connected.
  ui.servicePairingState();
  assert(display.isOn() && ui.isPairingScreenActive());
  assert(!ui._pairing_from_button && interfaces.usb && !interfaces.ble);
  checkPairingPresentation(ui, display);
  ui.navigation('n'); // Navigation may not steal a real phone pairing prompt.
  assert(ui.isPairingScreenActive());
  g_mock_millis += 120000;
  ui.servicePairingState();
  assert(!ui.isPairingScreenActive() && !display.isOn());

  // Manual button wake must expose the PIN despite USB and still allow paging.
  assert(ui.checkDisplayOn('n') == 0);
  assert(display.isOn() && ui.isPairingScreenActive() && ui._pairing_from_button);
  assert(ui._pairing_screen_until - millis() == 15000);
  checkPairingPresentation(ui, display);
  ui.navigation('n');
  assert(!ui.isPairingScreenActive() && !ui._pairing_from_button);
  display.text.clear();
  homePairing(&ui, display);
#if UI_POCKET_GUI
  assert(display.text.empty()); // Pocket's board view has no standalone PIN.
#else
  if (display.height() >= 64)
    assert(display.text.back() == "246810"); // USB must not replace the idle PIN.
  else assert(display.text.empty()); // Too short: only the pairing overlay fits.
#endif

  interfaces.pending = true;
  ui.servicePairingState();
  interfaces.ble = true; // Successful BLE pairing, not merely a USB connection.
  ui.servicePairingState();
  assert(!ui.isPairingScreenActive());
  display.text.clear();
  homePairing(&ui, display);
#if !UI_POCKET_GUI
  if (display.height() >= 64) assert(display.text.back() == "CONNECTED");
  else assert(display.text.empty());
#endif

  // Explicit off must remain off; disabling BLE must not reveal a stale PIN.
  interfaces.ble = false;
  displayPowerPrefs().usb.mode = DisplayMode::Off;
  ui.servicePairingState();
  interfaces.pending = true;
  ui.servicePairingState();
  assert(!display.isOn());
  ui.finishPairingScreen(false);
  ui.checkDisplayOn('n');
  assert(!display.isOn() && !ui._pairing_from_button);
  displayPowerPrefs().usb.mode = DisplayMode::ButtonPairing;
  interfaces.enabled = false;
  ui.servicePairingState();
  ui.checkDisplayOn('n');
  assert(display.isOn() && !ui.isPairingScreenActive());
  display.text.clear();
  homePairing(&ui, display);
#if !UI_POCKET_GUI
  if (display.height() >= 64)
    assert(display.text.back() == "CONNECTED"); // Only USB status remains.
  else assert(display.text.empty());
#endif
}
'''


class CompanionPairingDisplayTests(unittest.TestCase):
    def test_real_pairing_methods_and_usb_oled_selection(self):
        source = (ROOT / "examples/companion_radio/ui-new/UITask.cpp").read_text()
        signatures = (
            "void UITask::setCurrScreen(",
            "bool UITask::isPairingScreenActive() const",
            "void UITask::renderPairingBanner()",
            "void UITask::showPairingPin(bool from_button)",
            "void UITask::finishPairingScreen(bool timed_out)",
            "void UITask::servicePairingState()",
            "char UITask::checkDisplayOn(char c)",
        )
        methods = "\n".join(extract_braced(source, signature) + "\n"
                            for signature in signatures)
        navigation = extract_braced(source, "if (_pairing_from_button && c != 0)")
        methods += "void UITask::navigation(char c) {" + navigation + "}\n"
        marker = "#else\n      const bool bluetooth_enabled = _task->isBluetoothEnabled();"
        compact = source[source.index(marker) + len("#else\n"):]
        condition = "if (client_label != nullptr || show_bluetooth_pin)"
        selection = compact[:compact.index(condition)] + extract_braced(compact, condition) + "\n"
        program = HARNESS.replace("@METHODS@", methods).replace("@HOME_SELECTION@", selection)
        with tempfile.TemporaryDirectory(prefix="mesh-pairing-display-") as temp:
            cpp, binary = Path(temp) / "test.cpp", Path(temp) / "test"
            cpp.write_text(program)
            for profile, flags, sizes in (
                ("shared", [], [(128, 64), (160, 80), (80, 160), (128, 128),
                                 (240, 135), (320, 170), (320, 240), (240, 320),
                                 (250, 122), (200, 200), (128, 24)]),
                ("t096", ["-DHELTEC_T096=1"], [(160, 80), (80, 160), (128, 64)]),
                ("pocket", ["-DUI_POCKET_GUI=1"], [(250, 122), (200, 200)]),
            ):
                with self.subTest(profile=profile):
                    result = subprocess.run([
                        "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", *flags,
                        "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                        "-isystem", str(ROOT / "test/mocks"), "-I", str(ROOT / "src"),
                        str(cpp), str(ROOT / "src/helpers/ui/DisplayDriver.cpp"),
                        "-o", str(binary),
                    ], text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    for width, height in sizes:
                        with self.subTest(width=width, height=height):
                            result = subprocess.run([str(binary), str(width), str(height)],
                                                    text=True, capture_output=True)
                            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
