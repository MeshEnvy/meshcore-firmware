#!/usr/bin/env python3
"""Render production home-page branches; reject overlapping/clipped text boxes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

PREAMBLE = r'''
#include <helpers/ui/CompanionHomeLayout.h>
#include <helpers/ui/BluetoothPairingUiPolicy.h>
#include <helpers/ui/ReaderNavigationHint.h>
#include <helpers/ui/RadioProfileDisplayPage.h>
#include <RadioProfiles.h>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
ColorVal UIColor::window_bkg=0, UIColor::title_bkg=0, UIColor::title_txt=1;
ColorVal UIColor::primary_txt=1, UIColor::secondary_txt=1, UIColor::warning_txt=1;
ColorVal UIColor::popup_bkg=0, UIColor::popup_txt=1, UIColor::corp_blue=1;
#define PRESS_LABEL "Press"
#define SELECT_LABEL "Select"
struct Display : DisplayDriver {
  struct Row { mesh::ui::DisplayRegion box; std::string text; };
  std::vector<Row> rows;
  int size=1,x=0,y=0,small,large,render_size;
  Display(int w,int h,int a,int b,int r=0):DisplayDriver(w,h),small(a),large(b),render_size(r) {}
  int renderWidth() const override { return render_size?render_size:width(); }
  int renderHeight() const override { return render_size?render_size:height(); }
  bool isOn() override { return true; }
  void turnOn() override {} void turnOff() override {}
  void clear() override { rows.clear(); }
  void startFrame(ColorVal=0) override { clear(); } void endFrame() override {}
  void setTextSize(int s) override { size=s; }
  int textLineHeight() override { return size==1?small:size==2?large:size*small; }
  void setColor(ColorVal) override {}
  void setCursor(int a,int b) override { x=a;y=b; }
  uint16_t getTextWidth(const char* text) override {
    int w=0; for(;*text;++text) w += small==8 ? 6*size : (*text=='i'?3:9)*size;
    return w;
  }
  void print(const char* text) override {
    if(!*text) return;
    mesh::ui::DisplayRegion box{x,y,getTextWidth(text),textLineHeight()};
    assert(x>=0 && y>=0 && box.right()<=width() && box.bottom()<=height());
    for(const auto& row:rows) assert(!mesh::ui::displayRegionsOverlap(box,row.box));
    rows.push_back({box,text});
  }
  void fillRect(int x,int y,int w,int h) override {
    assert(x>=0 && y>=0 && w>=0 && h>=0 && x+w<=width() && y+h<=height());
  }
  void drawRect(int x,int y,int w,int h) override { fillRect(x,y,w,h); }
  void drawXbm(int,int,const uint8_t*,int,int) override {}
  const Row* find(const char* text) {
    for(const auto& row:rows) if(row.text==text) return &row;
    return nullptr;
  }
};
struct Task {
  bool enabled=true,connected=false,prompt=false;
  int count=0;
  bool isBluetoothEnabled(){return enabled;} bool hasBluetoothConnection(){return connected;}
  bool isPairingPromptActive(){return prompt;} int getPreviewCount(){return count;}
  int getMsgCount(){return count;}
  const char* inboxTitle(){return "HISTORY";}
  const char* connectedClientLabel(){return connected ? "BLUETOOTH" : nullptr;}
};
struct Mesh {
  uint32_t pin=123456;
  uint32_t getBLEPin(){return pin;}
  bool isDualRadioActive(){return false;}
} the_mesh;
const mesh::RadioProfiles* configuredRadioProfiles(){return nullptr;}
bool showingSecondaryRadioProfilePage(){return false;}
struct HomePage { enum { FIRST=0 }; };
struct IPAddress { int operator[](int i){return 255-i;} };
struct Wifi { IPAddress localIP(){return {};} int status(){return 3;} } WiFi;
constexpr int WL_CONNECTED=3;
bool isCompanionWiFiEnabled(){return true;}
bool isCompanionWiFiConnected(){return true;}
bool hasCompanionWiFiCredentials(){return true;}
struct Prefs { const char* node_name="A very long repeater name that must be truncated safely";
  float freq=910.525,bw=62.5; int sf=7,cr=5; bool powersaving_enabled=true;
};
struct Board { int getBattMilliVolts(){return 4100;} };
'''

SCENARIOS = r'''
int main() {
#ifdef UI_DEDICATED_PAIRING_BLOCK
  const std::vector<std::vector<int>> profiles={{160,160,8,16,320},{160,160,8,16,480}};
#else
  const std::vector<std::vector<int>> profiles={{128,64,8,16}, {160,80,8,16},
                    {250,122,21,27}, {250,122,22,29}, {200,200,22,29}, {240,135,10,20}};
#endif
  for(auto profile:profiles) {
    Display d(profile[0],profile[1],profile[2],profile[3],profile.size()>4?profile[4]:0); Task task;
    bool pairing_pin_visible=true;
    for(int state=0;state<5;++state) for(int count:{0,99,99999}) for(uint32_t pin:{123456U,12345U}) {
      task.enabled=state!=2; task.connected=state==1; task.prompt=state==4;
      task.count=count; the_mesh.pin=state==3?0:pin;
      d.clear(); home(d,&task,pairing_pin_visible);
      bool pin_expected=task.enabled && !task.connected && the_mesh.pin
          && (!task.prompt || d.height()>64);
      char pin_text[16]; snprintf(pin_text,sizeof(pin_text),"%06u",(unsigned)the_mesh.pin);
      assert((d.find(pin_text)!=nullptr)==pin_expected);
      assert(pairing_pin_visible==(d.find(pin_text)!=nullptr));
      if(pin_expected) {
        auto pin=d.find(pin_text);
        assert(pin->box.y>=d.height()/2);
#ifndef UI_DEDICATED_PAIRING_BLOCK
        assert(pin->box.bottom()==d.height()-(d.height()>64?2:0));
#endif
        assert(!d.find("Press: inbox"));
      }
      if(task.connected) assert(d.find("CONNECTED") || d.find("LINKED"));
    }
    // A render on another page must clear a PIN retained from the first page.
    pairing_pin_visible=true; d.clear(); home(d,&task,pairing_pin_visible,1);
    assert(!pairing_pin_visible);
    Prefs prefs; Board board; d.clear(); repeater(&d,&prefs,&board);
    assert(d.rows.size()==5);  // Never solve overlap by losing a status row.
    d.clear(); d.setTextSize(1);
    const auto hint=mesh::ui::makeButtonReaderHintLayout(d,d.textLineHeight(),d.height());
    const auto later=mesh::ui::makeButtonReaderHintLayout(d,d.textLineHeight(),d.height(),true);
    for(int i=0;i<hint.line_count;++i) assert(std::string(hint.lines[i])==later.lines[i]);
    mesh::ui::drawButtonReaderHint(d,hint);
    if(d.width()==128) {
      assert(hint.line_count==1);
      assert(d.find("4<<2<tap>1>>3 hold:X"));
    }
  }
  // Sweep font sizes and short viewports: retain complete digits, omit only
  // a label that cannot fit, and never draw outside the reserved lower band.
  for(int width:{32,80,160}) for(int h=16;h<=160;++h) for(int line=8;line<=32;++line) {
    Display d(width,h,line,2*line);
    const bool pin_visible=mesh::ui::drawBottomPairingBlock(d,h/3,"BLUETOOTH PIN","012345");
    assert(pin_visible==(d.find("012345")!=nullptr));
    for(const auto& row:d.rows) assert(row.box.y>=h/3);
  }
}
'''


class HomeTextSpacingTest(unittest.TestCase):
    def test_actual_home_and_repeater_branches(self):
        source = (ROOT / 'examples/companion_radio/ui-new/UITask.cpp').read_text()
        start = source.index('    _pairing_pin_visible = false;')
        metrics = source[start:source.index('    display.setColor', start)]
        start = source.index('    if (_page == HomePage::FIRST) {', start)
        home = source[start:source.index('#if UI_MESSAGES_HOME_PAGE == 1', start)]
        repeater = (ROOT / 'examples/simple_repeater/UITask.cpp').read_text()
        start = repeater.index('    // Reserve a full measured font-height')
        repeater = repeater[start:repeater.index('\n  }\n}', start)]
        implementation = ('void home(DisplayDriver& display, Task* _task, '
                          'bool& _pairing_pin_visible, int _page=0) {\n'
                          'char tmp[80];\n'
                          + metrics + 'display.drawTextEllipsized(0,2,display.width(),"Node name");\n'
                          + home + '\n}}\n'
                          'void repeater(DisplayDriver* _display, Prefs* _node_prefs, Board* _board) {'
                          'char tmp[80];\n' + repeater + '\n}\n')
        for wifi, dedicated in ((False, False), (True, False), (False, True), (True, True)):
            with self.subTest(wifi=wifi, dedicated=dedicated), tempfile.TemporaryDirectory() as temp:
                binary = str(Path(temp) / 'spacing')
                flags = ['-DWIFI_SSID="test"', '-DWITH_MQTT_BRIDGE'] if wifi else []
                if dedicated:
                    flags.append('-DUI_DEDICATED_PAIRING_BLOCK=1')
                result = subprocess.run(['c++', '-std=c++11', '-fsanitize=address,undefined',
                                         '-fno-pie', '-no-pie', '-I'+str(ROOT/'src'),
                                         *flags, '-x', 'c++', '-', '-o', binary],
                                        input=PREAMBLE+implementation+SCENARIOS,
                                        text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([binary], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
