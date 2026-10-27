#!/usr/bin/env python3
"""Bind the optional KISS heap allowance to the real codec and ELF gate."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"scripts"))
import check_firmware_ram as ram
from test_firmware_ram import elf32,esp_fixture


class HostKissRamTest(unittest.TestCase):
    def test_enabled_codec_adds_two_kib_without_lowering_other_reserves(self):
        for platform in ("ESP32_PLATFORM","NRF52_PLATFORM"):
            for target in ("board_companion_radio_full","board_repeater","board_room_server","board_sensor"):
                with self.subTest(platform=platform,target=target):
                    flags={"BLE_PIN_CODE":123456}
                    off=ram.requirements(platform,{**flags,"MESH_ENABLE_HOST_KISS":0},target)
                    on=ram.requirements(platform,{**flags,"MESH_ENABLE_HOST_KISS":1},target)
                    self.assertEqual(on["components"]["host_kiss_codec"],2048)
                    self.assertEqual(on["required_heap_bytes"],off["required_heap_bytes"]+2048)
                    self.assertEqual({key:value for key,value in on["components"].items()
                                      if key!="host_kiss_codec"},off["components"])
                    self.assertEqual(on["required_contiguous_bytes"],off["required_contiguous_bytes"])
                    default=ram.requirements(platform,{**flags,"COMPANION_RADIO_FULL":1},target)
                    self.assertEqual(default["components"],on["components"])
                    for extra in ({"COMPANION_RADIO_FULL":0},
                                  {"COMPANION_RADIO_FULL":1,"MESH_ENABLE_HOST_KISS":0},
                                  {"MESH_ENABLE_HOST_KISS":1,"MESH_PRIMARY_ESPNOW":1}):
                        self.assertEqual(ram.requirements(platform,{**flags,**extra},target)["components"],off["components"])
        for platform in ("STM32_PLATFORM","RP2040_PLATFORM"):
            self.assertEqual(ram.requirements(platform,{"MESH_ENABLE_HOST_KISS":1},"board_companion_radio_ble"),
                             ram.requirements(platform,{},"board_companion_radio_ble"))

    def test_real_elf_rejects_unbudgeted_codec_and_honors_exact_new_boundary(self):
        for platform in ("ESP32_PLATFORM","NRF52_PLATFORM"):
            with self.subTest(platform=platform),tempfile.TemporaryDirectory() as folder:
                folder=Path(folder);path=folder/"firmware.elf";report=folder/"memory.json"
                target="board_companion_radio_full"
                flags={"COMPANION_RADIO_FULL":1,"MESH_ENABLE_HOST_KISS":1,"BLE_PIN_CODE":123456}
                required=ram.requirements(platform,flags,target)["required_heap_bytes"]
                old=ram.requirements(platform,{**flags,"MESH_ENABLE_HOST_KISS":0},target)["required_heap_bytes"]
                self.assertEqual(required,old+2048)
                for available,accepted in ((old,False),(required-1,False),(required,True)):
                    with self.subTest(available=available),contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
                        if platform=="ESP32_PLATFORM":
                            esp_fixture(path,internal_length=available-0x10000+0x4000)
                        else:
                            elf32(path,{"__HeapBase":(0x20010000,0),"__HeapLimit":(0x20010000+available,0)})
                        status=ram.check_firmware(path,platform,"esp32" if platform=="ESP32_PLATFORM" else "nrf52840",flags,target,report)
                        actual=json.loads(report.read_text())
                        self.assertEqual(status==0,accepted)
                        self.assertEqual(actual["required_heap_bytes"],required)
                        self.assertEqual(actual["components"]["host_kiss_codec"],2048)

    def test_actual_codec_size_is_source_bounded_and_queue_growth_fails(self):
        source=(ROOT/"src/helpers/HostKissCodec.h").read_text()
        with tempfile.TemporaryDirectory() as folder:
            folder=Path(folder);header=folder/"Codec.h";main=folder/"main.cpp";binary=folder/"test"
            main.write_text('#include "Codec.h"\n#include <stdio.h>\nint main(){printf("%zu\\n",sizeof(mesh::hostkiss::Codec));}\n')
            for changed,accepted in ((source,True),(source.replace("QueueDepth = 2","QueueDepth = 3"),False)):
                header.write_text(changed)
                result=subprocess.run(["g++","-std=c++17",str(main),"-o",str(binary)],capture_output=True,text=True)
                self.assertEqual(result.returncode==0,accepted,result.stdout+result.stderr)
                if accepted:
                    size=int(subprocess.check_output([str(binary)]))
                    self.assertEqual(size,1828)
                    self.assertLessEqual(size+32,2048)
                else:
                    self.assertIn("runtime RAM allowance",result.stderr)


if __name__=="__main__":unittest.main()
