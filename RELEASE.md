# Releasing Firmware

For 1.18.0.1, build the complete qualified local release from a
clean, committed checkout. The builder runs one PlatformIO process at a time:

```bash
python3 scripts/build_local_release.py \
  --firmware-version v1.18.0.1-halo-keymind-cascade-dev --pio-jobs 8 --resume
```

Package its completed directory for stable publication:

```bash
python3 scripts/package_cascade_release.py \
  --input .releases/v1.18.0.1-halo-keymind-cascade-dev-SOURCE8 \
  --output /path/to/publication-stage --local-release --stable \
  --version 1.18.0.1 --commit FULL_SOURCE_COMMIT
```

The packager rechecks the local checksum inventory, qualified firmware,
Full/Reduced nRF52 pairs, separate exact-board OTAFIX 2.4.12 embedded variants,
standalone bootloader files, and every exact ESP32 migration package. It keeps
migration-only application identities inside their migration ZIPs. It does
not upload anything. Review `release-plan.json`, generate the picker controls
as described in [the picker guide](docs/firmware_picker.md), and generate
both Mesh America catalogs from the same staged asset manifests:

```bash
python3 scripts/generate_mesh_america_release.py \
  --stage /path/to/publication-stage \
  --catalog mesh-america/keymind-cascade-v1.16.0-provider.json \
  --output /path/to/new-provider.json
```

Both active provider URLs use the same binaries. Preserve their stable
filenames when updating them. Create supporting releases first and publish
the Companion family root last, marking only that root as GitHub Latest.
Upload each group's complete asset inventory and verify the public asset
names, sizes and checksums before switching the catalogs. Regenerate the
downloadable picker from the same family and update its checksum on every
page. Documentation-only follow-up commits do not move firmware tags.

Include the [1.18.0.1 release notes](docs/releases/1.18.0.1.md) and
[embedded bootloader guide](docs/nrf52_embedded_bootloader.md) in the
publication review. The historical
[1.17.1.8 WiFi upgrade guide](docs/upgrading_1.17.1.8_to_1.17.1.9.md) retains
its measured starting-version limits. Record the completed inventory only
after packaging passes. Keep the firmware
source separate from a later documentation-only commit, and retain the
measured hardware scope and old-updater completion limitation.

For the USA Cascade 1.17.1.5 matrix, use
[the option 3 release instructions](docs/old-releases/1.17.1.5.md).
Package the qualified outputs with `scripts/package_cascade_release.py`.
Local staging does not publish or push anything. Include the
[feature switches by role](docs/role_feature_switches.md),
[Full Companion guide](docs/full_companion_features.md), and
[USB web console](https://flasher.meshcore.io/console) in each role's notes.
The five 1.17.1.5 pages are development prereleases.

## Legacy tag-triggered GitHub Actions

The repository also has older workflows matching `companion-*`, `repeater-*`,
and `room-server-*` tags (for example `repeater-v1.0.0`). They invoke the shared
`firmware-builder.yml`, build their own target set, and request a draft release.
They are separate from the qualified option 3 matrix and local release staging.

The older broad `repeater-*` trigger also matched `repeater-room-*` Cascade tags.
For 1.17.1.5 that workflow added its own artifacts and changed the title of the
already-published Repeater/Room Server page. Do not assume a published Cascade
page is untouched while that workflow is running. Check workflow completion,
release title/body, and the final asset inventory after publication. Preserve
existing assets when making a documentation-only release edit.
The current trigger explicitly excludes `repeater-room-*`.

## Release notes

Write one public release page at `docs/releases/VERSION.md`. Make it the
starting point from the documentation home page and every GitHub download page.
Use the same format for each revision:

1. **Status and purpose:** stable or testing, version, region, and one sentence
   explaining the user benefit.
2. **New:** a short list of actual additions since the previous stable release.
   Say who each change helps; link setup instructions beside the feature.
3. **Fixed:** name affected boards and the behavior corrected.
4. **Choose a download:** picker first, then a short table of role-specific
   GitHub pages. Explain Full/Reduced sensors and included bootloaders once.
5. **Before updating:** only required actions, changed defaults and compatibility
   limits. Link the installation guides instead of copying them.
6. **Tested and limitations:** separate build checks from physical tests. Name
   important remaining limitations and link source-specific evidence.

Compare the actual previous release's source and notes. A long commit list can
include old work imported by merges; do not present it all as new. Do not relist
existing features as additions. Keep tuning commands, build counts, memory
layouts and implementation details in the reference guides or test reports.

The main GitHub release should summarize New, Fixed and Tested in a few bullets
and link the public release page. Other download pages should lead with their
purpose and link the same notes. Use readable link names such as "ESP32
Repeater, Room Server and Sensor" rather than internal keys like `full-profiles`.
Label split pages "part 1 of 3" and explain that they contain the same version,
not different feature levels. The picker should find the exact board for users.

Keep required bootloader guidance linked to the exact board/storage
[OTAFIX release](https://github.com/mikecarper/Adafruit_nRF52_Bootloader_OTAFIX/releases/tag/v0.11.0-OTAFIX2.4.12).
A notes-only correction may update public titles, bodies and documentation.
Preserve published tags, binaries, attached build notes and checksum files;
verify release status, Latest and the asset inventory remain unchanged.
