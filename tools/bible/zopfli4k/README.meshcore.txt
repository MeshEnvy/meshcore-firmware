Host-only Google Zopfli with a 4 KiB match window
================================================

Sources: Google Zopfli, bundled in the pinned python-zopfli 0.4.3 source release.
https://github.com/google/zopfli
https://pypi.org/project/zopfli/0.4.3/
Source archive: zopfli-0.4.3.tar.gz
SHA-256: d3a50f91a13cea9bafe025de8fd87a005eb26de02a4f0c193127ddbf23ac8ebe
License: Apache-2.0, preserved in LICENSE and the upstream source headers.

MeshCore changes: util.h sets ZOPFLI_WINDOW_SIZE to 4096 instead of 32768.
reader_pack.c is a small MeshCore command-line adapter. All other C/header
files are unchanged from that archive. This tool runs only on the build host
when regenerating Reader data. It is never compiled into firmware.

The 4 KiB limit is required by the existing on-device Reader scratch buffer.
The ordinary python-zopfli package has no configurable match-window API.
The generator compiles these pinned sources with a host C compiler and uses
1000 optimization iterations. No network access is required. Ordinary firmware
builds use the committed generated header and do not invoke this tool.
