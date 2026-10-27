#!/usr/bin/env python3
"""Exercise late KISS delimiters through the real Companion USB parser."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from cpp_source import body
import test_companion_usb_input as usb
from test_replay_reset_integration import extract_braced

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
#include <helpers/ArduinoSerialInterface.h>
#include <cassert>
#include <deque>
#include <vector>
static bool enabled=true, active=false;
static unsigned gate_calls=0;
static bool receiveIdle(int next) { ++gate_calls; return !enabled || (next!=0xc0 && !active); }
struct Port:Stream {
  std::deque<uint8_t> input;
  unsigned reads=0;
  int available()override{return int(input.size());}
  int read()override{if(input.empty())return -1;auto c=input.front();input.pop_front();++reads;return c;}
  int peek()override{return input.empty()?-1:input.front();}
  int availableForWrite()override{return 65536;}
  size_t write(uint8_t)override{return 1;}
  size_t write(const uint8_t*,size_t n)override{return n;}
  void push(const std::vector<uint8_t>& bytes){input.insert(input.end(),bytes.begin(),bytes.end());}
};
int main(){
  Port port;ArduinoSerialInterface usb;uint8_t out[MAX_FRAME_SIZE]={};
  usb.begin(port);usb.enable();usb.setIdleReceiveCheck(receiveIdle);
  assert(!usb.isReceivingFrame());
  // The top-of-loop KISS owner saw an empty port. A complete KISS frame then
  // arrives during mesh dispatch; the idle Companion parser must leave it all.
  assert(!usb.checkRecvFrame(out));
  const std::vector<uint8_t> kiss={0xc0,0,0x41,0xdb,0xdc,0xc0};
  port.push(kiss);assert(!usb.checkRecvFrame(out));
  assert(port.reads==0 && std::vector<uint8_t>(port.input.begin(),port.input.end())==kiss);
  port.input.clear();
  // Every possible split of an existing length-prefixed frame preserves its
  // opaque payload delimiter, including one arriving just after the split.
  const std::vector<uint8_t> binary={'<',5,0,0x42,0xc0,0xdb,0,0xff};
  for(size_t split=1;split<binary.size();++split){
    usb.resetSessionState();port.input.clear();gate_calls=0;
    port.push(std::vector<uint8_t>(binary.begin(),binary.begin()+split));
    assert(!usb.checkRecvFrame(out) && usb.isReceivingFrame());
    const unsigned before=gate_calls;
    port.push(std::vector<uint8_t>(binary.begin()+split,binary.end()));
    assert(usb.checkRecvFrame(out)==5 && !usb.isReceivingFrame());
    assert(std::vector<uint8_t>(out,out+5)==std::vector<uint8_t>(binary.begin()+3,binary.end()));
    assert(gate_calls==before); // no idle gate anywhere inside the frame
  }
  // A completed CLI RPC followed immediately by FEND dispatches the RPC once
  // and leaves the complete next protocol frame untouched on the same stream.
  port.push(binary);port.push(kiss);
  assert(usb.checkRecvFrame(out)==5);const unsigned before=port.reads;
  assert(!usb.checkRecvFrame(out) && port.reads==before && port.peek()==0xc0);
  port.input.clear();
  // Off and non-USB callers keep the original framing behavior.
  enabled=false;port.push(kiss);port.push(binary);
  assert(usb.checkRecvFrame(out)==5 && port.input.empty());
  usb.setIdleReceiveCheck(nullptr);enabled=true;port.push(kiss);port.push(binary);
  assert(usb.checkRecvFrame(out)==5 && port.input.empty());
}
'''


class HostKissIngressTest(unittest.TestCase):
    def test_late_fend_is_reserved_and_binary_payload_is_opaque(self):
        compiler = os.environ.get('CXX') or shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler)
        source = (ROOT / 'src/helpers/ArduinoSerialInterface.cpp').read_text()
        gate = ('    if (_state == RECV_STATE_IDLE && _idle_receive_check != nullptr\n'
                '        && !_idle_receive_check(_serial->peek())) return 0;\n')
        self.assertEqual(source.count(gate), 1)
        for negative in (False, True):
            with self.subTest(negative=negative), tempfile.TemporaryDirectory(prefix='host-kiss-ingress-') as directory:
                directory = Path(directory)
                (directory / 'fixture.cpp').write_text(FIXTURE, encoding='ascii')
                (directory / 'transport.cpp').write_text(source.replace(gate, '') if negative else source, encoding='ascii')
                flags = (['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                          '-fno-pie', '-no-pie'] if sys.platform.startswith('linux') else [])
                result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', *flags,
                    '-I', str(ROOT / 'test/mocks'), '-I', str(ROOT / 'src'),
                    '-I', str(ROOT / 'src/helpers'), str(directory / 'fixture.cpp'),
                    str(directory / 'transport.cpp'), '-o', str(directory / 'test')],
                    text=True, capture_output=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(directory / 'test')], text=True, capture_output=True, timeout=10)
                if negative:
                    self.assertNotEqual(result.returncode, 0, 'Old parser unexpectedly preserved late KISS input')
                else:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_actual_ascii_pump_leaves_late_fend_even_with_partial_input(self):
        main = (ROOT / 'examples/companion_radio/main.cpp').read_text()
        signatures = (
            'static void clearUsbTerminalLine()',
            'static bool acceptUsbBinaryStartupFrame(uint32_t completed_at)',
            'static void enterUsbTerminalMode(bool show_banner',
            'static void leaveUsbTerminalMode(bool acknowledge)',
            'static void resetUsbMotaMode()', 'static void leaveUsbMotaMode(bool acknowledge)',
            'static bool enterUsbMotaMode(mesh::UsbMotaEntryOrigin origin)',
            'static void serviceUsbMota()', 'static bool serviceUsbMotaResponseBoundary()',
            'static void resetUsbTerminalHostSession()', 'static void serviceUsbTerminal()',
            'static void expireUsbBinaryStartupProbeBeforeDispatch()',
        )
        functions = '\n'.join(extract_braced(main, signature) for signature in signatures)
        functions = functions.replace('static void enterUsbTerminalMode(bool show_banner)',
                                      'static void enterUsbTerminalMode(bool show_banner = true)')
        functions = functions.replace('mesh::hostkiss::usbInputReserved(', 'reserveTestInput(')
        accessor = extract_braced(usb.CONTEXT, 'static SerialMotaSource& serialFolderSource()')
        declarations = ('static bool reserve_kiss=false;\n'
                        'static bool reserveTestInput(int next){return reserve_kiss && next==0xc0;}\n')
        checks = r"""
  // An expired Binary probe must not drain FEND or print its ASCII banner.
  reset();reserve_kiss=true;leaveUsbTerminalMode(false);
  usb_binary_startup_probe.start(g_mock_millis,0);g_mock_millis+=1001;
  input.bytes={0xc0,0,0x41,0xc0};const auto expired_output=input.output;
  expireUsbBinaryStartupProbeBeforeDispatch();
  assert(input.bytes.size()==4 && input.peek()==0xc0 && input.output==expired_output);
  assert(!the_mesh.terminal && usb_binary_startup_probe.isActive());
  for(unsigned mode=0;mode<3;++mode){
    reset();reserve_kiss=true;
    if(mode==1){input.push("prefix");serviceUsbTerminal();}
    if(mode==2)usb_terminal_discard_line=true;
    const auto output_before=input.output;
    const std::vector<uint8_t> wire={0xc0,0,'v','e','r','\r',0xc0};
    for(auto byte:wire)input.bytes.push_back(byte);
    serviceUsbTerminal();
    assert(std::vector<uint8_t>(input.bytes.begin(),input.bytes.end())==wire);
    assert(input.output==output_before && the_mesh.commands.empty());
  }
  reserve_kiss=false;
"""
        template = usb.HARNESS.replace('@FUNCTIONS@', declarations + functions)
        template = template.replace('@SOURCE_ACCESSOR@', accessor).replace('int main() {', 'int main() {' + checks)
        template = usb.bind_production_input_constants(template)
        guard = '    if (reserveTestInput(usb_input.peek())) return;\n'
        self.assertEqual(template.count(guard), 2)
        for negative in (False, True):
            with self.subTest(negative=negative), tempfile.TemporaryDirectory(prefix='host-kiss-ascii-') as directory:
                directory = Path(directory)
                (directory / 'fixture.cpp').write_text(template.replace(guard, '') if negative else template, encoding='ascii')
                result = subprocess.run(['g++', '-std=c++17', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-fno-pie', '-no-pie',
                    '-I', str(ROOT / 'test/mocks'), '-I', str(ROOT / 'src'),
                    str(directory / 'fixture.cpp'), str(ROOT / 'src/helpers/ArduinoSerialInterface.cpp'),
                    str(ROOT / 'src/helpers/ota/MotaSourceSerial.cpp'), '-o', str(directory / 'test')],
                    text=True, capture_output=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(directory / 'test')], text=True, capture_output=True, timeout=15)
                if negative:
                    self.assertNotEqual(result.returncode, 0, 'Old ASCII pump unexpectedly preserved late FEND')
                else:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_every_raw_usb_consumer_defers_reserved_input(self):
        main = (ROOT / 'examples/companion_radio/main.cpp').read_text()
        setup = body(main, 'void setup()')
        self.assertIn('usb_serial_interface.setIdleReceiveCheck', setup)
        self.assertIn('return !mesh::hostkiss::usbInputReserved(next_byte)', setup)
        for signature in ('static void serviceUsbTerminal()',
                          'static void serviceUsbLoggingOwnership(bool logging_enabled)',
                          'static void expireUsbBinaryStartupProbeBeforeDispatch()'):
            self.assertIn('mesh::hostkiss::usbInputReserved(', body(main, signature))
        rescue = body((ROOT / 'examples/companion_radio/MyMesh.cpp').read_text(), 'void MyMesh::checkCLIRescueCmd()')
        self.assertLess(rescue.index('mesh::hostkiss::usbInputReserved(input.peek())'), rescue.index('input.read()'))
        for role in ('simple_repeater', 'simple_room_server', 'simple_sensor'):
            source = (ROOT / 'examples' / role / 'main.cpp').read_text()
            signature = ('static void __attribute__((noinline)) serviceCommandInterfaces()'
                         if role == 'simple_repeater' else 'void loop()')
            pump = body(source, signature)
            read = 'const int value = console_read()' if role == 'simple_repeater' else 'console.read()'
            self.assertLess(pump.index('mesh::hostkiss::usbInputReserved(console.peek())'), pump.index(read))


if __name__ == '__main__':
    unittest.main()
