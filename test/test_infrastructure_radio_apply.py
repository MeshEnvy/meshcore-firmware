"""Run permanent CLI setters, delay and room/sensor apply paths from production."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_repeater_radio_timing_integration import method

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <math.h>
#include <initializer_list>
#include <helpers/CLICommandUtils.h>
#include <helpers/RadioProfileCommandUtils.h>
#define MAX_LORA_TX_POWER 22
#include <helpers/radiolib/RadioPowerLimits.h>
#define MESH_DEBUG_PRINTLN(...) ((void)0)
static uint32_t now_ms=100;
uint32_t millis(){return now_ms;}
bool isValidLoRaBandwidth(float bw){return bw==62.5f || bw==125;}
struct Prefs {
  float freq=910.51f,bw=62.5f;
  uint8_t sf=7,cr=5,rx_ps_level=0;
  uint16_t primary_radio_preamble=32,rx_ps_preamble=0;
  uint32_t rx_ps_rx_us=100,rx_ps_sleep_us=200;
  int8_t tx_power_dbm=30;
  bool rx_powersaving_enabled=false;
};
struct Callbacks {virtual bool scheduleNormalRadio()=0;};
namespace mesh {
enum class RadioParamApplyResult{APPLIED,BUSY,FAILED};
struct RadioProfileCLI {
  static bool parseSuffix(const char*,unsigned,char*,size_t,uint16_t&);
  bool acceptsPrimary(float,float,uint8_t,uint8_t,uint16_t){return true;}
  void adoptPrimaryPreamble(uint16_t){}
  void appendPrimaryChirpWarning(char*,size_t,uint8_t,float,uint16_t){}
  RadioParamApplyResult applyPrimary(float,float,uint8_t,uint8_t,bool,uint16_t,const uint32_t*) {
    return RadioParamApplyResult::APPLIED;
  }
};
@SUFFIX@
}
struct CommonCLI {
  Prefs* _prefs;
  Callbacks* _callbacks;
  mesh::RadioProfileCLI _radio_profiles;
  uint32_t _primary_radio_apply_at=0;
  bool save_ok=true;
  unsigned saves=0;
  bool saveCommonPrefs(){++saves;return save_ok;}
  bool savePrimaryRadioParams(float,float,uint8_t,uint8_t,uint16_t);
  bool acceptsPrimaryRadioParams(float,float,uint8_t,uint8_t,uint16_t);
  bool applyAndSavePrimaryRadioParams(float,float,uint8_t,uint8_t,uint16_t,char*);
  bool primaryRadioApplyReady(bool reply_idle=true);
  void command(const char*,char*);
  mesh::RadioProfileCLI& radioProfiles(){return _radio_profiles;}
  static void recalculateRxPowerSavingFromLevel(Prefs* p){p->rx_ps_rx_us=300;p->rx_ps_sleep_us=400;}
  static bool calculateRxPowerSavingLevel(uint8_t,uint8_t,float,uint32_t,uint32_t*,uint32_t*){return true;}
};
void appendRxPowerSavingAdjustmentNote(char*,Prefs*,uint8_t,float){}
uint32_t rxPowerSavingPreambleForParams(uint8_t,float){return 16;}
uint32_t nextRadioApplyRetryDelay(uint8_t& failures){++failures;return 1000;}
struct {bool supportsRxPowerSaving(){return false;}} radio_driver;
struct Role:Callbacks {
  Prefs _prefs;
  CommonCLI _cli{&_prefs,this};
  uint32_t set_radio_at=0,revert_radio_at=0,radio_apply_retry_at=0;
  uint8_t radio_apply_failures=0,active_cr=5,pending_sf=7,pending_cr=5;
  uint16_t pending_preamble=32;
  float pending_freq=911,pending_bw=125,active_freq=910.51f;
  bool temp_radio_applied=false,saved_radio_apply_pending=false,outbound=false,apply_ok=true;
  struct Manager {unsigned queued=0;unsigned getOutboundTotal(){return queued;}} manager;
  Manager* _mgr=&manager;
  unsigned apply_attempts=0;
  uint32_t futureMillis(uint32_t delay){return now_ms+delay;}
  bool millisHasNowPassed(uint32_t at){return int32_t(now_ms-at)>=0;}
  bool hasOutbound(){return outbound;}
  bool applySavedRadioParams(){++apply_attempts;if(!apply_ok)return false;active_freq=_prefs.freq;return true;}
  bool scheduleNormalRadio() override;
  void servicePendingPrimaryRadioSettings(bool tx_gap=false);
};
@METHODS@
void CommonCLI::command(const char* config,char* reply) {
  @RADIO@
  else @FREQ@
}
int main(){
  for(const char* command:{"freq 915.25","radio 915.25,125,9,6,48"}) {
    for(bool temporary:{false,true}) {
      now_ms=100;
      Role node;node.temp_radio_applied=temporary;
      char reply[160]{};
      node._cli.save_ok=false;
      node._cli.command(command,reply);
      assert(strstr(reply,"Error") && node._prefs.freq==910.51f);
      assert(node._prefs.tx_power_dbm==30 && node._prefs.rx_ps_rx_us==100);
      assert(!node._cli._primary_radio_apply_at && !node.saved_radio_apply_pending);
      node._cli.save_ok=true;
      node._cli.command(command,reply);
      assert(strstr(reply,"applying in 2s") && strstr(reply,"TX power limited to 22"));
      assert(node._prefs.freq==915.25f && node.active_freq==910.51f);
      assert(node.saved_radio_apply_pending);
      if(command[0]=='f')assert(node._prefs.bw==62.5f && node._prefs.primary_radio_preamble==32);
      else assert(node._prefs.bw==125 && node._prefs.sf==9 && node._prefs.cr==6 && node._prefs.primary_radio_preamble==48);
      for(now_ms=101;now_ms<2100;now_ms+=333) {
        node.servicePendingPrimaryRadioSettings(true);
        assert(node.apply_attempts==0 && node.active_freq==910.51f);
      }
      now_ms=2100;node.outbound=true;
      node.servicePendingPrimaryRadioSettings();
      assert(node.apply_attempts==0); // Keep the current reply on the old tuple.
      node.outbound=false;node.manager.queued=1;
      node.servicePendingPrimaryRadioSettings(true);
      assert(node.apply_attempts==0); // A different packet finishing cannot lose our queued reply.
      node.manager.queued=0;node.apply_ok=false;
      node.servicePendingPrimaryRadioSettings();
      assert(node.apply_attempts==1 && node.saved_radio_apply_pending);
      node.apply_ok=true;now_ms=3100;
      node.servicePendingPrimaryRadioSettings();
      assert(node.active_freq==915.25f && !node.saved_radio_apply_pending && !node.temp_radio_applied);
    }
  }
  // A second accepted edit restarts the two-second handoff; malformed edits
  // neither save nor replace an already accepted deadline.
  Role node; char reply[160];now_ms=100;
  node._cli.command("freq 915",reply);
  now_ms=1500;node._cli.command("freq 916",reply);
  assert(node._cli._primary_radio_apply_at==3500);
  unsigned saves=node._cli.saves;
  for(const char* invalid:{"freq nan","freq 149","radio 915,125,13,5","radio 915,125,7,5,7"}) {
    node._cli.command(invalid,reply);
    assert(strstr(reply,"Error") && node._cli.saves==saves && node._cli._primary_radio_apply_at==3500);
  }
  now_ms=3499;node.servicePendingPrimaryRadioSettings(true);assert(node.apply_attempts==0);
  now_ms=3500;node.servicePendingPrimaryRadioSettings();assert(node.active_freq==916);
}
'''


class InfrastructureRadioApplyTest(unittest.TestCase):
    def test_room_and_sensor_permanent_cli_handoff(self):
        common = (ROOT / "src/helpers/CommonCLI.cpp").read_text()
        helpers = "\n".join(method(common, signature) for signature in (
            "bool CommonCLI::savePrimaryRadioParams(",
            "bool CommonCLI::acceptsPrimaryRadioParams(",
            "bool CommonCLI::applyAndSavePrimaryRadioParams(",
            "bool CommonCLI::primaryRadioApplyReady("))
        suffix = method((ROOT / "src/helpers/RadioProfileCLI.cpp").read_text(),
                        "bool RadioProfileCLI::parseSuffix(")
        def setter(key):
            anchor = common.index(f'memcmp(config, "{key} ",')
            start = common.rfind('} else if (', 0, anchor) + len('} else ')
            return method(common[start:], 'if (').replace('#ifdef WITH_BRIDGE\n', '')
        for file, role in (("simple_room_server/MyMesh.cpp", "MyMesh"),
                           ("simple_sensor/SensorMesh.cpp", "SensorMesh")):
            with self.subTest(role=role), tempfile.TemporaryDirectory() as temp:
                source = (ROOT / "examples" / file).read_text()
                methods = helpers + "\n" + "\n".join(
                    method(source, signature).replace(role + "::", "Role::")
                    for signature in (f"bool {role}::scheduleNormalRadio()",
                                      f"void {role}::servicePendingPrimaryRadioSettings("))
                code = HARNESS.replace('@METHODS@', methods).replace('@SUFFIX@', suffix)
                code = code.replace('@RADIO@', setter('radio')).replace('@FREQ@', setter('freq'))
                cpp = Path(temp) / "test.cpp"
                binary = Path(temp) / "test"
                cpp.write_text(code)
                subprocess.run(['c++', '-std=c++17', '-I'+str(ROOT/'src'),
                                str(cpp), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)

    def test_temp_query_distinguishes_pending_active_and_off(self):
        common = (ROOT / 'src/helpers/CommonCLI.cpp').read_text()
        query = method(common, 'if (!strcmp(command, "get tempradio") ||')
        setting = method((ROOT / 'examples/simple_repeater/MyMesh.h').read_text(),
                         'struct ScheduledRadioSetting {') + ';'
        for folder, cls, filename in (
                ('simple_repeater', 'MyMesh', 'MyMesh'),
                ('simple_room_server', 'MyMesh', 'MyMesh'),
                ('simple_sensor', 'SensorMesh', 'SensorMesh')):
            with self.subTest(role=folder), tempfile.TemporaryDirectory() as temp:
                if folder == 'simple_repeater':
                    source = (ROOT / 'examples' / folder / (filename + '.cpp')).read_text()
                    pending = method(source, f'bool {cls}::isTempRadioPending() const')
                    pending = pending.replace(cls + '::', '')
                    fields = setting + '\nScheduledRadioSetting scheduled_radio_settings[8]{};'
                    set_pending = ('void pending(bool value) {scheduled_radio_settings[0].active=value;'
                                   'scheduled_radio_settings[0].temporary=value;'
                                   'scheduled_radio_settings[0].started=false;}')
                else:
                    source = (ROOT / 'examples' / folder / (filename + '.h')).read_text()
                    pending = method(source, 'bool isTempRadioPending() const override').replace(' override', '')
                    fields = 'uint32_t set_radio_at=0;'
                    set_pending = 'void pending(bool value) {set_radio_at=value ? 2000 : 0;}'
                code = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <RadioProfiles.h>
struct Radio {
  mesh::RadioProfiles state;
  const mesh::RadioProfiles* profiles() const {return &state;}
  uint16_t profilePreamble(uint8_t) const {return 32;}
};
struct Callback {
  Radio radio;
  bool temp_radio_applied=false;
  @FIELDS@
  @PENDING@
  @SET_PENDING@
  const Radio* getProfileRadio() const {return &radio;}
};
struct CommonCLI {
  Callback* _callbacks;
  void get(char* reply, const char* command) { @QUERY@ }
};
void get(Callback& callbacks, char* reply, const char* command="get tempradio") {
  CommonCLI cli{&callbacks};cli.get(reply,command);
}
int main() {
  Callback callbacks;char reply[160];
  get(callbacks,reply);assert(!strcmp(reply,"> off"));
  callbacks.pending(true);
  get(callbacks,reply);assert(!strcmp(reply,"> pending"));
  get(callbacks,reply,"tempradio");assert(!strcmp(reply,"> pending"));
  callbacks.pending(false);callbacks.radio.state.primary_temporary=true;callbacks.temp_radio_applied=true;
  auto& p=callbacks.radio.state.primary;
  p.freq=910.539978f;p.bw=62.5f;p.sf=7;p.cr=5;
  get(callbacks,reply);assert(!strcmp(reply,"> 910.540,62.500,7,5,32"));
  // A replacement or future schedule must not hide the currently live tuple.
  callbacks.pending(true);get(callbacks,reply);assert(!strcmp(reply,"> 910.540,62.500,7,5,32"));
  callbacks.pending(false);callbacks.radio.state.primary_temporary=false;callbacks.temp_radio_applied=false;
  get(callbacks,reply);assert(!strcmp(reply,"> off"));
}
'''
                code = code.replace('@FIELDS@', fields).replace('@PENDING@', pending)
                code = code.replace('@SET_PENDING@', set_pending).replace('@QUERY@', query)
                cpp = Path(temp) / 'query.cpp'
                binary = Path(temp) / 'query'
                cpp.write_text(code)
                subprocess.run(['c++', '-std=c++17', '-I'+str(ROOT/'src'), str(cpp),
                                '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)

    def test_screens_show_applied_tuple_with_radio2_off_or_on(self):
        # Compile the actual render/signature/dashboard selection blocks. A
        # saved edit must not move the screen before the PHY accepts it.
        selections = []
        for role in ('simple_repeater', 'simple_room_server', 'simple_sensor'):
            source = (ROOT / 'examples' / role / 'UITask.cpp').read_text()
            offset = 0
            while True:
                start = source.find('float freq = _node_prefs->freq;', offset)
                if start < 0:
                    break
                condition = source.index('if (', start)
                block = method(source[condition:], 'if (')
                end = condition + len(block)
                selections.append(source[start:end] + '\nreturn {freq,bw,sf,cr};')
                offset = end
            if 'bool UITask::buildDashboardContext(' in source:
                start = source.index('ctx->freq = _node_prefs->freq;',
                                     source.index('bool UITask::buildDashboardContext('))
                condition = source.index('if (profiles', start)
                end = condition + len(method(source[condition:], 'if ('))
                selections.append('View context; View* ctx=&context;\n' + source[start:end]
                                  + '\nreturn context;')
        self.assertEqual(len(selections), 7)
        code = r'''
#include <cassert>
#include <cstddef>
#include <RadioProfiles.h>
#include <helpers/ui/RadioProfileDisplayPage.h>
#include <helpers/ui/DisplayFrameSignature.h>
struct View{float freq,bw;uint8_t sf,cr;const char* radio_label="";};
static View saved{920,125,9,6};
static View* _node_prefs=&saved;
static mesh::RadioProfiles state;
static const mesh::RadioProfiles* configuredRadioProfiles(){return &state;}
static bool secondary_page=false;
static bool showingSecondaryRadioProfilePage(){return secondary_page;}
'''
        for i, selection in enumerate(selections):
            # Dashboard declares its own profiles pointer.
            setup = '' if 'const mesh::RadioProfiles* profiles' in selection else (
                'const auto* profiles=configuredRadioProfiles();')
            code += f'View render{i}(){{' + setup + (
                'const bool dual_radio=state.enabled();'
                'uint32_t signature=DisplayFrameSignature::INITIAL;') + selection + '}\n'
        code += 'int main(){\n'
        for i in range(len(selections)):
            code += f'''
  state.primary.freq=910.51f;state.primary.bw=62.5f;state.primary.sf=7;state.primary.cr=5;
  state.secondary.params.freq=911;state.secondary.params.bw=125;state.secondary.params.sf=8;state.secondary.params.cr=6;
  state.secondary.mode=mesh::RadioProfileMode::Off;secondary_page=true;
  auto v{i}=render{i}();assert(v{i}.freq==910.51f && v{i}.bw==62.5f && v{i}.sf==7);
  state.primary.freq=920;state.primary.bw=125;state.primary.sf=9;
  v{i}=render{i}();assert(v{i}.freq==920 && v{i}.bw==125 && v{i}.sf==9);
  state.primary_temporary=true;state.primary.freq=912;
  v{i}=render{i}();assert(v{i}.freq==912);
  state.secondary.mode=mesh::RadioProfileMode::Rx;
  v{i}=render{i}();assert(v{i}.freq==911 && v{i}.sf==8);
  secondary_page=false;v{i}=render{i}();assert(v{i}.freq==912);
'''
        code += '}\n'
        with tempfile.TemporaryDirectory() as temp:
            cpp = Path(temp) / 'display.cpp'
            binary = Path(temp) / 'display'
            cpp.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-I'+str(ROOT/'src'), str(cpp),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
