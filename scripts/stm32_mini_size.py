"""Retain the measured Mini Repeater size policy in the final LTO link."""

Import("env")

# PlatformIO's build_flags apply these to compilation, but do not pass them
# to the LTO driver automatically. Keep this list matched by the profile test.
# No compiler/ABI, flash boundary or capacity check is overridden here.
if env["PIOENV"] == "wio-e5-mini_repeater":
    import importlib.util
    from pathlib import Path

    # Reuse the qualified RAK CLI's equivalent libc reply copies. Restrict
    # this flag to CommonCLI.cpp; radio, framework and other objects retain
    # the existing optimizer recipe.
    source = Path(__file__).resolve().with_name("nrf52_flash_trim.py")
    spec = importlib.util.spec_from_file_location("mesh_mini_cli_replies", source)
    replies = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(replies)
    env.AddBuildMiddleware(replies.compile_shared_cli_replies, "*CommonCLI.cpp")

    env.Append(LINKFLAGS=[
        "-fno-caller-saves",
        "-fno-tree-dominator-opts",
        "-fno-reorder-blocks",
        "-fno-reorder-functions",
        "-fno-tree-switch-conversion",
    ])
