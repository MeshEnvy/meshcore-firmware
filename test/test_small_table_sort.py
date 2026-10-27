#!/usr/bin/env python3
"""Exercise the production small-table sorter and its platform fallback."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

from cpp_source import body

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>
#include <helpers/ContactListOrder.h>
#include <helpers/SmallTableSort.h>

struct Contact {
  uint8_t flags;
  uint32_t timestamp;
};
static std::array<Contact, 358> contacts;
static size_t comparisons;
static int compareIndexes(const void* a, const void* b) {
  ++comparisons;
  const Contact& left = contacts[*static_cast<const uint16_t*>(a)];
  const Contact& right = contacts[*static_cast<const uint16_t*>(b)];
  return mesh::compareContactListOrder(left.flags, left.timestamp,
                                       right.flags, right.timestamp);
}
struct Advert {
  uint8_t public_key[8];
  char name[32];
  uint32_t timestamp;
  uint8_t path[64];
  uint16_t identity;
};
static_assert(sizeof(uint16_t) == 2, "production compact contact index");
static_assert(sizeof(Advert) > 100, "exercise wide bytewise record swaps");
static int compareAdverts(const void* a, const void* b) {
  ++comparisons;
  const auto left = static_cast<const Advert*>(a)->timestamp;
  const auto right = static_cast<const Advert*>(b)->timestamp;
  return left > right ? -1 : left < right ? 1 : 0;
}
// Defined in a second translation unit: both consumers must share the inline
// header's single STM32/nRF52 function body and compatible callback signature.
void sortElsewhere(void*, size_t, size_t, int (*)(const void*, const void*));

static void checkComparisonBound(size_t count) {
  size_t height = 0;
  for (size_t level = count; level > 1; level = (level + 1) / 2) ++height;
  // Allow two comparisons per heap level and linear heap construction. This
  // also admits the platform qsort fallback without fixing one comparison
  // sequence or the unspecified tie order. Quadratic selection sort fails
  // this bound on both 108- and 358-entry tables.
  assert(comparisons <= count * (2 * height + 2));
}

static void checkIndexes(size_t count, unsigned order, std::mt19937& random) {
  std::vector<uint16_t> indexes(count);
  std::iota(indexes.begin(), indexes.end(), uint16_t(0));
  std::sort(indexes.begin(), indexes.end(), [](uint16_t a, uint16_t b) {
    return compareIndexes(&a, &b) < 0;
  });
  if (order == 1) std::reverse(indexes.begin(), indexes.end());
  else if (order == 2) std::shuffle(indexes.begin(), indexes.end(), random);
  comparisons = 0;
  mesh::sortSmallTable(indexes.data(), count, sizeof(indexes[0]), compareIndexes);
  checkComparisonBound(count);
  std::vector<bool> seen(count);
  for (size_t i = 0; i < count; ++i) {
    assert(indexes[i] < count && !seen[indexes[i]]);
    seen[indexes[i]] = true;
    if (i) assert(compareIndexes(&indexes[i - 1], &indexes[i]) <= 0);
  }
}
static void checkAdverts(size_t count, unsigned order, std::mt19937& random) {
  std::vector<Advert> original(count), records(count);
  for (size_t i = 0; i < count; ++i) {
    // Include object padding in the preservation check. Repeated timestamps
    // intentionally leave equal-key ordering unspecified, matching qsort.
    std::memset(&original[i], static_cast<int>(i ^ 0xa5), sizeof(Advert));
    original[i].identity = static_cast<uint16_t>(i);
    original[i].timestamp = static_cast<uint32_t>((i * 7) % 5);
    records[i] = original[i];
  }
  std::sort(records.begin(), records.end(), [](const Advert& a, const Advert& b) {
    return compareAdverts(&a, &b) < 0;
  });
  if (order == 1) std::reverse(records.begin(), records.end());
  else if (order == 2) std::shuffle(records.begin(), records.end(), random);
  comparisons = 0;
  sortElsewhere(records.data(), count, sizeof(Advert), compareAdverts);
  checkComparisonBound(count);
  std::vector<bool> seen(count);
  for (size_t i = 0; i < count; ++i) {
    const auto identity = records[i].identity;
    assert(identity < count && !seen[identity]);
    seen[identity] = true;
    assert(std::memcmp(&records[i], &original[identity], sizeof(Advert)) == 0);
    if (i) assert(compareAdverts(&records[i - 1], &records[i]) <= 0);
  }
}
int main() {
  std::mt19937 random(0x1a180001);
  mesh::sortSmallTable(nullptr, 0, sizeof(uint16_t), compareIndexes);
  uint16_t untouched = 0x1234;
  mesh::sortSmallTable(&untouched, contacts.size(), 0, compareIndexes);
  assert(untouched == 0x1234);
  for (unsigned pattern = 0; pattern < 3; ++pattern) {
    for (size_t i = 0; i < contacts.size(); ++i) {
      contacts[i].flags = pattern == 0 ? 0 : static_cast<uint8_t>(i % 3 == 0);
      contacts[i].timestamp = pattern == 2 ? 42 : static_cast<uint32_t>((i * 7919) % 31);
    }
    for (size_t count : {0U, 1U, 2U, 16U, 100U, 108U, 350U, 358U}) {
      for (unsigned order = 0; order < 3; ++order) {
        checkIndexes(count, order, random);
        checkAdverts(count, order, random);
      }
    }
  }
  puts("PASS: compact indexes, wide records, duplicate keys, order and permutation");
}
'''

SECOND_TRANSLATION_UNIT = r'''
#include <helpers/SmallTableSort.h>
void sortElsewhere(void* items, size_t count, size_t width,
                   int (*compare)(const void*, const void*)) {
  mesh::sortSmallTable(items, count, width, compare);
}
'''

NEIGHBOUR_HARNESS = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>
#include <helpers/SmallTableSort.h>
struct NeighbourInfo {
  uint32_t heard_timestamp;
  int8_t snr;
  uint16_t identity;
};
@COMPARATORS@
static void rpcSort(NeighbourInfo** sorted_neighbours, int16_t neighbours_count,
                    uint8_t order_by) {
  @RPC_SORT@
}
static void cliSort(NeighbourInfo** sorted_neighbours, int16_t neighbours_count) {
  @CLI_SORT@
}
static bool before(const NeighbourInfo* a, const NeighbourInfo* b, unsigned order) {
  if (order == 0) return a->heard_timestamp > b->heard_timestamp;
  if (order == 1) return a->heard_timestamp < b->heard_timestamp;
  if (order == 2) return a->snr > b->snr;
  return a->snr < b->snr;
}
int main() {
  std::mt19937 random(0x18180002);
  const uint32_t timestamps[] = {1, 42, 42, UINT32_MAX, 1, UINT32_MAX - 1};
  const int8_t snrs[] = {INT8_MIN, -16, -1, 0, 0, 7, 7, INT8_MAX};
  int (*const compare[])(const void*, const void*) = {
    cmp_neighbours_newest_to_oldest, cmp_neighbours_oldest_to_newest,
    cmp_neighbours_strongest_to_weakest, cmp_neighbours_weakest_to_strongest
  };
  for (size_t count : {0U, 1U, 2U, 16U, 50U, 100U}) {
    std::vector<NeighbourInfo> records(count);
    for (size_t i = 0; i < count; ++i) {
      records[i].heard_timestamp = timestamps[i % 6];
      records[i].snr = snrs[i % 8];
      records[i].identity = static_cast<uint16_t>(i);
    }
    const auto original = records;
    for (unsigned order = 0; order < 4; ++order) {
      std::vector<NeighbourInfo*> actual(count), expected(count);
      for (size_t i = 0; i < count; ++i) actual[i] = &records[i];
      std::shuffle(actual.begin(), actual.end(), random);
      expected = actual;
      std::sort(expected.begin(), expected.end(), [order](const NeighbourInfo* a,
                                                         const NeighbourInfo* b) {
        return before(a, b, order);
      });
      rpcSort(actual.data(), static_cast<int16_t>(count), order);
      std::vector<bool> seen(count);
      for (size_t i = 0; i < count; ++i) {
        const auto index = actual[i]->identity;
        assert(index < count && !seen[index] && actual[i] == &records[index]);
        seen[index] = true;
        assert(!before(actual[i], expected[i], order));
        assert(!before(expected[i], actual[i], order));
        assert(compare[order](&actual[i], &expected[i]) == 0);
        if (i) assert(!before(actual[i], actual[i - 1], order));
      }
      // Unsupported order values retain the caller's original pointer order.
      const auto unchanged = actual;
      rpcSort(actual.data(), static_cast<int16_t>(count), 4);
      assert(actual == unchanged);
      cliSort(actual.data(), static_cast<int16_t>(count));
      for (size_t i = 1; i < count; ++i) {
        assert(actual[i - 1]->heard_timestamp >= actual[i]->heard_timestamp);
      }
      for (size_t i = 0; i < count; ++i) {
        assert(records[i].heard_timestamp == original[i].heard_timestamp);
        assert(records[i].snr == original[i].snr);
        assert(records[i].identity == original[i].identity);
      }
    }
  }
  puts("PASS: real neighbour RPC orders and CLI, duplicate keys, negative SNR and pointer records");
}
'''


class SmallTableSortTests(unittest.TestCase):
    def test_real_neighbour_rpc_and_cli_orderings(self):
        compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "a host C++17 compiler is required")
        production = (ROOT / "examples/simple_repeater/MyMesh.cpp").read_text()
        names = ("newest_to_oldest", "oldest_to_newest",
                 "strongest_to_weakest", "weakest_to_strongest")
        comparators = "\n".join(body(production, "static int cmp_neighbours_" + name + "(")
                                for name in names)
        start = production.index("if (order_by == 0) {")
        rpc_sort = production[start:production.index("// build results buffer", start)]
        formatter = body(production, "void MyMesh::formatNeighborsReply(")
        start = formatter.index("// sort neighbours newest to oldest")
        cli_sort = formatter[start:formatter.index("for (int i = 0;", start)]
        source = NEIGHBOUR_HARNESS.replace("@COMPARATORS@", comparators)
        source = source.replace("@RPC_SORT@", rpc_sort).replace("@CLI_SORT@", cli_sort)
        for platform in ("STM32_PLATFORM", "NRF52_PLATFORM", "ESP32_PLATFORM", "RP2040_PLATFORM"):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory(
                    prefix="neighbour-sort-") as directory:
                work = Path(directory)
                (work / "test.cpp").write_text(source)
                binary = work / "sort"
                command = [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                           "-D" + platform + "=1", "-I" + str(ROOT / "src"),
                           str(work / "test.cpp"), "-o", str(binary)]
                if sys.platform.startswith("linux"):
                    command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                    "-fno-pie", "-no-pie"]
                built = subprocess.run(command, capture_output=True, text=True, timeout=60)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                tested = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
                self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
                self.assertIn("negative SNR and pointer records", tested.stdout)

    def test_sort_on_all_platforms(self):
        compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "a host C++17 compiler is required")
        for platform in (None, "STM32_PLATFORM", "NRF52_PLATFORM", "ESP32_PLATFORM", "RP2040_PLATFORM"):
            with self.subTest(platform=platform or "native"):
                with tempfile.TemporaryDirectory(prefix="small-table-sort-") as directory:
                    work = Path(directory)
                    (work / "test.cpp").write_text(HARNESS)
                    (work / "second.cpp").write_text(SECOND_TRANSLATION_UNIT)
                    binary = work / "sort"
                    command = [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                               f"-I{ROOT / 'src'}", str(work / "test.cpp"),
                               str(work / "second.cpp"), "-o", str(binary)]
                    if platform:
                        command.insert(1, f"-D{platform}=1")
                    if sys.platform.startswith("linux"):
                        command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                        "-fno-pie", "-no-pie"]
                    built = subprocess.run(command, capture_output=True, text=True, timeout=60)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    tested = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
                    self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
                    self.assertIn("duplicate keys, order and permutation", tested.stdout)


if __name__ == "__main__":
    unittest.main()
