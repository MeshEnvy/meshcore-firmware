"""Keep the reviewed VL53L0X tuning tables in nRF52 flash.

The vendor tuning loader only reads these bytes and copies register writes
through a local buffer. Compile a private dependency copy with const inputs;
leave its public caller-owned tuning-buffer setter/getter unchanged. Unknown
library versions retain their ordinary implementation and RAM requirements.
"""

import hashlib
from pathlib import Path
import shutil


FILES = {
    "vl53l0x_tuning.h": "0e0331b7df1084d183248355f75d835b3446200b22f71765b5c63a470ff01c63",
    "vl53l0x_interrupt_threshold_settings.h": "9e5e6c00af66497d03d6c6a13a269521c12b33c90455efcd3b8494487e1780c2",
    "vl53l0x_api_core.h": "948de4a0363339facc58c4e1bc5781b954b99cca1c13a4a105eb3969aeba8653",
    "core/src/vl53l0x_api_core.cpp": "d31da82edbad0f7fd89106340f7a006b61cd2e5b7c353460fb394978ce55953b",
    "core/src/vl53l0x_api.cpp": "6cfa0784600bd797e372b511761f4be10e97264b55d5c7269dd41ba607b8edb7",
}
REPLACEMENTS = {
    "vl53l0x_tuning.h": ("uint8_t DefaultTuningSettings[]", "const uint8_t DefaultTuningSettings[]"),
    "vl53l0x_interrupt_threshold_settings.h": (
        "uint8_t InterruptThresholdSettings[]", "const uint8_t InterruptThresholdSettings[]"),
    "vl53l0x_api_core.h": ("uint8_t* pTuningSettingBuffer);", "const uint8_t* pTuningSettingBuffer);"),
    "core/src/vl53l0x_api_core.cpp": (
        "uint8_t* pTuningSettingBuffer) {", "const uint8_t* pTuningSettingBuffer) {"),
    "core/src/vl53l0x_api.cpp": ("uint8_t* pTuningSettingBuffer;", "const uint8_t* pTuningSettingBuffer;"),
}


def digest(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def patched_files(source_root):
    result = {}
    for name, expected in FILES.items():
        text = (source_root / name).read_text(encoding="utf-8").replace("\r\n", "\n")
        old, new = REPLACEMENTS[name]
        if digest(text) != expected or text.count(old) != 1:
            raise RuntimeError("changed reviewed VL53L0X dependency: " + name)
        result[name] = text.replace(old, new, 1)
    return result


def write_if_changed(path, content):
    if not path.exists() or path.read_text(encoding="utf-8") != content:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")


def replace_vl53_source(build_env, node):
    source = Path(node.srcnode().get_abspath())
    source_root = next((parent for parent in source.parents
                        if parent.name == "src" and parent.parent.name == "Adafruit_VL53L0X"), None)
    if source_root is None or source.suffix != ".cpp":
        return node
    try:
        patches = patched_files(source_root)
    except (OSError, UnicodeError, RuntimeError) as error:
        print("nRF52 VL53L0X const tables: ordinary source retained (" + str(error) + ")")
        return node
    destination = Path(build_env.subst("$BUILD_DIR")) / "readonly-nrf52-vl53l0x" / "src"
    # Keep every quoted sibling/relative include inside the same private copy.
    # All compilation units therefore see the same const loader declaration.
    for path in source_root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(source_root)
        output = destination / relative
        if relative.as_posix() in patches:
            write_if_changed(output, patches[relative.as_posix()])
        elif not output.exists() or output.read_bytes() != path.read_bytes():
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, output)
    return build_env.File(str(destination / source.relative_to(source_root)))


def install(build_env):
    build_env.AddBuildMiddleware(replace_vl53_source, "*Adafruit_VL53L0X*/*.cpp")
