# Windows caps a process command line at 32,767 characters, and the GCC
# driver hands cc1plus the fully expanded argument list. ESP32 Arduino
# compiles pass ~200 framework include paths; with the Cardputer UI libraries
# the command no longer fits ("CreateProcess: No such file or directory").
# Shorten it the way Arduino-ESP32's own platform.txt does: name the shared
# framework include root once with -iprefix and add each directory under it
# with -iwithprefixbefore (same search position as -I, order preserved).
Import("env")

import os
import sys
from collections import Counter

if sys.platform == "win32":
    PREFIX_DEPTH = 3  # e.g. <packages>/framework-arduinoespressif32-libs/esp32s3/include

    def _prefix_root(packages_dir, path):
        rel = os.path.relpath(path, packages_dir)
        if rel.startswith(".."):
            return None
        parts = rel.replace("\\", "/").split("/")
        if len(parts) <= PREFIX_DEPTH:
            return None
        return os.path.join(packages_dir, *parts[:PREFIX_DEPTH])

    def _short_incflags(env):
        packages_dir = os.path.normcase(os.path.abspath(env.subst("$PROJECT_PACKAGES_DIR")))
        paths = []
        for entry in env.Flatten(env.get("CPPPATH", [])):
            path = env.subst(str(entry)) if not hasattr(entry, "abspath") else entry.abspath
            if path:
                paths.append(path)

        roots = Counter()
        for path in paths:
            root = _prefix_root(packages_dir, os.path.normcase(os.path.abspath(path)))
            if root:
                roots[root] += 1
        if not roots:
            return ["-I" + p for p in paths]
        prefix = roots.most_common(1)[0][0]

        flags = ["-iprefix", prefix.replace("\\", "/") + "/"]
        for path in paths:
            full = os.path.normcase(os.path.abspath(path))
            if full.startswith(prefix + os.sep):
                flags += ["-iwithprefixbefore", full[len(prefix) + 1:].replace("\\", "/")]
            else:
                flags.append("-I" + path)
        return flags

    env["_cardputer_short_incflags"] = _short_incflags
    env["_CPPINCFLAGS"] = "$( ${_cardputer_short_incflags(__env__)} $)"
