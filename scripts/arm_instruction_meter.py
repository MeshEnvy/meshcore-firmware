"""Measure every ARM instruction with a C callback, or the identical Python one.

The optional native callback avoids billions of crossings into Python while
keeping instruction/stack coverage. It uses the reviewed private handle API of
pinned Unicorn 2.1.4. Other versions or hosts without a C compiler use Python.
No Unicorn headers, linker development package, or network access are needed.
"""
import ctypes
from functools import lru_cache
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unicorn
from unicorn import UC_HOOK_CODE
from unicorn import arm_const as arm


class _NativeState(ctypes.Structure):
    _fields_ = [('read_register', ctypes.c_void_p), ('sp_register', ctypes.c_int),
                ('error', ctypes.c_int), ('minimum_sp', ctypes.c_uint64),
                ('instructions', ctypes.c_uint64)]


@lru_cache(maxsize=1)
def _native_library():
    compiler = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if not compiler:
        raise RuntimeError('no host C compiler; use the Python instruction callback')
    directory = tempfile.TemporaryDirectory(prefix='mesh-arm-instruction-meter-')
    library = Path(directory.name) / 'meter.so'
    result = subprocess.run([compiler, '-std=c11', '-O2', '-shared', '-fPIC',
        str(Path(__file__).with_suffix('.c')), '-o', str(library)],
        capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError('native callback compilation failed: ' + result.stderr)
    loaded = ctypes.CDLL(str(library))
    loaded.mesh_arm_measurement_size.restype = ctypes.c_size_t
    if loaded.mesh_arm_measurement_size() != ctypes.sizeof(_NativeState):
        raise RuntimeError('native/Python measurement layout mismatch')
    return loaded, directory


class ArmInstructionMeter:
    def __init__(self, machine, prefer_native=True):
        self.backend = 'python'
        self._state = {'minimum_sp': 0, 'instructions': 0}
        self._native = None
        if prefer_native:
            try:
                self._install_native(machine)
                self.backend = 'native'
                return
            except (RuntimeError, OSError, AttributeError) as error:
                print('ARM meter Python fallback: ' + str(error), file=sys.stderr)
        self.handle = machine.hook_add(UC_HOOK_CODE, self._measure_python)

    def _install_native(self, machine):
        if unicorn.__version__ != '2.1.4':
            raise RuntimeError('native callback requires reviewed Unicorn 2.1.4 bindings')
        from unicorn.unicorn_py3 import unicorn as bindings
        library, directory = _native_library()
        state = _NativeState(ctypes.cast(bindings.uclib.uc_reg_read, ctypes.c_void_p),
                             arm.UC_ARM_REG_SP, 0, 0, 0)
        callback = bindings.HOOK_CODE_CFUNC(('mesh_arm_measure_instruction', library))
        handle = bindings.uc_hook_h()
        result = bindings.uclib.uc_hook_add(machine._uch, ctypes.byref(handle), UC_HOOK_CODE,
            callback, ctypes.byref(state), ctypes.c_uint64(1), ctypes.c_uint64(0))
        if result:
            raise RuntimeError('native Unicorn hook registration failed: ' + str(result))
        # Keep the C callback, its context, loaded code and temporary file alive
        # for at least the lifetime of the registered machine hook.
        machine._callbacks[handle.value] = (callback, state, library, directory)
        self._native, self.handle = state, handle.value

    def _measure_python(self, machine, address, size, data):
        self._state['minimum_sp'] = min(self._state['minimum_sp'], machine.reg_read(arm.UC_ARM_REG_SP))
        self._state['instructions'] += 1

    def reset(self, top):
        if self._native is not None:
            self._native.minimum_sp, self._native.instructions, self._native.error = top, 0, 0
        else:
            self._state.update(minimum_sp=top, instructions=0)

    def snapshot(self):
        if self._native is not None:
            if self._native.error:
                raise RuntimeError('Unicorn SP read failed: ' + str(self._native.error))
            return {'minimum_sp': self._native.minimum_sp,
                    'instructions': self._native.instructions}
        return dict(self._state)
