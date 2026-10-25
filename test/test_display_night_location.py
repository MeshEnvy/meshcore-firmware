#!/usr/bin/env python3
"""Run the night location cache and verified-advert filter on the host."""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]

MESH = r'''
#pragma once
#include <stdint.h>
#include <string.h>
#define MAX_ADVERT_DATA_SIZE 32
namespace mesh {
class Identity {
public:
  uint8_t pub_key[32] = {};
  Identity(uint32_t prefix = 0) { memcpy(pub_key, &prefix, sizeof(prefix)); }
  bool matches(const Identity& other) const {
    return memcmp(pub_key, other.pub_key, sizeof(pub_key)) == 0;
  }
};
class Packet {
public:
  bool radio_local = false, forwarded = false, transport = false;
  uint16_t transport_codes[2] = {};
  uint8_t hops = 0;
  uint8_t getPathHashCount() const { return hops; }
  bool hasTransportCodes() const { return transport; }
};
}
'''

SENSORS = r'''
#pragma once
class SensorManager {
public:
  bool valid = false;
  double latitude = 0, longitude = 0;
  mutable unsigned reads = 0;
  bool getCachedGpsPosition(double& lat, double& lon) const {
    ++reads; lat = latitude; lon = longitude; return valid;
  }
};
'''

HARNESS = r'''
#include <cassert>
#include <cmath>
#include <limits>
#include <string>
#include <Mesh.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/SensorManager.h>
#include <helpers/ui/DisplayNightLocation.h>

using mesh::ui::DisplayNightLocationCache;
static unsigned updates = 0;
static bool observed_valid = false;
static int32_t observed_lat = 0, observed_lon = 0;
static uint32_t observed_utc = 0;
namespace mesh { namespace ui {
void updateDisplayNightMode(uint32_t utc, bool valid, int32_t lat, int32_t lon) {
  ++updates; observed_valid = valid; observed_lat = lat; observed_lon = lon;
  observed_utc = utc;
}
} }

static void expect(DisplayNightLocationCache& cache, uint32_t now,
                   int32_t expected_lat, int32_t expected_lon) {
  int32_t lat = 123, lon = 123;
  assert(cache.average(now, lat, lon));
  assert(lat == expected_lat && lon == expected_lon);
}
static void missing(DisplayNightLocationCache& cache, uint32_t now) {
  int32_t lat = 123, lon = 123;
  assert(!cache.average(now, lat, lon));
  assert(lat == 0 && lon == 0);
}
static void advert(mesh::Packet& packet, uint32_t prefix, uint32_t now,
                   int32_t latitude = 10000000, int32_t longitude = 20000000,
                   uint8_t type = ADV_TYPE_CHAT) {
  uint8_t bytes[9];
  bytes[0] = type | ADV_LATLON_MASK;
  memcpy(bytes + 1, &latitude, 4); memcpy(bytes + 5, &longitude, 4);
  mesh::ui::observeDisplayNightAdvert(&packet, mesh::Identity(prefix),
      mesh::Identity(99), bytes, sizeof(bytes), now);
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string scenario = argv[1];
  DisplayNightLocationCache cache;
  static_assert(sizeof(cache) == 192, "The 16 observation cache must stay 192 bytes");
  const uint32_t max_age = DisplayNightLocationCache::MAX_AGE_MS;
  if (scenario == "unique") {
    missing(cache, 0);
    assert(cache.observe(1, 10000000, 20000000, 0));
    assert(cache.observe(2, 30000000, 40000000, 1));
    for (unsigned i = 0; i < 1000; ++i)
      assert(cache.observe(1, 10000000, 20000000, i + 2));
    expect(cache, 1002, 20000000, 30000000);
    cache.observe(1, 50000000, 60000000, 1003);
    expect(cache, 1003, 40000000, 50000000);
  } else if (scenario == "freshness") {
    cache.observe(1, 10000000, 20000000, 100);
    expect(cache, 100 + max_age, 10000000, 20000000);
    missing(cache, 101 + max_age);
    missing(cache, 100); // Expiration cannot resurrect after millis wraps.
    cache.observe(1, 10000000, 20000000, UINT32_MAX - 100);
    expect(cache, 100, 10000000, 20000000);
    missing(cache, max_age + 1);
  } else if (scenario == "capacity") {
    for (unsigned i = 1; i <= 16; ++i)
      cache.observe(i, i * 1000000, 20000000, i);
    expect(cache, 17, 8500000, 20000000);
    cache.observe(17, 17000000, 20000000, 17);
    expect(cache, 17, 9500000, 20000000); // Replaced oldest, not a recent peer.
    cache.observe(2, 2000000, 20000000, 18);
    cache.observe(18, 18000000, 20000000, 19);
    expect(cache, 19, 10437500, 20000000); // Identity 2 was refreshed, 3 evicted.
  } else if (scenario == "coordinates") {
    for (const auto& point : {std::pair<int32_t,int32_t>{0,0},
        {90000001,0}, {-90000001,0}, {0,180000001}, {0,-180000001}})
      assert(!cache.observe(1, point.first, point.second, 0));
    missing(cache, 0);
    cache.observe(1, -12345678, 23456789, 0);
    expect(cache, 0, -12350000, 23460000);
    cache.observe(1, 90000000, 180000000, 1);
    expect(cache, 1, 90000000, 180000000);
  } else if (scenario == "dateline") {
    cache.observe(1, 10000000, 179000000, 0);
    cache.observe(2, 30000000, -179000000, 0);
    expect(cache, 0, 20000000, 180000000);
    DisplayNightLocationCache reversed;
    reversed.observe(2, 30000000, -179000000, 0);
    reversed.observe(1, 10000000, 179000000, 0);
    int32_t lat, lon;
    assert(reversed.average(0, lat, lon) && lat == 20000000 && std::abs(lon) == 180000000);
    cache.observe(3, 20000000, 180000000, 0);
    expect(cache, 0, 20000000, 180000000);
  } else if (scenario == "gps") {
    SensorManager sensors;
    cache.observe(1, 10000000, 20000000, 0);
    int32_t lat, lon;
    assert(mesh::ui::readDisplayNightLocation(cache, sensors, 0, lat, lon));
    assert(lat == 10000000 && lon == 20000000);
    sensors.valid = true; sensors.latitude = 47.6062; sensors.longitude = -122.3321;
    assert(mesh::ui::readDisplayNightLocation(cache, sensors, 1, lat, lon));
    assert(lat == 47606200 && lon == -122332100);
    assert(mesh::ui::readDisplayNightLocation(cache, sensors, max_age + 1, lat, lon));
    sensors.valid = false;
    assert(!mesh::ui::readDisplayNightLocation(cache, sensors, max_age + 2, lat, lon));
    cache.observe(1, 10000000, 20000000, max_age + 2);
    sensors.valid = true;
    for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), 91.0}) {
      sensors.latitude = invalid;
      assert(mesh::ui::readDisplayNightLocation(cache, sensors, max_age + 2, lat, lon));
      assert(lat == 10000000 && lon == 20000000);
    }
  } else if (scenario == "filter") {
    mesh::Packet packet;
    SensorManager sensors;
    packet.hops = 1; advert(packet, 1, 0);
    packet.hops = 0; packet.transport = true; advert(packet, 2, 0); // Share.
    packet.transport = false; packet.forwarded = true; advert(packet, 3, 0); // Bridge.
    packet.forwarded = false; packet.radio_local = true; advert(packet, 4, 0); // Import.
    packet.radio_local = false; advert(packet, 99, 0); // Self.
    advert(packet, 5, 0, 0, 0);
    const uint8_t missing_coords[] = {ADV_TYPE_CHAT};
    mesh::ui::observeDisplayNightAdvert(&packet, mesh::Identity(6), mesh::Identity(99),
        missing_coords, sizeof(missing_coords), 0);
    const uint8_t truncated[] = {ADV_TYPE_CHAT | ADV_LATLON_MASK};
    mesh::ui::observeDisplayNightAdvert(&packet, mesh::Identity(7), mesh::Identity(99),
        truncated, sizeof(truncated), 0);
    mesh::ui::refreshDisplayNightLocation(sensors, 123, 0);
    assert(updates == 1 && !observed_valid && observed_utc == 123);
    // All advertised role types qualify, independent of contact admission.
    for (uint8_t type : {ADV_TYPE_CHAT, ADV_TYPE_REPEATER, ADV_TYPE_ROOM, ADV_TYPE_SENSOR})
      advert(packet, type, 1, type * 10000000, 20000000, type);
    for (unsigned i = 0; i < 100; ++i) advert(packet, 1, 2);
    mesh::ui::refreshDisplayNightLocation(sensors, 124, 14999);
    assert(updates == 1); // Source refresh is bounded to one pass per 15 seconds.
    mesh::ui::refreshDisplayNightLocation(sensors, 125, 15000);
    assert(updates == 2 && observed_valid && observed_lat == 25000000 && observed_lon == 20000000);
    packet.hops = 1; advert(packet, 1, max_age);
    mesh::ui::refreshDisplayNightLocation(sensors, 126, max_age + 3);
    assert(updates == 3 && !observed_valid); // Relayed traffic did not extend RF age.
  } else assert(false);
}
'''


class DisplayNightLocationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get("CXX", "g++")
        if shutil.which(compiler) is None:
            raise unittest.SkipTest("C++ compiler unavailable")
        cls.temp = tempfile.TemporaryDirectory(prefix="mesh-night-location-")
        temp = Path(cls.temp.name)
        (temp / "helpers").mkdir()
        (temp / "Mesh.h").write_text(MESH, encoding="ascii")
        (temp / "Arduino.h").write_text("#pragma once\n", encoding="ascii")
        (temp / "helpers/SensorManager.h").write_text(SENSORS, encoding="ascii")
        (temp / "main.cpp").write_text(HARNESS, encoding="ascii")
        cls.exe = temp / "location"
        subprocess.run([
            compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-Wno-reorder",
            "-DDISPLAY_CLASS=ST7735Display",
            "-I", str(temp), "-I", str(ROOT / "src"), str(temp / "main.cpp"),
            str(ROOT / "src/helpers/ui/DisplayNightLocation.cpp"),
            str(ROOT / "src/helpers/AdvertDataHelpers.cpp"), "-o", str(cls.exe),
        ], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_location_sources(self):
        for scenario in ("unique", "freshness", "capacity", "coordinates", "dateline", "gps", "filter"):
            with self.subTest(scenario=scenario):
                subprocess.run([str(self.exe), scenario], check=True)


if __name__ == "__main__":
    unittest.main()
