#!/usr/bin/env python3
"""Prove compact nRF glyph metrics retain every original field and pixel."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
FONT_FILES=("Pixel5FontData.h","Squeezed6FontData.h")

HARNESS=r'''
#include <helpers/ui/Pixel5FontData.h>
#include <helpers/ui/Squeezed6FontData.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#if defined(NRF52_PLATFORM)
static_assert(sizeof(mesh::ui::SmallFontGlyph)==3,"Cortex-M4 glyph packing");
#else
static_assert(sizeof(mesh::ui::SmallFontGlyph)==8,"Existing non-nRF glyph ABI");
#endif
void show(const mesh::ui::SmallFontGlyph* glyphs,const uint8_t* bitmap) {
  for(unsigned n=0;n<95;++n) {
    const auto& g=glyphs[n];
    printf("%u,%u,%u,%u,%d,%d:",unsigned(g.bitmap_offset),unsigned(g.width),
           unsigned(g.height),unsigned(g.advance),int(g.x_offset),int(g.y_offset));
    for(unsigned row=0;row<g.height;++row) {
      for(unsigned col=0;col<g.width;++col) {
        const unsigned bit=row*g.width+col;
        putchar(bitmap[g.bitmap_offset+bit/8]&(0x80>>(bit%8))?'1':'0');
      }
    }
    putchar('\n');
  }
}
int main() {
  show(mesh::ui::pixel5Glyphs,mesh::ui::pixel5Bitmaps);
  show(mesh::ui::squeezed6Glyphs,mesh::ui::squeezed6Bitmaps);
}
'''


class SmallFontMetricsTest(unittest.TestCase):
    def source_metrics(self):
        result=[]
        for filename in FONT_FILES:
            source=(ROOT/"src/helpers/ui"/filename).read_text()
            rows=[tuple(map(int,row.split(",")))
                  for row in re.findall(r"\{([\d, \-]+)\}",source)]
            self.assertEqual(len(rows),95)
            for offset,width,height,advance,x,y in rows:
                self.assertLessEqual(offset,255)
                self.assertTrue(0<=width<=7 and 0<=height<=7 and 0<=advance<=7)
                self.assertTrue(-4<=x<=3 and -8<=y<=7)
            result+=rows
        return result

    def test_all_190_metrics_and_pixels_match_original_layout(self):
        expected=self.source_metrics()
        with tempfile.TemporaryDirectory(prefix="mesh-small-font-") as folder:
            folder=Path(folder);source=folder/"main.cpp";source.write_text(HARNESS)
            output=[]
            for nrf in (False,True):
                binary=folder/("nrf" if nrf else "original")
                result=subprocess.run(["g++","-std=c++11","-O1","-g","-Wall","-Wextra",
                    "-Werror=overflow","-Werror=conversion","-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all","-fno-pie","-no-pie","-I"+str(ROOT/"src"),
                    *(["-DNRF52_PLATFORM=1"] if nrf else []),str(source),"-o",str(binary)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                run=subprocess.run([str(binary)],capture_output=True,text=True)
                self.assertEqual(run.returncode,0,run.stdout+run.stderr)
                output.append(run.stdout)
                actual=[tuple(map(int,row.split(":")[0].split(","))) for row in run.stdout.splitlines()]
                self.assertEqual(actual,expected)
            self.assertEqual(output[0],output[1])

    def test_actual_cortex_m4_layout_and_flash_saving(self):
        compiler=shutil.which("arm-none-eabi-g++")
        if not compiler:
            path=Path.home()/".platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-g++"
            if path.is_file():compiler=str(path)
        if not compiler:self.skipTest("Cortex-M4 compiler unavailable")
        compiler=Path(compiler)
        nm=compiler.with_name("arm-none-eabi-nm")
        self.source_metrics()
        with tempfile.TemporaryDirectory(prefix="mesh-small-font-arm-") as folder:
            folder=Path(folder);source=folder/"main.cpp"
            source.write_text('#include <helpers/ui/Pixel5FontData.h>\n#include <helpers/ui/Squeezed6FontData.h>\n'
                'extern "C" const void* keep(unsigned n) { return n?mesh::ui::pixel5Glyphs:mesh::ui::squeezed6Glyphs; }\n')
            sizes=[]
            for nrf in (False,True):
                obj=folder/("nrf.o" if nrf else "original.o")
                result=subprocess.run([str(compiler),"-std=gnu++11","-mcpu=cortex-m4","-mthumb","-Oz",
                    "-fno-exceptions","-fno-rtti","-Werror=overflow","-Werror=conversion",
                    "-I"+str(ROOT/"src"),*(["-DNRF52_PLATFORM=1"] if nrf else []),
                    "-c",str(source),"-o",str(obj)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                symbols=subprocess.check_output([str(nm),"-S",str(obj)],text=True)
                rows=[int(line.split()[1],16) for line in symbols.splitlines()
                      if "pixel5Glyphs" in line or "squeezed6Glyphs" in line]
                self.assertEqual(sorted(rows),[95*(3 if nrf else 8)]*2)
                sizes.append(sum(rows))
            self.assertEqual(sizes[0]-sizes[1],950)


if __name__=="__main__":unittest.main()
