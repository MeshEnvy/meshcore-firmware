"""Exercise color-display night controls through real config handlers and UI."""
import json
from pathlib import Path
import unittest

import test_webconfig_lost_reply as web_fixture

ROOT = Path(__file__).resolve().parents[1]


class DisplayNightWebConfigTest(unittest.TestCase):
    compile_run = web_fixture.LostReplyWebConfigTest.compile_run
    setup_values = web_fixture.LostReplyWebConfigTest.setup_values
    run_page = web_fixture.LostReplyWebConfigTest.run_page

    def test_actual_snapshot_and_authenticated_cli_batch(self):
        fixture = web_fixture.LostReplyWebConfigTest.fixture(self)
        fixture = fixture[:fixture.index('String writeBody(')]
        for color in (False, True):
            with self.subTest(color=color):
                source = r'''
#include <cstdint>
#define MESHCORE_HAS_REAL_DISPLAY 1
@COLOR@
namespace mesh { namespace ui {
struct Profile { uint8_t mode=0; uint16_t seconds=15; };
struct Prefs { Profile battery, usb; };
const Prefs& displayPowerPrefs() { static Prefs prefs; return prefs; }
const char* displayModeName(uint8_t) { return "button"; }
bool displayPairingSupported() { return false; }
uint8_t night_mode=2;
int16_t night_delay=60;
bool night_active=false;
uint8_t displayNightMode() { return night_mode; }
int16_t displayNightDelayMinutes() { return night_delay; }
bool displayNightActive() { return night_active; }
}}
'''.replace('@COLOR@', '#define MESHCORE_HAS_COLOR_DISPLAY 1' if color else '') + fixture + r'''
int main() {
  assert(mesh::esp32::managementPassword::publish("password"));
  for(unsigned mode=0;mode<3;++mode) {
    WebConfigServer server;
    mesh::ui::night_mode=mode;mesh::ui::night_delay=mode?0:-30;mesh::ui::night_active=mode==1;
    AsyncWebServerRequest get;server.handleConfigGet(&get);
    JsonDocument snapshot;
    assert(get.status==200 && deserializeJson(snapshot,get.output)==DeserializationError::Ok);
#ifdef MESHCORE_HAS_COLOR_DISPLAY
    const char* names[]={"off","on","auto"};
    assert(snapshot["display"]["night_supported"].as<bool>());
    assert(strcmp(snapshot["display"]["night"].as<const char*>(),names[mode])==0);
    assert(snapshot["display"]["night_delay"].as<int>()==(mode?0:-30));
    assert(snapshot["display"]["night_active"].as<bool>()==(mode==1));
#else
    assert(snapshot["display"]["night_supported"].isNull());
    assert(snapshot["display"]["night"].isNull());
#endif
  }
  for(const char* key:{"display.night","display.night.delay"}) {
    assert(wcIsAllowedSetKey(key) && !wcSetKeyRequiresReboot(key));
    WebConfigServer server;
    const char* value=strcmp(key,"display.night")==0?"auto":"-30";
    String body=String("{\"reqid\":\"0123456789abcdef\",\"set\":{\"")+key+"\":\""+value+"\"}}";
    AsyncWebServerRequest unauthenticated(body);unauthenticated.header.cookies.clear();
    server.handleConfigPost(&unauthenticated);
    assert(unauthenticated.status==401 && !server._batch_count);
    AsyncWebServerRequest save(body);server.handleConfigPost(&save);
    assert(save.status==202 && server._batch_count==1 && !server._batch_reboot);
    assert(String(server._batch[0].cmd)==String("set ")+key+" "+value);
  }
  for(const char* key:{"display.night.extra","display.night.delay.extra"})assert(!wcIsAllowedSetKey(key));
}
'''
                self.compile_run(source)

    def test_color_form_preserves_zero_and_saves_only_changed_night_fields(self):
        status, config = self.setup_values()
        status.update(mode="lan")
        config["display"] = {"mode": "button", "timeout": 15, "usb_mode": "on", "usb_timeout": 15,
                             "night_supported": True, "night": "auto", "night_delay": 0, "night_active": False}
        prelude = r'''<script>
(function(){
  var status=%s,config=%s,submitted=null;
  function reply(value,code){return Promise.resolve({ok:true,status:code||200,json:function(){return Promise.resolve(value)}})}
  window.fetch=function(path,options){
    if(path==="/api/status")return reply(status);
    if(path==="/api/config"&&(!options||options.method!=="POST"))return reply(config);
    if(path==="/api/config"){
      submitted=JSON.parse(options.body);config.display.night=submitted.set["display.night"];
      config.display.night_delay=Number(submitted.set["display.night.delay"]);
      return reply({state:"pending",reqid:submitted.reqid,count:2},202);
    }
    if(path.indexOf("/api/config/result?")===0)return reply({state:"done",reqid:submitted.reqid,results:[{key:"display.night",reply:"OK"},{key:"display.night.delay",reply:"OK"}]});
    return reply({});
  };
  function mark(key,value){document.body.setAttribute("data-test-night-"+key,String(value))}
  window.addEventListener("load",function(){
    setTimeout(function(){enterApp()},50);
    setTimeout(function(){
      var group=document.getElementById("display-night-settings"),mode=group.querySelector('select'),delay=group.querySelector('input');
      mark("visible",!group.classList.contains("hide"));mark("enabled",!mode.disabled&&!delay.disabled);
      mark("mode",mode.value);mark("delay",delay.value);mark("bounds",delay.min+","+delay.max);
      mark("choices",Array.from(mode.options).map(function(o){return o.value}).join(","));
      mode.value="on";mode.dispatchEvent(new Event("input",{bubbles:true}));
      delay.value="-30";delay.dispatchEvent(new Event("input",{bubbles:true}));appSave();
    },650);
    setTimeout(function(){
      mark("keys",submitted?Object.keys(submitted.set).sort().join(","):"missing");
      mark("saved",submitted?submitted.set["display.night"]+","+submitted.set["display.night.delay"]:"missing");
      mark("no-reboot",submitted&&!submitted.reboot);mark("clean",!st.dirty["display.night"]&&!st.dirty["display.night.delay"]);
    },1800);
  });
})();
</script>''' % (json.dumps(status), json.dumps(config))
        dom = self.run_page(prelude, virtual_time=2400)
        for key, value in (("visible", "true"), ("enabled", "true"), ("mode", "auto"), ("delay", "0"),
                           ("bounds", "-180,180"), ("choices", "auto,on,off"),
                           ("keys", "display.night,display.night.delay"), ("saved", "on,-30"),
                           ("no-reboot", "true"), ("clean", "true")):
            self.assertIn('data-test-night-%s="%s"' % (key, value), dom)

    def test_monochrome_and_headless_forms_hide_and_disable_night_controls(self):
        for display in (None, {"mode": "button", "timeout": 15, "usb_mode": "on", "usb_timeout": 15}):
            with self.subTest(display=display):
                status, config = self.setup_values()
                status.update(mode="lan")
                if display is None:
                    config.pop("display", None)
                else:
                    config["display"] = display
                prelude = r'''<script>
window.fetch=function(path){var value=path==="/api/status"?%s:%s;return Promise.resolve({ok:true,status:200,json:function(){return Promise.resolve(value)}})};
window.addEventListener("load",function(){
  setTimeout(function(){enterApp()},50);
  setTimeout(function(){
    var group=document.getElementById("display-night-settings"),body=document.body;
    body.setAttribute("data-test-night-hidden",group.classList.contains("hide"));
    body.setAttribute("data-test-night-disabled",Array.from(group.querySelectorAll('[data-k]')).every(function(e){return e.disabled}));
    body.setAttribute("data-test-night-dirty",Object.keys(st.dirty).length);
  },650);
});
</script>''' % (json.dumps(status), json.dumps(config))
                dom = self.run_page(prelude)
                for key, value in (("hidden", "true"), ("disabled", "true"), ("dirty", "0")):
                    self.assertIn('data-test-night-%s="%s"' % (key, value), dom)


if __name__ == "__main__":
    unittest.main()
