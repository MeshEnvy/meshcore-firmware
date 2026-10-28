"""Exercise the real bounded Cardputer SD layout parser with hostile input."""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
VARIANT = ROOT / "variants/m5stack_cardputer_adv"
FS = r'''
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <map>
#define FILE_READ "r"
struct FileData {
  std::string bytes, replacement;
  size_t at=0, reads=0, closes=0, size_override=0, fail_after=~size_t(0);
  bool stalled=false, fail_seek=false;
};
class File {
  std::shared_ptr<FileData> d;
public:
  File(std::shared_ptr<FileData> value={}) : d(value) {}
  explicit operator bool() const { return bool(d); }
  size_t size() { return d->size_override ? d->size_override : d->bytes.size(); }
  size_t position() { return d->at; }
  int read() {
    ++d->reads;
    if(d->at>=d->bytes.size() || d->reads>d->fail_after) return -1;
    const uint8_t value=uint8_t(d->bytes[d->at]);
    if(!d->stalled) ++d->at;
    return value;
  }
  bool seek(size_t at) {
    if(d->fail_seek) return false;
    if(!d->replacement.empty()) { d->bytes=d->replacement; d->replacement.clear(); }
    d->at=at; return true;
  }
  void close() { ++d->closes; }
};
class FS {
public:
  std::map<std::string,std::shared_ptr<FileData>> files;
  bool exists(const char* path) { return files.count(path); }
  File open(const char* path,const char*,bool) {
    auto found=files.find(path);
    if(found==files.end())return {};
    found->second->at=found->second->reads=0;
    return File(found->second);
  }
  std::shared_ptr<FileData> put(const char* path,const std::string& bytes) {
    auto data=std::make_shared<FileData>();data->bytes=bytes;files[path]=data;return data;
  }
};
'''
HARNESS = r'''
#include <cassert>
#include <cstring>
#include <new>
#include <string>
#include <cstdlib>
#include "CardputerLayout.h"
static int fail_allocation=-1, allocations=0;
static size_t largest_allocation=0;
void* operator new(size_t size,const std::nothrow_t&) noexcept {
  largest_allocation=std::max(largest_allocation,size);
  if(allocations++==fail_allocation)return nullptr;
  return ::operator new(size);
}
void* operator new[](size_t size,const std::nothrow_t&) noexcept {
  largest_allocation=std::max(largest_allocation,size);
  if(allocations++==fail_allocation)return nullptr;
  return ::operator new[](size);
}
using namespace Keyboard;
using namespace Keyboard::Layout;
static Event key(unsigned id=1,unsigned mask=0) {
  Event e;e.key=static_cast<Key>(id);e.modifiers.fn=mask&1;e.modifiers.ctrl=mask&2;
  e.modifiers.shift=mask&4;e.modifiers.opt=mask&8;e.modifiers.alt=mask&16;return e;
}
static void expect(CardputerLayout& layout,const char* text,unsigned id=1,unsigned mask=0) {
  auto e=key(id,mask);assert(!std::strcmp(layout.lookup(e),text));
}
static std::string layer(unsigned mask) {
  std::string text="L ";
  if(!mask)text+='B';
  if(mask&1)text+='F';
  if(mask&2)text+='C';
  if(mask&4)text+='S';
  if(mask&8)text+='O';
  if(mask&16)text+='A';
  return text+"\n";
}
static void healthy(FS& fs,CardputerLayout& layout) {
  fs.put(KB_LAYOUT_MAIN_FILE,"N en\r\nL B\r\nK 01=x\r\nK 56=\xe2\x82\xac");
  layout.begin(fs);expect(layout,"x");expect(layout,"\xe2\x82\xac",56);
  assert(!std::strcmp(layout.getMainLayoutCode(),"en"));
}
int main(int argc,char** argv) {
  assert(argc==2);
  static_assert(sizeof(LayoutLayer)==225,"update the complete layer RAM bound");
  static_assert(CardputerLayout::MAX_LAYOUT_LAYERS==32,"five modifier bits");
  FS fs;CardputerLayout layout;
  const std::string mode=argv[1];
  if(mode=="normal") {
    healthy(fs,layout);
    fs.put(KB_LAYOUT_ALT_FILE,"N xx\nL B\nK 01=y\nL S\nK 01=Y\n");
    layout.begin(fs);layout.switchLayout();expect(layout,"y");expect(layout,"Y",1,4);
    fs.put(KB_LAYOUT_ALT_FILE,"N yy\nL B\nK 01=z\n");
    layout.begin(fs);expect(layout,"z");assert(!std::strcmp(layout.getCurrentCode(),"yy"));
    layout.switchLayout();expect(layout,"x");
  } else if(mode=="all-layers") {
    std::string text="N en\n";
    for(unsigned mask=0;mask<32;++mask) {
      text+=layer(mask);
      for(unsigned id=1;id<=56;++id) {
        char row[16];std::snprintf(row,sizeof(row),"K %02u=%c\n",id,'a'+mask%26);text+=row;
      }
    }
    assert(text.size()<CardputerLayout::MAX_LAYOUT_FILE_BYTES);
    fs.put(KB_LAYOUT_MAIN_FILE,text);fs.put(KB_LAYOUT_ALT_FILE,text);layout.begin(fs);
    for(unsigned alternate=0;alternate<2;++alternate) {
      for(unsigned mask=0;mask<32;++mask) {
        const char wanted[]={char('a'+mask%26),0};
        for(unsigned id=1;id<=56;++id)expect(layout,wanted,id,mask);
      }
      layout.switchLayout();
    }
    assert(largest_allocation==32*sizeof(LayoutLayer));
    assert(allocations==4);
  } else if(mode=="duplicates") {
    std::string text="N en\n";
    for(unsigned i=0;i<1000;++i)text+="L B\nK 01=z\n";
    fs.put(KB_LAYOUT_MAIN_FILE,text);layout.begin(fs);expect(layout,"z");
    assert(largest_allocation==sizeof(LayoutLayer) && allocations==2);
  } else if(mode=="allocation-failure") {
    healthy(fs,layout);
    for(int fail=0;fail<2;++fail) {
      allocations=0;fail_allocation=fail;
      fs.put(KB_LAYOUT_MAIN_FILE,"N zz\nL B\nK 01=q\n");layout.begin(fs);
      expect(layout,"x");assert(!std::strcmp(layout.getMainLayoutCode(),"en"));
    }
    fail_allocation=-1;healthy(fs,layout);
  } else if(mode=="bad-input") {
    healthy(fs,layout);
    for(const auto& text : {std::string("N xx\nL B\nK 01=z\n#")+std::string(81,'!'),
                            std::string("L B\nK 01=z\0bad",15),
                            std::string("#")+std::string(32768,'!'),std::string("N xx\n")}) {
      fs.put(KB_LAYOUT_MAIN_FILE,text);layout.begin(fs);expect(layout,"x");
    }
    // Both file and line limits are inclusive, including a final line without LF.
    std::string maximum="N en\nL B\nK 01=v\n";
    while(maximum.size()+81<=32768)maximum+="#"+std::string(79,'!')+"\n";
    maximum.append(32768-maximum.size(),'#');
    assert(maximum.size()==32768);
    fs.put(KB_LAYOUT_MAIN_FILE,maximum);layout.begin(fs);expect(layout,"v");
  } else if(mode=="broken-file") {
    healthy(fs,layout);
    for(unsigned failure=0;failure<5;++failure) {
      auto data=fs.put(KB_LAYOUT_MAIN_FILE,"N xx\nL B\nK 01=z\n");
      if(failure==0)data->stalled=true;
      if(failure==1)data->fail_after=3;
      if(failure==2)data->fail_seek=true;
      if(failure==3)data->fail_after=data->bytes.size()+3;
      if(failure==4)data->replacement="L B\nL F\nK 01=z\n";
      layout.begin(fs);expect(layout,"x");assert(data->closes==1);
      assert(data->reads<=2*32768);
    }
  } else { assert(false); }
}
'''


class CardputerLayoutTests(unittest.TestCase):
    def test_bounded_production_parser_preserves_valid_layout_on_every_failure(self):
        compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "a native C++ compiler is required")
        with tempfile.TemporaryDirectory(prefix="cardputer-layout-") as directory:
            work = Path(directory)
            (work / "FS.h").write_text(FS, encoding="ascii")
            (work / "Adafruit_TCA8418.h").write_text(
                "#pragma once\nclass Adafruit_TCA8418 {};\n", encoding="ascii")
            (work / "main.cpp").write_text(HARNESS, encoding="ascii")
            binary = work / ("layout.exe" if os.name == "nt" else "layout")
            flags = [] if os.name == "nt" else ["-fsanitize=address,undefined",
                "-fno-sanitize-recover=all", "-fno-pie", "-no-pie"]
            built = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                *flags, "-I", str(work), "-I", str(VARIANT), "-I", str(VARIANT / "hw"),
                str(work / "main.cpp"), str(VARIANT / "hw/CardputerLayout.cpp"), "-o", str(binary)],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            for scenario in ("normal", "all-layers", "duplicates", "allocation-failure",
                             "bad-input", "broken-file"):
                with self.subTest(scenario=scenario):
                    tested = subprocess.run([str(binary), scenario], capture_output=True,
                                            text=True, timeout=10)
                    self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)


if __name__ == "__main__":
    unittest.main()
