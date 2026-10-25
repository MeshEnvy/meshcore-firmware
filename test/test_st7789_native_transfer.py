"""Real ST7789 SPI flush at native widths >255 and non-eight-row heights."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeTransferTest(unittest.TestCase):
    def test_flush_covers_only_native_panel_rows(self):
        driver = (ROOT / "src/helpers/ui/ST7789Spi.h").read_text()
        flush = driver[driver.index("    void display(void)"):driver.index(" virtual void resetOrientation()")]
        source = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <vector>
using std::min; using std::max;
#define LOW 0
#define HIGH 1
#define rtos_malloc std::malloc
#define rtos_free std::free
void yield() {}
struct Bus {
  int transfers=0, expected_bytes=0;
  template<class T> void beginTransaction(T) {}
  void endTransaction() {}
  void transfer(void*,void*,int bytes) { assert(bytes==expected_bytes); ++transfers; }
};
class Panel {
 public:
  int displayWidth,displayHeight,_buffheight;
  uint16_t _RGB=0xffff, _lastRGB=0xffff;
  int _spiSettings=0;
  Bus bus; Bus* _spi=&bus;
  std::vector<uint8_t> front,back;
  uint8_t* buffer; uint8_t* buffer_back;
  std::vector<int> rows;
  Panel(int w,int h):displayWidth(w),displayHeight(h),_buffheight((h+7)/8),
    front(w*_buffheight,0xff),back(w*_buffheight,0),
    buffer(front.data()),buffer_back(back.data()),rows(h,0) {}
  void set_CS(int) {}
  void setAddrWindow(int x,int y,int w,int h) {
    assert(x==0 && w==displayWidth && h==1 && y>=0 && y<displayHeight);
    ++rows[y]; bus.expected_bytes=w*2;
  }
'''
        source += flush + r'''
};
int main() {
  for(auto size:{std::make_pair(240,135),{320,170},{320,240}}) {
    Panel p(size.first,size.second); p.display();
    assert(p.bus.transfers==p.displayHeight);
    for(int row:p.rows) assert(row==1);
#ifdef OLEDDISPLAY_DOUBLE_BUFFER
    p.display(); assert(p.bus.transfers==p.displayHeight);
    // A tint-only night transition must flush even if every monochrome
    // framebuffer bit is unchanged, then return to normal dirty suppression.
    p._RGB=0xf800; p.display();
    assert(p.bus.transfers==2*p.displayHeight);
    p.display(); assert(p.bus.transfers==2*p.displayHeight);
    p._RGB=0xffff; p.display();
    assert(p.bus.transfers==3*p.displayHeight);
#endif
  }
}
'''
        with tempfile.TemporaryDirectory(prefix="meshcore-native-spi-") as temp:
            for double in (False, True):
                flags = ["-DOLEDDISPLAY_DOUBLE_BUFFER"] if double else []
                if os.name != "nt":
                    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                binary = Path(temp) / "flush.exe"
                result = subprocess.run(["c++", "-std=c++11", *flags, "-x", "c++", "-", "-o", str(binary)],
                                        input=source, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
