#!/usr/bin/env python3
"""Execute Cardputer text-entry, rendering and radio validation without hardware."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
VARIANT = ROOT / "variants/m5stack_cardputer_adv"
UI = VARIANT / "companion_radio/ui-cardputer"


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class CardputerUiTests(unittest.TestCase):
    def test_actual_settings_entry_render_validation_and_headless_shutdown(self):
        settings = (UI / "screen/SettingsScreen.cpp").read_text(encoding="ascii")
        header = (UI / "screen/SettingsScreen.h").read_text(encoding="ascii")
        enum = re.search(r"enum SettingsItem \{.*?\};", header, re.S).group()
        limit = re.search(r"static constexpr size_t MAX_NUMERIC_EDIT_BYTES = \d+;", header).group()
        cli = (ROOT / "src/helpers/CLICommandUtils.h").read_text()
        radio = (ROOT / "src/helpers/radiolib/RadioLibWrappers.cpp").read_text()
        task = (UI / "CardputerUITask.cpp").read_text(encoding="ascii")
        channels = (UI / "screen/main/ChannelsPage.cpp").read_text(encoding="ascii")
        code = r'''
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include "RadioProfiles.h"
using std::isfinite;
#define LORA_TX_POWER 22
#define MAX_GROUP_CHANNELS 40
class String {
  std::string value;
public:
  String() = default;
  String(const char* s): value(s) {}
  String& operator=(const char* s) { value=s; return *this; }
  unsigned length() const { return value.size(); }
  const char* c_str() const { return value.c_str(); }
  long toInt() const { return std::strtol(value.c_str(),nullptr,10); }
  int indexOf(char c) const { auto p=value.find(c); return p==std::string::npos ? -1 : int(p); }
  void concat(char c) { value+=c; }
  void concat(const char* s) { value+=s; }
  char charAt(unsigned i) const { return i<value.size() ? value[i] : 0; }
  void remove(unsigned i,unsigned n) { value.erase(i,n); }
};
#include "helpers.h"
namespace Keyboard {
enum Key { KEY_NONE, KEY_BACKSPACE, KEY_PERIOD, ARROW_LEFT, ARROW_RIGHT, KEY_1,
           ARROW_UP, ARROW_DOWN, KEY_RETURN };
struct Event { Key key=KEY_NONE; const char* text=""; };
}
struct Layout {
  const char* getCurrentCode() const { return "en"; }
  const char* lookupDefault(Keyboard::Event& e) { return e.text; }
  const char* lookup(Keyboard::Event& e) { return e.text; }
};
struct Board {
  Layout layout;
  unsigned off=0,boots=0;
  Layout* getLayout() { return &layout; }
  uint16_t getBattMilliVolts() const { return 4000; }
  void powerOff() { ++off; }
  void reboot() { ++boots; }
};
struct Task {
  Board board;
  Board* getBoard() { return &board; }
  bool getGPSState() const { return true; }
  bool isBluetoothEnabled() const { return true; }
  bool powerSaveEnabled() const { return true; }
  void showAlert(const char*,int) {}
};
struct Prefs {
  char node_name[32]="node",default_scope_name[31]="";
  uint8_t default_scope_key[32]={};
  float freq=910.525f,bw=62.5f;
  uint8_t sf=8,cr=5,path_hash_mode=1,buzzer_quiet=0;
  int8_t tx_power_dbm=22;
};
struct CustomPrefs { float battery_correction=1; };
struct ChannelDetails { char name[32]={}; };
struct Mesh {
  unsigned saves=0,applies=0;
  Prefs* prefs=nullptr;
  CustomPrefs* custom=nullptr;
  CustomPrefs committed;
  bool fail=false;
  std::string command;
  ChannelDetails channels[MAX_GROUP_CHANNELS];
  bool savePrefs() { if(fail) return false; ++saves; return true; }
  bool saveCustomPrefs() {
    if(fail) { *custom=committed; return false; }
    committed=*custom; ++saves; return true;
  }
  uint16_t getPrimaryRadioPreamble() const { return 96; }
  bool handleLocalCommand(const char* text,char* reply) {
    command=text;
    if(fail) { strcpy(reply,"Error: save failed"); return true; }
    if(command.rfind("set radio ",0)==0) {
      float f,bw; unsigned sf,cr,pre;
      assert(sscanf(text+10,"%f,%f,%u,%u,%u",&f,&bw,&sf,&cr,&pre)==5 && pre==96);
      prefs->freq=f; prefs->bw=bw; prefs->sf=sf; prefs->cr=cr; ++applies;
    } else if(command.rfind("set tx ",0)==0) {
      prefs->tx_power_dbm=std::stoi(command.substr(7)); ++applies;
    } else if(command.rfind("set name ",0)==0) {
      snprintf(prefs->node_name,sizeof(prefs->node_name),"%s",text+9);
    } else { assert(false); }
    ++saves; strcpy(reply,"OK"); return true;
  }
  bool getChannel(int i,ChannelDetails& result) {
    if(i<0 || i>=MAX_GROUP_CHANNELS) return false;
    result=channels[i]; return true;
  }
} the_mesh_cp;
namespace mesh { namespace Utils {
void sha256(uint8_t* dest,size_t n,const uint8_t*,size_t,const uint8_t*,size_t) { memset(dest,0,n); }
} namespace cli {
'''
        code += function(cli, "inline bool parseDecimalStrict(") + "\n}}\n"
        code += r'''
struct RadioLibWrapper {
  unsigned off=0;
  bool supportsMeshtext() const { return false; }
  bool validateProfile(const mesh::RadioProfileParams&) const;
  bool isTxPowerValid(int8_t p) const { return p>=-9 && p<=22; }
  void powerOff() { ++off; }
} radio_driver;
'''
        code += function(radio, "bool RadioLibWrapper::validateProfile(") + "\n"
        code += r'''
struct CardputerDisplay {
  enum PaletteColor { P_GREEN,P_YELLOW,P_RED,P_BLACK,P_WHITE };
  std::string last;
  int getRowHeight() const { return 16; }
  int height() const { return 135; }
  int width() const { return 240; }
  int getTextWidth(const char* s) { return strlen(s)*8; }
  int getTextLevel() const { return 1; }
  static const char* textLevelName(int) { return "Large"; }
  void setColor(PaletteColor) {}
  void setTextSize(int) {}
  void drawTextCentered(int,int,const char*) {}
  void drawTextLeftAlign(int,int,const char*) {}
  void drawTextEllipsized(int,int,int,const char*) {}
  void drawTextLeftAlignWithScroll(int,int,int,const char* s) { last=s; }
  void drawRect(int,int,int,int) {}
  void fillRect(int,int,int,int) {}
  void turnOff() {}
};
struct SettingsScreen {
'''
        code += enum + "\n" + limit + r'''
  Prefs* _node_prefs;
  CustomPrefs* _custom_prefs;
  Task* _task;
  String edit_buffer;
  uint8_t edit_u8=0;
  int menu_index=RadioFreq;
  char edit_error[80]="Invalid value";
  bool is_editing=true;
  void renderItem(CardputerDisplay&,SettingsItem,int,int);
  bool commitItemEdit(SettingsItem);
  void handleEditInput(SettingsItem,Keyboard::Event&);
  bool inputParsePositiveFloat(float&);
  bool validRadioParams(float,float,uint8_t,uint8_t) const;
  bool commitCommand(const char*);
  bool commitRadio(float,float,uint8_t,uint8_t);
  int render(CardputerDisplay&);
};
'''
        for signature in (
            "void SettingsScreen::renderItem(",
            "bool SettingsScreen::commitCommand(",
            "bool SettingsScreen::commitRadio(",
            "bool SettingsScreen::commitItemEdit(",
            "void SettingsScreen::handleEditInput(",
            "bool SettingsScreen::inputParsePositiveFloat(",
            "bool SettingsScreen::validRadioParams(",
            "int SettingsScreen::render(",
        ):
            code += function(settings, signature) + "\n"
        code += r'''
struct CardputerUITask {
  CardputerDisplay* _display=nullptr;
  Board* _board;
  bool allow_shutdown=true;
  bool prepareForShutdown() { return allow_shutdown; }
  void showAlert(const char*,int) {}
  void shutdown(bool restart=false);
};
'''
        code += function(task, "void CardputerUITask::shutdown(") + "\n"
        code += r'''
struct MainScreen {
  int selected=-1;
  void selectChannel(const ChannelDetails&,int index) { selected=index; }
};
struct ChannelsPage {
  MainScreen* _p;
  int channel_list_idx=0;
  int getChannelCount();
  bool getChannelIndex(int,int&);
  bool handleInput(Keyboard::Event&);
};
'''
        for signature in (
            "int ChannelsPage::getChannelCount(",
            "bool ChannelsPage::getChannelIndex(",
            "bool ChannelsPage::handleInput(",
        ):
            code += function(channels, signature) + "\n"
        code += r'''
int main() {
  Prefs prefs; CustomPrefs custom; Task task; CardputerDisplay display;
  SettingsScreen s{&prefs,&custom,&task};
  the_mesh_cp.prefs=&prefs; the_mesh_cp.custom=&custom;
  Keyboard::Event digit{Keyboard::KEY_1,"9"};
  for(int i=0;i<4096;++i) s.handleEditInput(SettingsScreen::RadioFreq,digit);
  assert(s.edit_buffer.length()==s.MAX_NUMERIC_EDIT_BYTES);
  s.render(display); assert(display.last.size()<=63);
  // Render must also remain safe when an external/current value exceeds the input bound.
  std::string long_value(4096,'9'); s.edit_buffer=long_value.c_str();
  s.render(display); assert(display.last.size()==63);
  for(const char* bad:{"",".","0","-1","nan","inf","1e3","910x","42949672960"}) {
    s.edit_buffer=bad; float value=123;
    assert(!s.inputParsePositiveFloat(value) && value==123);
  }
  for(const char* bad:{"0","149.9","960.1","42949672960"}) {
    s.edit_buffer=bad;
    assert(!s.commitItemEdit(SettingsScreen::RadioFreq));
    assert(prefs.freq==910.525f && the_mesh_cp.saves==0);
  }
  s.edit_buffer="915.25";
  assert(s.commitItemEdit(SettingsScreen::RadioFreq));
  assert(prefs.freq==915.25f && the_mesh_cp.saves==1 && the_mesh_cp.applies==1);
  for(const char* bad:{"0","123.45","812.5"}) {
    s.edit_buffer=bad;
    assert(!s.commitItemEdit(SettingsScreen::RadioBw));
    assert(prefs.bw==62.5f && the_mesh_cp.saves==1);
  }
  s.edit_buffer="125"; assert(s.commitItemEdit(SettingsScreen::RadioBw));
  assert(prefs.bw==125 && the_mesh_cp.saves==2);
  s.edit_u8=4; assert(!s.commitItemEdit(SettingsScreen::RadioSf));
  assert(!s.commitItemEdit(SettingsScreen::RadioCr));
  s.edit_u8=23; assert(!s.commitItemEdit(SettingsScreen::RadioPwr));
  assert(prefs.sf==8 && prefs.cr==5 && prefs.tx_power_dbm==22 && the_mesh_cp.saves==2);
  // The UI must use the live shared command path and leave prefs untouched on failure.
  assert(the_mesh_cp.command=="set radio 915.250000,125.000000,8,5,96");
  the_mesh_cp.fail=true;
  s.edit_buffer="916"; assert(!s.commitItemEdit(SettingsScreen::RadioFreq));
  assert(prefs.freq==915.25f && strstr(s.edit_error,"save failed"));
  s.edit_buffer="renamed"; assert(!s.commitItemEdit(SettingsScreen::PublicInfoName));
  assert(!strcmp(prefs.node_name,"node"));
  strcpy(prefs.default_scope_name,"original"); memset(prefs.default_scope_key,0xA5,sizeof(prefs.default_scope_key));
  s.edit_buffer="replacement"; assert(!s.commitItemEdit(SettingsScreen::MeshDefaultScope));
  assert(!strcmp(prefs.default_scope_name,"original") && prefs.default_scope_key[0]==0xA5);
  s.edit_u8=3; assert(!s.commitItemEdit(SettingsScreen::MeshPathSize));
  assert(prefs.path_hash_mode==1);
  s.edit_buffer="4400"; assert(!s.commitItemEdit(SettingsScreen::DeviceBatteryCorrection));
  assert(custom.battery_correction==1.0f);
  the_mesh_cp.fail=false;
  s.edit_buffer="node renamed"; assert(s.commitItemEdit(SettingsScreen::PublicInfoName));
  assert(!strcmp(prefs.node_name,"node renamed"));
  for(const char* bad:{"0","1","9999"}) {
    s.edit_buffer=bad; assert(!s.commitItemEdit(SettingsScreen::DeviceBatteryCorrection));
    assert(custom.battery_correction==1.0f);
  }
  s.edit_buffer="4400"; assert(s.commitItemEdit(SettingsScreen::DeviceBatteryCorrection));
  assert(custom.battery_correction==1.1f);
  // A missing/failed display cannot turn a battery shutdown into a null dereference.
  CardputerUITask ui{nullptr,&task.board};
  ui.shutdown(); assert(task.board.off==1 && radio_driver.off==1);
  ui.shutdown(true); assert(task.board.boots==1);
  ui.allow_shutdown=false; ui.shutdown(); ui.shutdown(true);
  assert(task.board.off==1 && task.board.boots==1 && radio_driver.off==1);
  // Channel list indices must map back to actual sparse channel slots.
  MainScreen main_screen; ChannelsPage list{&main_screen};
  for(int i:{2,7,39}) strcpy(the_mesh_cp.channels[i].name,"channel");
  assert(list.getChannelCount()==3);
  int real=-1;
  assert(list.getChannelIndex(0,real) && real==2);
  assert(list.getChannelIndex(1,real) && real==7);
  assert(list.getChannelIndex(2,real) && real==39);
  assert(!list.getChannelIndex(3,real));
  Keyboard::Event enter{Keyboard::KEY_RETURN,""};
  list.handleInput(enter); assert(main_screen.selected==2);
  Keyboard::Event up{Keyboard::ARROW_UP,""},down{Keyboard::ARROW_DOWN,""};
  list.handleInput(up); assert(list.channel_list_idx==2);
  list.handleInput(enter); assert(main_screen.selected==39);
  list.handleInput(down); assert(list.channel_list_idx==0);
  for(auto& channel:the_mesh_cp.channels) channel=ChannelDetails{};
  assert(list.getChannelCount()==0);
  list.handleInput(enter); assert(main_screen.selected==39);
  list.handleInput(up); assert(list.channel_list_idx==0);
  std::puts("PASS: actual Cardputer entry, render, validation and headless shutdown");
}
'''
        with tempfile.TemporaryDirectory(prefix="cardputer-ui-") as tmp:
            source = Path(tmp) / "test.cpp"
            binary = Path(tmp) / "test"
            source.write_text(code, encoding="ascii")
            compiled = subprocess.run([
                "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-Wno-missing-field-initializers",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                "-DMESH_RADIO_PROFILE_CHIP=1262", "-DMESH_ENABLE_HOST_KISS=0",
                "-DMESH_ENABLE_ETHERNET_WIRE=0", "-I", str(ROOT / "src"),
                "-I", str(VARIANT), str(source), "-o", str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            executed = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_bounded_formatting_and_font_attribution(self):
        for source in UI.rglob("*.cpp"):
            self.assertNotRegex(source.read_text(encoding="ascii"), r"\bsprintf\s*\(", str(source))
        notice = (VARIANT / "LICENSE.DejaVu").read_text(encoding="ascii")
        self.assertIn("Copyright (c) 2003 by Bitstream", notice)
        self.assertIn("Copyright (c) 2006 by Tavmjong Bah", notice)
        self.assertIn("included in all copies of one or more", notice)
        for font in ("font.h", "font_16.h", "font_19.h"):
            self.assertIn("LICENSE.DejaVu", (UI / font).read_text(encoding="ascii"))


if __name__ == "__main__":
    unittest.main()
