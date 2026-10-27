#!/usr/bin/env python3
"""Exercise the actual bounded Reader stream decoder and selected-verse lookup."""
import importlib.util
import json
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]


def raw_deflate(data, level=9, strategy=zlib.Z_DEFAULT_STRATEGY):
    compressor = zlib.compressobj(level, zlib.DEFLATED, -12, 8, strategy)
    return compressor.compress(data) + compressor.flush()


def stored(data, final=True):
    assert len(data) <= 65535
    size = len(data)
    return bytes([int(final)]) + size.to_bytes(2, "little") + (size ^ 65535).to_bytes(2, "little") + data


class Bits:
    def __init__(self):
        self.bits = []

    def put(self, value, count):
        self.bits.extend((value >> bit) & 1 for bit in range(count))

    def huffman(self, value, count):
        self.put(int(f"{value:0{count}b}"[::-1], 2), count)

    def literal(self, symbol):
        if symbol <= 143:
            self.huffman(48 + symbol, 8)
        elif symbol <= 255:
            self.huffman(400 + symbol - 144, 9)
        elif symbol <= 279:
            self.huffman(symbol - 256, 7)
        else:
            self.huffman(192 + symbol - 280, 8)

    def finish(self):
        self.bits.extend([0] * ((-len(self.bits)) % 8))
        return bytes(sum(self.bits[start + bit] << bit for bit in range(8))
                     for start in range(0, len(self.bits), 8))


def back_reference(distance):
    bits = Bits()
    bits.put(1, 1)
    bits.put(1, 2)  # final fixed-Huffman block
    bits.literal(257)  # length 3
    if distance == 4096:
        bits.huffman(23, 5)
        bits.put(1023, 10)
    elif distance == 4097:
        bits.huffman(24, 5)
        bits.put(0, 11)
    else:
        raise AssertionError(distance)
    bits.literal(0)
    bits.literal(256)
    return bits.finish()


def array(data):
    return "{" + ",".join(str(byte) for byte in data) + "}"


class ReaderStreamTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("g++") or not shutil.which("gcc"):
            raise unittest.SkipTest("native C/C++ compilers unavailable")
        cls.temporary = tempfile.TemporaryDirectory(prefix="mesh-reader-stream-")
        cls.directory = Path(cls.temporary.name)
        cls.source = cls.directory / "main.cpp"
        cls.decoder = cls.directory / "tinf.o"
        flags = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-fno-omit-frame-pointer", "-fno-pie", "-I" + str(ROOT / "src"),
                 "-DOTA_TRANSPORT_DEFLATE_TEST=1", "-DTINF_READER_STREAM_TEST=1"]
        subprocess.run(["gcc", "-std=c11", *flags, "-c", str(ROOT / "src/helpers/ota/OtaTinf.c"),
                        "-o", str(cls.decoder)], check=True, capture_output=True)
        cls.flags = flags

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def compile(self, body, name="test"):
        source = self.directory / (name + ".cpp")
        source.write_text(body, encoding="ascii")
        binary = self.directory / name
        result = subprocess.run(["g++", "-std=c++17", *self.flags, "-no-pie", str(source),
                                 str(self.decoder), "-o", str(binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return binary

    def run_binary(self, binary, *args):
        result = subprocess.run([str(binary), *args], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace"))
        return result.stdout

    def test_canonical_stream_header_and_token_vocabulary_are_reproducible(self):
        import sys
        path = str(ROOT / "tools/bible")
        sys.path.insert(0, path)
        try:
            spec = importlib.util.spec_from_file_location("reader_stream_repro_packer", ROOT / "tools/bible/pack_reader_stream.py")
            packer = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(packer)
            document = json.loads(packer.blocks.SOURCE.read_text(encoding="utf-8"))
            self.assertEqual(packer.HEADER.read_text(encoding="ascii"), packer.render(document))
            pinned = json.loads(packer.TOKENS.read_text(encoding="ascii"))
            self.assertLessEqual(len(pinned), 128)
            self.assertEqual(len(pinned), len(set(pinned)))
            for word in pinned:
                self.assertTrue(word)
                self.assertTrue(all(32 <= byte < 127 for byte in word.encode("ascii")))
        finally:
            sys.path.remove(path)

    def test_all_879_verses_match_independently_normalized_source(self):
        binary = self.compile(r"""
#include <helpers/bible/ReaderStreamData.generated.h>
#include <cassert>
#include <cstdio>
#include <cstring>
int main() {
  using namespace mesh::bible;
  struct { unsigned char before[32]; char scratch[kBlockSize]; unsigned char after[32]; } buffer;
  memset(&buffer, 0xa5, sizeof(buffer));
  for (unsigned chapter=1; chapter<=21; ++chapter) {
    for (unsigned verse=1; verse<=kChapterVerses[chapter-1]; ++verse) {
      const char* text=nullptr;
      assert(lookup(generated::readerCorpus, Reference{uint8_t(chapter),uint8_t(verse)},
                    buffer.scratch,sizeof(buffer.scratch),text)==LookupResult::Found);
      assert(text>=buffer.scratch && text<buffer.scratch+sizeof(buffer.scratch));
      assert(memchr(text,0,buffer.scratch+sizeof(buffer.scratch)-text));
      for(auto c:buffer.before) assert(c==0xa5);
      for(auto c:buffer.after) assert(c==0xa5);
      fwrite(text,1,strlen(text)+1,stdout);
    }
  }
}
""", "corpus")
        document = json.loads((ROOT / "tools/bible/reader-web.json").read_text())
        typography = str.maketrans({"\u00a0": " ", "\u2013": "-", "\u2014": "-",
                                   "\u2018": "'", "\u2019": "'", "\u201c": '\"', "\u201d": '\"'})
        expected = b"".join(text.translate(typography).encode("ascii") + b"\0"
                            for text in document["verses"].values())
        self.assertEqual(self.run_binary(binary), expected)

    def test_ring_bounds_truncation_distances_and_deterministic_mutations(self):
        cases = []

        def good(plain, record, expected, compressor=raw_deflate):
            packed = compressor(plain)
            cases.append((packed, len(plain), record, 0, expected))
            return packed

        for compressor in (raw_deflate, stored,
                           lambda plain: raw_deflate(plain, strategy=zlib.Z_FIXED)):
            good(b"hello\0world\0", 1, b"world", compressor)
            good(b"p" * 4090 + b"\0" + b"crossing-boundary-record\0", 1,
                 b"crossing-boundary-record", compressor)
            good(b"q" * 4095 + b"\0", 0, b"q" * 4095, compressor)
            good(b"\0missing\0", 0, b"", compressor)
        good(b"a\0" * 3000 + b"last record\0", 3000, b"last record")
        prefix = b"abc\0" * 1024
        packed = stored(prefix, False) + back_reference(4096)
        self.assertEqual(zlib.decompress(packed, -15), prefix + b"abc\0")
        cases.append((packed, len(prefix) + 4, 1024, 0, b"abc"))
        prefix = b"a\0" * 2049
        packed = stored(prefix, False) + back_reference(4097)
        self.assertEqual(zlib.decompress(packed, -15), prefix + b"\0a\0\0")
        cases.append((packed, len(prefix) + 4, 2049, -1, b""))
        for bad in (b"\x07", b"\x01\x01\x00\x00\x00x", b"\x05\x00\x00"):
            cases.append((bad, 4096, 0, -1, b""))
        packed = raw_deflate(b"a\0second\0")
        cases.append((packed, 8, 1, -1, b""))  # terminator exceeds total-output limit
        cases.append((packed, 20, 2, -1, b""))  # selected record absent
        cases.append((raw_deflate(b"q" * 4096 + b"\0"), 5000, 0, -1, b""))
        for size in range(len(packed)):
            cases.append((packed[:size], 20, 2, -1, b""))
        definitions, calls = [], []
        for index, (packed, limit, record, status, expected) in enumerate(cases):
            definitions.append(f"const unsigned char p{index}[]={array(packed or b'x')};")
            definitions.append(f"const unsigned char e{index}[]={array(expected + bytes([0]))};")
            calls.append(f"check(p{index},{len(packed)},{limit},{record},{status},e{index},{len(expected)});")
        body = r"""
#include <helpers/ota/tinf/tinf.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
struct Guard { unsigned char before[32]; unsigned char window[4096]; unsigned char after[32]; };
void check(const unsigned char* source,unsigned size,unsigned limit,unsigned record,int expected,
           const unsigned char* text,unsigned length) {
  Guard buffer; memset(&buffer,0xa5,sizeof(buffer)); unsigned actual=777;
  int result=tinf_uncompress_record(buffer.window,4096,&actual,source,size,limit,record);
  if(expected==0) { assert(result==TINF_OK);assert(actual==length);assert(!memcmp(buffer.window,text,length+1)); }
  else assert(result!=TINF_OK);
  for(auto c:buffer.before) assert(c==0xa5);
  for(auto c:buffer.after) assert(c==0xa5);
}
""" + "\n".join(definitions) + r"""
int main() {
""" + "\n".join(calls) + r"""
  Guard buffer; memset(&buffer,0xa5,sizeof(buffer)); unsigned length=777;
  const unsigned char sample[]={3,0};
  assert(tinf_uncompress_record(nullptr,4096,&length,sample,2,4096,0)!=TINF_OK);
  assert(tinf_uncompress_record(buffer.window,2048,&length,sample,2,4096,0)!=TINF_OK);
  assert(tinf_uncompress_record(buffer.window,4096,nullptr,sample,2,4096,0)!=TINF_OK);
  assert(tinf_uncompress_record(buffer.window,4096,&length,nullptr,2,4096,0)!=TINF_OK);
  assert(tinf_uncompress_record(buffer.window,4096,&length,sample,2,0,0)!=TINF_OK);
  uint32_t seed=0x84a123f1; unsigned char input[512];
  for(unsigned pass=0;pass<10000;++pass) {
    auto next=[&]() { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; };
    const unsigned size=next()%sizeof(input);
    for(unsigned i=0;i<size;++i) input[i]=next();
    length=777;
    int result=tinf_uncompress_record(buffer.window,4096,&length,input,size,16384,next()%12);
    if(result==TINF_OK) { assert(length<4096);assert(buffer.window[length]==0); }
    for(auto c:buffer.before) assert(c==0xa5);
    for(auto c:buffer.after) assert(c==0xa5);
  }
}
"""
        self.run_binary(self.compile(body, "malformed"))

    def test_literal_capacity_and_lookup_rejects_invalid_metadata(self):
        # Generated input keeps every reference and the original 4095-byte
        # literal capacity, including corpora with no profitable dictionary.
        import sys
        path = str(ROOT / "tools/bible")
        sys.path.insert(0, path)
        try:
            spec = importlib.util.spec_from_file_location("reader_stream_fixture_packer", ROOT / "tools/bible/pack_reader_stream.py")
            packer = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(packer)
            document = json.loads(packer.blocks.SOURCE.read_text())
            document["verses"] = dict.fromkeys(packer.blocks.references(), None)
            document["verses"]["1:1"] = "q" * 4095
            rendered = packer.render(document)
        finally:
            sys.path.remove(path)
        header = self.directory / "literal.h"
        header.write_text(rendered.replace('#include "ReaderStreamLookup.h"', '#include <helpers/bible/ReaderStreamLookup.h>'), encoding="ascii")
        binary = self.compile(r"""
#include "literal.h"
#include <cassert>
#include <cstring>
int main() {
  using namespace mesh::bible;
  char scratch[kBlockSize];const char* text=nullptr;
  const auto& c=generated::readerCorpus;
  assert(lookup(c,{1,1},scratch,sizeof(scratch),text)==LookupResult::Found);
  assert(strlen(text)==4095);for(unsigned n=0;n<4095;++n) assert(text[n]=='q');
  assert(lookup(c,{1,2},scratch,sizeof(scratch),text)==LookupResult::Missing);assert(!text);
  assert(lookup(c,{0,1},scratch,sizeof(scratch),text)==LookupResult::Invalid);assert(!text);
  assert(lookup(c,{1,1},scratch,sizeof(scratch)-1,text)==LookupResult::Corrupt);assert(!text);
  assert(lookup(c,{1,1},nullptr,sizeof(scratch),text)==LookupResult::Corrupt);assert(!text);
  auto bad=c;bad.data=nullptr;assert(lookup(bad,{1,1},scratch,sizeof(scratch),text)==LookupResult::Corrupt);
  bad=c;bad.data_size=1;assert(lookup(bad,{1,1},scratch,sizeof(scratch),text)==LookupResult::Corrupt);
  bad=c;bad.encoded_size=1;assert(lookup(bad,{1,1},scratch,sizeof(scratch),text)==LookupResult::Corrupt);
  bad=c;--bad.verse_count;assert(lookup(bad,{1,1},scratch,sizeof(scratch),text)==LookupResult::Corrupt);
}
""", "literal")
        self.run_binary(binary)

    def test_independent_stream_header_bounds_and_boundary_records(self):
        binary = self.compile(r"""
#include <helpers/bible/ReaderStreamData.generated.h>
#include <cassert>
#include <cstring>
#include <vector>
int main() {
  using namespace mesh::bible;
  const auto& original=generated::readerCorpus;
  assert(original.chunk_count==3);
  struct { unsigned char before[32];char scratch[kBlockSize];unsigned char after[32]; } buffer;
  auto check=[&](const StreamTokenCorpus& corpus) {
    memset(&buffer,0xa5,sizeof(buffer));const char* text=nullptr;
    assert(lookup(corpus,{1,1},buffer.scratch,sizeof(buffer.scratch),text)==LookupResult::Corrupt);
    assert(!text);
    for(auto byte:buffer.before) assert(byte==0xa5);
    for(auto byte:buffer.after) assert(byte==0xa5);
  };
  auto bad=original;bad.chunk_count=4;check(bad);
  bad=original;bad.chunk_count=kVerseCount+1;check(bad);
  bad=original;bad.data_size=5;check(bad);
  bad=original;--bad.data_size;check(bad);
  bad=original;++bad.encoded_size;check(bad);
  std::vector<uint8_t> bytes(original.data,original.data+original.data_size);
  const size_t second=6+bytes[4]+(unsigned(bytes[5])<<8);
  for(auto field: {size_t(0),size_t(2),size_t(4),second,second+2,second+4}) {
    bytes.assign(original.data,original.data+original.data_size);
    bytes[field]=bytes[field+1]=0;
    if(field==0) bytes[0]=1; // First record must start at verse zero.
    bad=original;bad.data=bytes.data();check(bad);
  }
  bytes.assign(original.data,original.data+original.data_size);
  bytes[4]=bytes[5]=0xff;bad=original;bad.data=bytes.data();check(bad);
  bytes.assign(original.data,original.data+original.data_size);
  bytes.push_back(0);bad=original;bad.data=bytes.data();++bad.data_size;check(bad);
  // Independently inspect all stream boundaries and byte bounds. The complete
  // canonical comparison also checks every record immediately around them.
  unsigned offset=0,total=0,first=0;
  for(unsigned n=0;n<original.chunk_count;++n) {
    const auto* p=original.data+offset;
    unsigned current=p[0]+(unsigned(p[1])<<8),plain=p[2]+(unsigned(p[3])<<8);
    unsigned packed=p[4]+(unsigned(p[5])<<8);
    assert((n ? current>first : current==0) && plain<=17000 && packed>0);
    first=current;offset+=6+packed;total+=plain;
  }
  assert(offset==original.data_size && total==original.encoded_size);
}
""", "chunks")
        self.run_binary(binary)

    def test_dictionary_corruption_tokens_and_workspace_are_bounded(self):
        cases = []

        def fixture(encoded=b"\x80 suffix", words=b"Hello\0world\0", count=2,
                    *, plain_size=None, packed_words=None, expected=3, output=b""):
            plain=encoded+b"\0"+bytes(878)
            cases.append((raw_deflate(plain),len(plain),
                          raw_deflate(words) if packed_words is None else packed_words,
                          len(words) if plain_size is None else plain_size,count,expected,output))

        fixture(expected=0,output=b"Hello suffix")
        fixture(encoded=b"\x81",expected=0,output=b"world")
        fixture(encoded=b"\x82")  # token outside the declared vocabulary
        fixture(encoded=b"literal\x1f")
        fixture(encoded=b"literal\x7f")
        fixture(count=0)
        fixture(count=129)
        fixture(words=b"Hello\0\0")
        fixture(words=b"Hello\0wo\x7frld\0")
        fixture(words=b"Hello\0wo\x80rld\0")
        fixture(words=b"Hello\0world")
        fixture(words=b"Hello\0world\0extra\0")
        fixture(plain_size=1)
        fixture(plain_size=4096)
        fixture(plain_size=4097)
        fixture(packed_words=raw_deflate(b"Hello\0world\0")[:-1])
        fixture(packed_words=raw_deflate(b"Hello\0world\0")+b"x")
        fixture(packed_words=b"")
        fixture(encoded=b"\x80"*100,words=b"h"*64+b"\0",count=1)
        fixture(encoded=b"\x80"*4095,words=b"Hello\0",count=1)
        definitions,calls=[],[]
        for n,(data,size,words,word_plain,count,expected,output) in enumerate(cases):
            definitions.extend((f"const unsigned char p{n}[]={array(data)};",
                                f"const unsigned char w{n}[]={array(words or b'x')};",
                                f"const unsigned char e{n}[]={array(output+bytes([0]))};"))
            calls.append(f"check(p{n},sizeof(p{n}),{size},w{n},{len(words)},{word_plain},{count},{expected},e{n});")
        body=r"""
#include <helpers/bible/ReaderStreamLookup.h>
#include <cassert>
#include <cstring>
void check(const uint8_t* data,unsigned data_size,unsigned plain_size,const uint8_t* words,
           unsigned words_size,unsigned word_plain,unsigned count,unsigned expected,const unsigned char* output) {
  using namespace mesh::bible;
  StreamTokenCorpus c={"fixture","test",data,data_size,plain_size,words,
      uint16_t(words_size),uint16_t(word_plain),uint16_t(count),kVerseCount};
  struct { unsigned char before[32];char scratch[kBlockSize];unsigned char after[32]; } buffer;
  memset(&buffer,0xa5,sizeof(buffer));const char* text=nullptr;
  auto result=lookup(c,{1,1},buffer.scratch,sizeof(buffer.scratch),text);
  assert(unsigned(result)==expected);
  if(result==LookupResult::Found) assert(text && !strcmp(text,reinterpret_cast<const char*>(output)));
  else assert(!text);
  for(auto byte:buffer.before) assert(byte==0xa5);
  for(auto byte:buffer.after) assert(byte==0xa5);
}
"""+"\n".join(definitions)+"\nint main() {\n"+"\n".join(calls)+"\n}\n"
        self.run_binary(self.compile(body,"dictionary"))

    def test_worst_case_lookup_host_benchmark(self):
        # Keep timings informational: hosts vary, but report the CPU tradeoff
        # against the unchanged block decoder using identical host options.
        binary = self.compile(r"""
#include <helpers/bible/ReaderStreamData.generated.h>
#define generated old_generated
#include <helpers/bible/ReaderData.generated.h>
#undef generated
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
template<class Corpus> double bench(const Corpus& corpus) {
  using namespace mesh::bible;
  char scratch[kBlockSize];const char* text=nullptr;
  volatile unsigned sum=0;
  auto start=std::chrono::steady_clock::now();
  for(unsigned n=0;n<80;++n) {
    assert(lookup(corpus,{21,25},scratch,sizeof(scratch),text)==LookupResult::Found);
    sum+=strlen(text);
  }
  assert(sum>0);
  return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/80;
}
int main() { printf("Reader last verse host ms: block=%.4f stream=%.4f\n",
                     bench(mesh::bible::old_generated::readerCorpus),bench(mesh::bible::generated::readerCorpus)); }
""", "benchmark")
        print(self.run_binary(binary).decode().strip())


if __name__ == "__main__":
    unittest.main()
