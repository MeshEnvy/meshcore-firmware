These fixtures preserve Adafruit_VL53L0X 1.2.5's two tuning tables and the
complete `VL53L0X_load_tuning_settings` function, with their ST license notices.
The loader's hardware write function is replaced only in the native harness.

Source: https://github.com/adafruit/Adafruit_VL53L0X/tree/1.2.5/src

`test_nrf52_vl53l0x_const.py` compares every register write and internal tuning
parameter before and after const qualification, including hardware failures.
The production middleware fingerprints the five complete affected files and
compiles a private library copy. Unknown dependency revisions use their ordinary
source; the downloaded library is never modified.
