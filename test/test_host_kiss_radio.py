"""Exercise raw radio2 host KISS isolation and concrete modem protocol selection."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

from cpp_source import body
from test_meshtext_transport import HARNESS as FOREIGN_HARNESS
import test_meshtext_config as feature_config
import test_ethernet_radio as ethernet_test

ROOT = Path(__file__).resolve().parents[1]


class HostKissRadioTest(unittest.TestCase):
    def run_cpp(self, source, *, dispatcher=False):
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix="meshcore-host-kiss-") as folder:
            cpp, exe = Path(folder) / "test.cpp", Path(folder) / "test"
            cpp.write_text(source, encoding="ascii")
            args = [compiler, "-std=c++17", "-Wall", "-Wextra",
                    "-DMESH_ENABLE_HOST_KISS=1", "-DMESH_ENABLE_MESHTEXT_CHAT=0",
                    "-DMESH_ENABLE_MESHTEXT_WIRE=0", "-DMESH_ENABLE_ETHERNET_WIRE=0",
                    "-DMESH_ENABLE_TX_BEACONS=0", "-I", str(ROOT / "test/mocks"),
                    "-I", str(ROOT / "src")]
            if os.name != "nt":
                args += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
            if dispatcher:
                args += [str(ROOT / "src" / name) for name in (
                    "Dispatcher.cpp", "Packet.cpp", "helpers/StaticPoolPacketManager.cpp")]
            result = subprocess.run(args + [str(cpp), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr[-16000:])
            env = dict(os.environ)
            env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
            result = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_actual_dispatcher_host_only_routing_and_scheduler_admission(self):
        fixture = FOREIGN_HARNESS.split("int main()", 1)[0]
        fixture = fixture.replace("RadioProfileProtocol::Meshtext", "RadioProfileProtocol::Kiss")
        fixture = fixture.replace("bool supportsMeshtext() const override", "bool supportsHostKiss() const override")
        fixture = fixture.replace("return queueForeignRadioFrame(b,n,1,_radio->profiles()->generation[1]);",
                                  "return submitHostKissFrame(b,n,_radio->profiles()->generation[1]);")
        hooks = r'''
namespace mesh { namespace hostkiss {
unsigned rx=0; std::vector<bool> completions;
void receive(const uint8_t* b,size_t n,uint8_t p,uint32_t g,float,float) {
  assert(n==3 && b[0]==0x11 && p==1 && g); ++rx;
}
void sendComplete(bool success) { completions.push_back(success); }
} }
int main() {
  using namespace mesh::hostkiss;
  { Fixture f;const auto done=completions.size();assert(f.d.queue(bytes));
    assert(f.d.hostKissFramePending() && !f.d.queue(bytes));f.tick();
    assert(f.radio.sends==1 && f.radio.active==1 && f.radio.sent==std::vector<uint8_t>(bytes,bytes+3));
    assert(!f.d.cancelHostKissFrame());f.radio.complete=true;f.tick(20);
    assert(completions.size()==done+1 && completions.back() && f.d.done.empty());
    assert(!f.d.hostKissFramePending() && f.radio.p.tx_packets[1]==1);
    assert(!f.d.getNumSentFlood() && !f.d.getNumSentDirect() && f.d.getTotalAirTime()==20);
  }
  { Fixture f;auto before=rx;f.radio.active=1;f.radio.incoming=true;f.tick();
    assert(rx==before+1 && !f.d.foreign_rx && !f.d.mc_rx && !f.d.raw_logs);
    assert(!f.d.getNumRecvFlood() && !f.d.getNumRecvDirect() && !f.d.getLastMeshCoreRecvMillis());
    assert(f.radio.processed==1 && f.radio.p.rx_packets[1]==1);
    f.radio.p.secondary.mode=RadioProfileMode::Off;f.radio.incoming=true;f.tick();assert(rx==before+1);
    f.radio.active=0;f.radio.p.primary.protocol=RadioProfileProtocol::Kiss;
    f.radio.incoming=true;f.tick();assert(rx==before+1 && !f.d.foreign_rx && !f.d.raw_logs);
  }
  { Fixture f;auto done=completions.size();assert(f.d.queue(bytes));assert(f.d.cancelHostKissFrame());
    assert(completions.size()==done+1 && !completions.back() && !f.d.hostKissFramePending());
    f.tick();assert(!f.radio.sends && f.d.done.empty());
  }
  { Fixture f;auto done=completions.size();assert(f.d.queue(bytes));++f.radio.p.generation[1];f.tick();
    assert(!f.radio.sends && !f.d.hostKissFramePending() && completions.size()==done+1 && !completions.back());
  }
  { Fixture f;auto done=completions.size();assert(f.d.queue(bytes));f.tick();++f.radio.p.generation[1];
    f.radio.complete=true;f.tick();assert(completions.size()==done+1 && !completions.back() && f.radio.finishes==1);
  }
  { Fixture f;assert(!f.d.submitHostKissFrame(bytes,3,f.radio.p.generation[1]+1));
    f.radio.supported=false;assert(!f.d.queue(bytes));f.radio.supported=true;
    f.radio.p.secondary.mode=RadioProfileMode::Rx;assert(!f.d.queue(bytes));
    f.radio.p.secondary.mode=RadioProfileMode::Off;assert(!f.d.queue(bytes));
    f.radio.p.secondary.mode=RadioProfileMode::RxTx;f.radio.p.secondary.params.protocol=RadioProfileProtocol::MeshCore;
    assert(!f.d.queue(bytes));f.radio.p.secondary.params.protocol=RadioProfileProtocol::Kiss;
    assert(!f.d.queue(nullptr));assert(!f.d.queue(bytes,0));uint8_t full[256]={};assert(!f.d.queue(full,256));
    f.radio.carrier=true;assert(!f.d.queue(bytes));f.radio.carrier=false;
    f.d.allow_foreign=false;assert(!f.d.queue(bytes));
  }
  { Fixture f;auto done=completions.size();f.radio.prepare=RadioParamApplyResult::BUSY;
    assert(f.d.queue(bytes));f.tick();assert(!f.radio.sends && f.d.hostKissFramePending());
    f.radio.prepare=RadioParamApplyResult::APPLIED;f.tick(11);assert(f.radio.sends==1 && f.radio.active==1);
    f.d.setRadioAvailable(false);assert(completions.size()==done+1 && !completions.back());
  }
  { Fixture f;auto done=completions.size();f.radio.start_ok=false;assert(f.d.queue(bytes));f.tick();f.tick(2000);
    assert(f.radio.sends==1 && !f.d.hostKissFramePending() && completions.size()==done+1 && !completions.back());
  }
  { Fixture f;auto done=completions.size();assert(f.d.queue(bytes));f.tick();f.tick(1151);
    assert(f.radio.sends==1 && f.radio.finishes==1 && f.radio.recovery==1);
    assert(completions.size()==done+1 && !completions.back());
  }
  { Fixture f;f.radio.receiving=true;assert(f.d.queue(bytes));f.tick();f.tick(201);
    assert(!f.radio.sends && f.d.hostKissFramePending());f.radio.receiving=false;f.tick(201);
    assert(f.radio.sends==1 && f.radio.active==1);
  }
  { Fixture f;f.d.duty_window=200;f.d.begin();assert(f.d.queue(bytes));f.tick();
    f.clock.now+=100;f.radio.complete=true;f.d.loop();assert(f.d.getRemainingTxBudget()==0);
    assert(f.d.queue(bytes));f.tick(100);assert(f.radio.sends==1);f.tick(101);assert(f.radio.sends==2);
  }
  { Fixture f;assert(f.d.queue(bytes));f.tick();f.radio.complete=true;f.tick();assert(f.d.queue(bytes));
    auto* p=newPacket(f.d,RADIO_TX_PRIMARY);assert(f.d.sendPacket(p,0));f.tick();
    assert(f.radio.sends==2 && f.radio.active==0);f.radio.complete=true;f.tick();f.tick();
    assert(f.radio.sends==3 && f.radio.active==1);
  }
  { Fixture f;auto* p=newPacket(f.d);f.radio.p.cross=RadioCrossMode::On;
    assert(!f.radio.p.canCross() && f.d.getTransmitProfileMask(p)==1);
    assert(f.d.sendPacket(p,0));f.tick();assert(f.radio.sends==1 && f.radio.active==0);
    auto* denied=newPacket(f.d,RADIO_TX_SECONDARY);assert(!f.d.sendPacket(denied,0));
  }
}
'''
        self.run_cpp(fixture + hooks, dispatcher=True)

    def test_actual_protocol_selection_on_every_supported_wrapper(self):
        for family in ("SX1262", "SX1268", "SX1276", "LLCC68", "STM32WLx", "LR1110", "LR1121", "LR2021"):
            with self.subTest(family=family):
                source = (ROOT / "src/helpers/radiolib" / ("Custom" + family + "Wrapper.h")).read_text()
                select = body(source, "bool applyProtocol(mesh::RadioProfileProtocol protocol)")
                select = select.replace(" override", "")
                harness = r'''
#include <cassert>
#include <RadioProfiles.h>
#include <helpers/radiolib/LR2021ApplyStatus.h>
#define RADIOLIB_ERR_NONE 0
struct Driver {
  unsigned calls=0; uint8_t sync=0; int status=0;
  int setProfileProtocol(uint8_t s) { ++calls;sync=s;return status; }
};
using @CLASS@=Driver;
struct TestWrapper {
  Driver driver; Driver* _radio=&driver;
  bool supportsMeshtext() const { return false; }
  bool supportsHostKiss() const { return true; }
  int recordApply(mesh::lr2021::ApplyStatus::Stage,int code) { return code; }
  @SELECT@
};
int main() {
  using P=mesh::RadioProfileProtocol;
  TestWrapper w;assert(w.applyProtocol(P::Kiss));assert(w.driver.sync==0x12 && w.driver.calls==1);
  assert(w.applyProtocol(P::MeshCore));assert(w.driver.sync==0x12 && w.driver.calls==2);
  assert(!w.applyProtocol(static_cast<P>(255)) && w.driver.calls==2);
  w.driver.status=-17;assert(!w.applyProtocol(P::Kiss));
}
'''
                self.run_cpp(harness.replace("@CLASS@", "Custom" + family).replace("@SELECT@", select))

    def test_actual_cli_protocol_lease_preserves_saved_intent_and_other_owners(self):
        code = r'''
#define main existing_cli_main
#include "test/fixtures/radio_profiles/cli_test.cpp"
#undef main
int main() {
  using P=mesh::RadioProfileProtocol;
  Fixture f;
  f.cmd("set radio2 910.5,500,8,5,rxtx,80");f.advance(2000);
  const auto saved=f.fs.files["/radio_profiles"];
  const auto tuple=f.radio.p.secondary;
  const auto intent=f.cli.secondaryIntentGeneration();
  const auto generation=f.radio.p.generation[1];
  assert(f.cli.manualSecondaryConfigured() && f.cli.secondaryRuntimeAvailable());
  assert(f.cli.useSecondaryProtocol(P::Kiss));
  assert(f.radio.p.secondary.params.protocol==P::Kiss && f.radio.p.generation[1]!=generation);
  assert(f.radio.profilePreamble(1)==80 && f.fs.files["/radio_profiles"]==saved);
  assert(f.cli.secondaryIntentGeneration()==intent && f.cli.savedSecondaryConfig().params==tuple.params);
  const auto kiss_generation=f.radio.p.generation[1];
  assert(f.cli.useSecondaryProtocol(P::Kiss) && f.radio.p.generation[1]==kiss_generation);
  // A running host lease is not replaceable by another foreign service.
  assert(!f.cli.useSecondaryProtocol(P::Meshtext));
  assert(!f.cli.useSecondaryProtocol(P::Ethernet));
  f.cli.clearSecondaryProtocol();
  assert(f.radio.p.secondary.params==tuple.params && f.radio.p.secondary.mode==tuple.mode);
  assert(f.fs.files["/radio_profiles"]==saved);
  // Saved manual changes retire the host intent immediately, while preserving
  // the two-second reply window before publishing the new running tuple.
  assert(f.cli.useSecondaryProtocol(P::Kiss));
  f.cmd("set radio2 912.5,500,9,5,rxtx,88");
  assert(f.cli.secondaryIntentGeneration()!=intent && !f.cli.secondaryRuntimeAvailable());
  assert(f.radio.p.secondary.params.protocol==P::Kiss && f.radio.p.secondary.params.freq==910.5f);
  f.cli.clearSecondaryProtocol();f.advance(1999);
  assert(f.radio.p.secondary.params.protocol==P::Kiss && f.radio.p.secondary.params.freq==910.5f);
  f.advance(1);
  assert(f.radio.p.secondary.params.protocol==P::MeshCore && f.radio.p.secondary.params.freq==912.5f);
  assert(f.radio.profilePreamble(1)==88 && f.cli.secondaryRuntimeAvailable());
  assert(f.cli.useSecondaryProtocol(P::Kiss));
  // A direct outside owner wins even if the old host later releases its
  // overlay. Old host cleanup must not restore the saved tuple over it.
  auto foreign=f.radio.p.secondary;foreign.params.protocol=P::Meshtext;foreign.params.freq=913.5f;
  f.radio.p.setSecondary(foreign,false);
  f.cli.clearSecondaryProtocol();f.advance(1);
  assert(f.radio.p.secondary.params==foreign.params && !f.cli.secondaryRuntimeAvailable());
  f.cmd("set radio2 914.5,500,9,5,rxtx,88");f.advance(2000);
  assert(f.cli.secondaryRuntimeAvailable() && f.cli.useSecondaryProtocol(P::Kiss));
#if MESH_ENABLE_MESHTEXT_CHAT
  f.cli.clearSecondaryProtocol();assert(f.cli.useSecondaryProtocol(P::Meshtext));
  const auto mt=f.radio.p.secondary;
  assert(!f.cli.useSecondaryProtocol(P::Kiss) && f.radio.p.secondary.params==mt.params);
#endif
#if MESH_ENABLE_ETHERNET_TUNNEL
  f.cli.clearSecondaryProtocol();assert(f.cli.useSecondaryProtocol(P::Ethernet));
  const auto eth=f.radio.p.secondary;
  assert(!f.cli.useSecondaryProtocol(P::Kiss) && f.radio.p.secondary.params==eth.params);
#endif
  Fixture absent;assert(!absent.cli.useSecondaryProtocol(P::Kiss));
  absent.cmd("set radio2 off");absent.advance(2000);
  assert(!absent.cli.useSecondaryProtocol(P::Kiss));
}
'''
        runner = ethernet_test.EthernetRadioTest()
        runner.setUpClass()
        for foreign in (0, 1):
            with self.subTest(other_foreign_services=foreign):
                runner.compile_run(code, ["-DMESH_ENABLE_HOST_KISS=1",
                    "-DMESH_ENABLE_MESHTEXT_CHAT=" + str(foreign),
                    "-DMESH_ENABLE_ETHERNET_TUNNEL=" + str(foreign)],
                    ["src/helpers/RadioProfileCLI.cpp"], ["test/fixtures/radio_profiles/mocks"])

    def test_feature_layout_is_consistent_with_platform_and_explicit_overrides(self):
        checker = feature_config.MeshtextConfigTest()
        checker.setUp()
        for platform in ("ESP32_PLATFORM", "NRF52_PLATFORM"):
            checker.assert_orders([platform, "COMPANION_RADIO_FULL=1"], 1, feature="MESH_ENABLE_HOST_KISS")
            checker.assert_orders([platform, "COMPANION_RADIO_FULL=0"], 0, feature="MESH_ENABLE_HOST_KISS")
            checker.assert_orders([platform, "COMPANION_RADIO_FULL=1", "MESH_ENABLE_HOST_KISS=0"], 0,
                                  feature="MESH_ENABLE_HOST_KISS")
            checker.assert_orders([platform, "MESH_ENABLE_HOST_KISS=1"], 1, feature="MESH_ENABLE_FOREIGN_RADIO")
        checker.assert_orders([], 0, feature="MESH_ENABLE_HOST_KISS")
        checker.assert_orders(["RP2040_PLATFORM", "COMPANION_RADIO_FULL=1"], 0, feature="MESH_ENABLE_HOST_KISS")
        checker.assert_orders(["MESH_PRIMARY_ESPNOW=1", "MESH_ENABLE_HOST_KISS=1"], 0,
                              feature="MESH_ENABLE_HOST_KISS")


if __name__ == "__main__":
    unittest.main()
