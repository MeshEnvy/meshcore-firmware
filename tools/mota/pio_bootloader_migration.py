"""Optional, explicit RAK4631 bootloader migration resource hook."""
Import('env')

import os
from pathlib import Path
import sys

legacy = os.environ.get('MC_MIGRATION_LEGACY_BOOTLOADER')
candidate = os.environ.get('MC_MIGRATION_CANDIDATE_BOOTLOADER')
if bool(legacy) != bool(candidate):
    raise RuntimeError('migration requires both exact legacy and candidate bootloader images')
if legacy:
    if env['PIOENV'] != 'RAK_4631_repeater':
        raise RuntimeError('migration qualification is restricted to RAK_4631_repeater')
    sys.path.insert(0, str(Path(env['PROJECT_DIR']) / 'tools' / 'mota'))
    from bootloader_migration_resource import emit_resource
    generated = Path(env.subst('$BUILD_DIR')) / 'bootloader-migration-resource'
    emit_resource(legacy, candidate, generated)
    env.Append(CPPDEFINES=[('OTA_BOOTLOADER_MIGRATION', 1)])
    env.Append(CPPPATH=[str(generated)])
    env.BuildSources(str(generated / 'objects'), str(generated), src_filter=['+<*.cpp>'])
