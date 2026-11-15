# Adding an LV2 plugin

A bundled LV2 plugin is built by the native prebuild (`cmake/`, driven by
`build.sh`), staged into the app and the Play asset packs, and discovered at
runtime by lilv. Adding one takes five things; the rest is derived from them.

| What | Where |
|------|-------|
| The source, as a submodule (plus local patches) | `3rd_party/<Name>`, `3rd_party/patches/<Name>/NNNN-*.patch` |
| A target file that builds and stages it | `cmake/targets/<name>.cmake` |
| One registry line | `cmake/plugins.cmake` |
| A description for the plugin browser | `plugin_descriptions.json` |
| A credit | `3rd_party/README.md` |

Derived from the registry, with nothing else to edit: which target files
CMake includes, `all_plugins`, what the metadata step waits for and which
bundles it describes, which Play asset pack the libraries ship in, and the
staging into `app/src/` (`build.sh stage`). `build.sh clean` removes all of
`build/` except `build/gradle`, so a new build directory needs no clean entry.

## 1. Submodule and patches

```bash
git submodule add <repo-url> 3rd_party/<Name>
```

Local changes go in numbered `patch -p1` diffs under
`3rd_party/patches/<Name>/`. `build.sh` (`./build.sh patch`) applies them in
name order through `scripts/apply-patches.sh`: a patch that is already in the
tree is skipped, and one that neither applies nor is applied stops the build.

## 2. The target file: `cmake/targets/<name>.cmake`

`<name>` is the registry name (lower case by convention). The file must:

1. stage the plugin's TTLs into `${ASSETS_DIR}/<Bundle>.lv2/`,
2. build the DSP `.so` (and the UI `.so`, if any),
3. stage the libraries into `${JNILIBS_DIR}` as `lib<binary>.so`, where
   `<binary>` is the name in the TTL's `lv2:binary` / `ui:binary`,
4. define a target `<name>_done` that depends on all of it.

`JNILIBS_DIR` is set per plugin by `cmake/CMakeLists.txt`, to
`build/prebuild/stage/<PACK>`; don't set it yourself. CMake fails at configure
time if `<name>_done` is missing.

Pick the closest existing file as a template:

| Plugin shape | Template | Helpers it uses |
|--------------|----------|-----------------|
| brummer10-style DSP + xputty/cairo UI | `collisiondrive.cmake`, `fatfrog.cmake` | `lv2_stage_bundle`, `brummer_add_plugin` |
| Custom DSP build (extra libraries) + xputty UI | `neuralrack.cmake`, `impulseloader.cmake` | `lv2_stage_bundle`, `brummer_setup_xputty`, `brummer_add_ui_target`, `lv2_sync_dsp_ui` |
| Several plugins from one repository | `modamptk.cmake` | as above, once per plugin |
| Upstream CMake project (ExternalProject) | `nam.cmake`, `aidax.cmake` | `lv2_sync_to_jnilibs` on the build's `.so` |
| DSP only, generated TTLs | `doubletracker.cmake` | hand-written sync |

The helpers live in `cmake/modules/`:

- `lv2_stage_bundle(<bundle_dir> TTL_DIR <dir> [TTLS <file>...] [MOD_DIR <dir>])`
  (`LV2PluginUtils.cmake`) copies `manifest.ttl` and the listed TTLs, which
  must exist, plus MOD extras when present: MOD's `manifest.ttl` (it adds
  `rdfs:seeAlso <modgui.ttl>`), `modgui.ttl` and `modgui/`.
- `lv2_set_dsp_properties(<target> <output_name> <build_dir>)` sets the DSP
  output name and the usual compile flags; `lv2_strip_and_save_debug` keeps an
  unstripped `<name>_debug.so` next to the build output.
- `lv2_sync_dsp_ui(NAME ... OUTPUT_NAME ... BUILD_DIR ... DSP_TARGET ... UI_TARGET ...)`
  copies and strips `<OUTPUT_NAME>.so` and `<OUTPUT_NAME>_ui.so` into
  `${JNILIBS_DIR}` and defines `<NAME>_done`.
- `lv2_sync_to_jnilibs(<target> <source_dir> <depends>)` does the same for
  every DSP `.so` under a directory (UI and `_debug` files are skipped by
  default); define `<name>_done` on top of it.
- `brummer_add_plugin(...)` (`BrummerPlugin.cmake`) is the whole DSP + UI +
  sync chain for the brummer10 layout.

Things that bite:

- DSP sources want `${LV2_COMPAT_DIR}` (redirects for old `lv2plug.in/ns/...`
  include paths) and `${LV2_INCLUDE}` (the LV2 headers) on their include path.
  `generate_lv2_compat_headers(<dir>)` makes a per-plugin copy if needed.
- `lv2_set_dsp_properties` compiles with `-std=c++17`. For a C DSP, add
  `target_compile_options(<target> PRIVATE -std=c11)` after it (the last flag
  wins).
- A cairo/X11 UI links `grc_x11_ui_libs` (cairo, pixman, png, the X11 client
  libraries, the MIT-SHM stub) and needs `add_dependencies(<ui> x11_sysroot)`.
- Name the libraries uniquely: two staged libraries with the same file name
  stop `build.sh`.

## 3. The registry line: `cmake/plugins.cmake`

```cmake
grc_plugin(<name> PACK <gx|neural|brummer> AUTHOR <author> BUNDLES <Bundle>...)
```

- `PACK`: the Play asset pack the libraries go to in the `playstore` flavor
  (the `full` flavor ships every plugin in its jniLibs). Use the one that
  matches the plugin's family; it only decides which install-time pack holds
  the files.
- `BUNDLES`: the plugin's own bundles, `assets/lv2/<Bundle>.lv2`, which the
  metadata step lists in `plugin_metadata.json` with `AUTHOR` as their author,
  so the plugin browser groups them. A declared bundle that isn't staged is a
  build error.

## 4. Description and credit

- `plugin_descriptions.json`: add `"<doap:name>": "<one line>"`, keyed by the
  plugin's `doap:name` from its TTL.
- `3rd_party/README.md`: add the submodule to its author's table.

## 5. Build and check

```bash
./build.sh native <name>_done     # configure (the new files trigger it), build, stage
./gradlew assembleFullDebug       # or ./run.sh debug
```

`./build.sh native <target>...` builds only what you name and then restages;
run a plain `./build.sh` (or `./build.sh native`) once before committing to
build everything and regenerate the metadata.

These fail loudly instead of shipping a broken plugin:

- a TTL `lv2_stage_bundle` expects is missing;
- a bundle declared in `BUNDLES` wasn't staged (metadata step);
- two staged libraries share a file name (`build.sh` staging);
- a library left in `app/src/main/jniLibs` that `config/core-libs.txt` doesn't
  allow, or a missing native prebuild (Gradle's `verifyNativeInputs<Variant>`);
- empty asset packs when bundling the `playstore` flavor (`verifyAssetPacks`).

## How it fits together at runtime

- `build.sh stage` mirrors `build/prebuild/stage/`: `core/` to
  `app/src/main/jniLibs` (the base APK), every pack to `app/src/full/jniLibs`
  (the `full` flavor), and each pack to its `<pack>/src/main/assets/plugins`
  (the `playstore` flavor, listed in `app/src/playstore/assets/plugin_libs.txt`
  for `PluginAssetExtractor`).
- The TTLs ship as assets under `assets/lv2/`. The app extracts the files
  listed in `assets/lv2_files.txt` (which `build.sh stage` regenerates), so a
  bundle missing from it is invisible.
- At startup `LV2PluginFactory::rewriteManifestPaths()` points each
  manifest's `lv2:binary <x.so>` at `lib<x.so>` in the app's native library
  dir (`full`), or in the directory the asset packs were extracted to
  (`playstore`).

## Importing a plugin at runtime (full flavor)

A user can install an LV2 bundle on the device without rebuilding the app. This
is the `full` flavor only (`BuildConfig.LV2_IMPORT_ENABLED`): it needs to
`dlopen` a library from `filesDir`, which `targetSdk` 28 still allows and
`playstore`'s `targetSdk` 35 forbids (W^X).

The browser's overflow menu offers **Import LV2 (ZIP)**, **Import LV2 (folder)**
and **Manage imported**. Imported bundles are copied to
`filesDir/lv2_user/<uuid>/` (never executed from the SAF location), recorded in
`filesDir/lv2_user/registry.json` (written to a staging file and renamed over
the old one, so a crash mid-import cannot truncate the bookkeeping), and loaded
by the LV2 factory, which scans `filesDir/lv2_user` in addition to the extracted
assets. No restart is needed:
`nativeRefreshPluginRegistry()` → `PluginRegistry::refreshAll()` →
`LV2PluginFactory::refresh()` loads only the bundles it has not seen yet into
the existing `LilvWorld` (a live plugin holds pointers into it, so the world is
never rebuilt).

What the importer refuses before the engine ever sees the files
(`Lv2BundleImporter`):

- no `manifest.ttl` anywhere in the tree, or no `.so`;
- any `.so` that is not ELF64 little-endian AArch64 (a plugin built for x86-64
  or ARM32 fails on `dlopen` with a far worse message);
- paths that escape the destination directory (`..`, absolute), more than 4096
  files, or more than 512 MB unpacked.

Plugin binaries are made read-only after the copy. A ZIP whose `manifest.ttl`
sits at the archive root is wrapped in `<picked name>.lv2/`.

The copy is staged: it lands in `lv2_user/.staging-<uuid>/` and is only renamed
to `lv2_user/<uuid>/` once validation passes, so a failed import leaves nothing
behind. The native scanner skips every dot directory, not just `.` and `..`, so a
bundle that is still being copied — or one the importer is about to roll back —
is never loaded into the `LilvWorld` (`tests/lv2/TestLV2BundleScanner.cpp`,
`SkipsStagingDirectoriesOfInFlightImports`).

Known limits, by design:

- Deleting an imported bundle deletes its files and marks its registry entry
  `removed`. The URI stays in the browser list filter and in the `LilvWorld`
  until the process restarts; lilv cannot unload a single bundle safely.
- `ui:binary`/`guiext:binary` inside an imported bundle is rewritten to an
  absolute path inside the bundle when it is not in the app's native library
  directory, so bundled UIs keep working.

### Which bundle a plugin came from

The engine reports `PluginInfo.bundlePath` for every LV2 plugin: the absolute
directory lilv loaded it from (the same thing `lilv_plugin_get_bundle_uri()`
returns, without the trailing slash). `modguiBasePath` is **not** usable for
this: it is only set for bundles that ship a `modgui.ttl` with an
`iconTemplate` (`LV2Utils.cpp`).

`Lv2BundleAttribution` uses that path to map a plugin URI back to the entry in
`registry.json`: a plugin belongs to bundle `id` when its bundle lives at
`filesDir/lv2_user/<id>` or below (a plain `<id>/Foo.lv2/`, or the wrapped
directory of a bare bundle). The browser reconciles that mapping on every
enumeration (`Lv2BundleRegistry.reconcileUris`), so a bundle imported while the
engine was not enumerating is attributed on the next refresh instead of keeping
an empty URI list forever. The "URIs that appeared during the rescan" diff is
only kept as a fallback message at import time.

The path travels through the whole native chain (lilv bundle URI →
`pluginBundlePath()` → `PluginInfo` → JNI → `PluginInfo.kt`) with nothing to
enforce it at the far end: an empty `bundlePath` does not fail, it silently
un-attributes every imported plugin. `tests/lv2/TestLV2PluginFactoryBundlePath.cpp`
therefore drives the real `LV2PluginFactory` against the vendored lilv and asserts
the reported path (the Android headers that translation unit includes are stubbed
under `tests/lv2/stubs/`, which is all the host build needs).

### Threading: the shared LilvWorld has one lock

Importing while the rack is running means loading new bundles into a world that
live plugins and the browser are reading. lilv/sord have no thread safety at
all, and there is no read-only call in them:

- `lilv_world_load_bundle()` inserts into `world->model` (the writer);
- `lilv_new_uri()` — used by `getInfo()` for every port class and by
  `lilv_plugin_instantiate()` for `rdf:type` — interns the URI in the sord world
  (`lilv/src/node.c:69`, `sord_new_uri` → hash insert);
- `lilv_plugin_instantiate()` runs `lilv_plugin_load_if_necessary()` and
  `lilv_lib_open()`, which looks the `.so` handle up in `world->libs` and bumps
  its refcount, inserting it on first use (`lilv/src/lib.c:48-50` and `:124`).

So a read/write lock with shared readers would still let two concurrent plugin
creations race on the same intern trees. Everything that touches the world
serializes on one mutex, `guitarrackcraft::lv2world::lock()`
(`app/src/main/cpp/plugin/lv2/LV2WorldLock.h`), and the lock order is:

1. `lv2world::lock()`;
2. `PluginRegistry` cache mutex;
3. `PluginChain` chain mutex.

Covered by the lock: `PluginRegistry::initializeAll()`/`refreshAll()`,
`nativeAddPluginToRack`, `nativeGetRackPluginInfo`, `nativeStartEngine`,
`nativeProcessFile`, `nativeSaveChainState`, and the `getInfo()` calls of
`PluginUIManager::createPluginUI()` and `LV2PluginUI::instantiate()` (the UI is
instantiated on the display's pluginUI thread while the JNI thread waits, so the
lock sits in each body, never around the hand-off).

Not covered, on purpose: the audio callback (it only runs `lilv_instance_run`
on instances already in the chain), `nativeStopEngine` and
`nativeRestorePluginState` (instance-level calls only),
`nativeGetAvailablePlugins` (reads the registry cache), and
`nativeCreatePluginUI` itself (see above).

#### The one uncovered writer: plugin teardown

Removing a plugin from the rack is the remaining path that mutates the world
without the lock. It is **pre-existing**, not introduced here: before this
feature the world was written by `LV2PluginFactory::initialize()` (once, at
startup, before anything else runs) and by this teardown path — which never
needed a lock because `initialize()` had already finished. Importing bundles at
runtime adds a *third* writer, and that is what makes the two collide. The path
is

```
nativeRemovePluginFromRack        jni/NativeBridge.cpp:716-730
  └─ PluginChain::removePlugin    plugin/PluginChain.cpp:99-117
      └─ enqueueTeardown          plugin/PluginChain.cpp:119-137
          └─ teardownLoop         plugin/PluginChain.cpp:139-165
              └─ ~LV2Plugin       plugin/lv2/LV2Plugin.cpp:228-237
                  └─ lilv_instance_free   lilv/src/instance.c:113-124
                      └─ lilv_lib_close  lilv/src/lib.c:140-154
```

`removePlugin()` detaches the instance from the chain and hands the object to a
dedicated teardown thread, which drops it (the local `std::unique_ptr` in
`teardownLoop`, `PluginChain.cpp:142`, dies at the end of the iteration,
`PluginChain.cpp:163`). The destruction therefore happens on that thread, and
`lilv_instance_free()` ends in `lilv_lib_close()`, which **writes** `world->libs`
when the last reference goes away: `--lib->refs`, then `zix_tree_remove()` on the
world's library tree, `lilv_node_free(lib->uri)` and a plain `free(lib)`
(`lilv/src/lib.c:142-153`). No lock is held there.

That races the same structures the lock protects: a concurrent
`refreshAll()`/`LV2PluginFactory::refresh()` loading a bundle
(`zix_tree_insert()` into `world->libs`) or a concurrent `nativeAddPluginToRack`
opening the same library (`lilv_lib_open()`, which inserts or bumps the refcount
in the same tree) can walk a tree that the teardown thread is removing a node
from. The window is narrow and needs an import or an add *while* a plugin is
being removed — the rack UI makes that possible (delete a plugin, then confirm
the import dialog).

The feature narrows it but cannot close it. Plugin *creation* and every rescan
now serialize on `lv2world::lock()`, so the two writers that remain are creation
(matched) and teardown (unmatched). Closing it needs a lock around
`lilv_instance_free()` in `~LV2Plugin` (`LV2Plugin.cpp:233`) — the destructor
runs on the teardown thread, never in the audio callback, so that acquisition
cannot enter the real-time path and does not change latency behaviour. That edit
belongs to the plugin/chain layer (`LV2Plugin.cpp`, plus `PluginChain.cpp` if the
teardown ordering is to be asserted), not to this feature:
**follow-up, tracked as "hold `lv2world::lock()` around `lilv_instance_free()` in
the teardown path"**. `LV2Plugin::deactivate()`'s re-instantiation path
(`LV2Plugin.cpp:282`) frees the previous instance the same unlocked way and would
want the same treatment.

Tests: `tests/plugin/TestPluginRegistryWorldLock.cpp` pins that a rebuild holds
the lock for its whole duration and waits for the JNI paths, and
`tests/lv2/TestLV2WorldLock.cpp` runs a real import against a real plugin
creation. Built with `-DGRC_HOST_TSAN=ON` the latter is the race detector for
lilv itself: without the lock TSan reports a data race in
`lilv_lib_open()` (`lilv/src/lib.c`) between the importing thread inserting the
`.so` into `world->libs` and the creating thread walking the same table — both
tests are TSan-clean with the lock.

Known leak, accepted: `LV2PluginFactory::refresh()` re-runs
`loadSpecificationsAndClasses()` whenever it loaded a new bundle, and
`lilv_world_load_plugin_classes()` re-parses every LV2 plugin class into the
world without freeing the previous copy: for each class it mallocs a fresh
`LilvPluginClass` and inserts it into `world->plugin_classes`
(`lilv/src/world.c:1037`), and on a repeat run the tree rejects the duplicate
(`lilv/src/pluginclass.c:27` never looks at the tree first) — so the class and
its three nodes are orphaned. The leak is bounded by "number of plugin classes ×
number of imports that loaded something" — kilobytes per import, independent of
the bundle's size — and it is not fixable from this repository: lilv is a
prebuilt library here (no meson in the build) and the only alternative,
recreating the world, would invalidate every live `LV2Plugin`.
