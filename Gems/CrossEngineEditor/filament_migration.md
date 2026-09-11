# Filament Backend Migration

> **Status**: Draft (2026-09-08, kimi独立审 + glm独立审 合并终稿)
> **Filament**: `I:\filament @ rc/1.76.0` (d15885c49, MSVC 2022 verified)
> **Related**: Plan.md (§A6/§B12), rbfx_migration.md (§4), godot_migration.md, material_migration.md (§7.4)

---

## §0 TL;DR

**Filament is a PBR rendering engine, not a game engine.** `Scene` is a flat container (`Scene.h:39-41`), no scene serialization, no material instance file format, no editor. CEE provides all editing value; Filament provides rendering + glTF I/O + material runtime.

**Three source-verified facts determine the design**:

1. **`add_subdirectory` path.** Filament 1.76 on Windows **requires MSVC, rejects clang-cl** (`CMakeLists.txt:364-368`). Same toolchain as O3DE/CEE.
2. **HWND direct embed.** `Engine::createSwapChain(void* nativeWindow, flags)` (`Engine.h:912`); WGL does `GetDC` + `SetPixelFormat` internally. Multi-window precedent: `samples/multiple_windows.cpp`.
3. **gltfio has no writer, but cgltf does.** `libs/gltfio` grep "Writer" = zero; vendored cgltf 1.15 includes `cgltf_write.h` (`third_party/cgltf/cgltf_write.h:42-43`).

**Design**: `--backend filament`. Scene truth = live cgltf document (never call `releaseSourceData()`); save = `cgltf_write_file`. Material: precompiled `.filamat` type + value JSON + curated schema + `hasParameter` cross-validation (material_migration.md §7.4). Preview = offscreen `RenderTarget` + async `readPixels` — native three-state, zero stall (best of three backends).

**Prerequisite**: Execute `rbfx_migration.md` §4 **E2 contract slimming** before third backend. Filament achieves **stub rate 0**.

**Cost**: E2 prereq ≈0.5w + build ≈1w + scene ≈2w + material ≈1.5w + polish ≈0.5w ≈ **5.5–6 person-weeks**.

---

## §1 Hard Rules (Plan §A6 C1–C7)

| Rule | Requirement | Landing |
|---|---|---|
| C1 Atom baseline | Filament unrelated to Atom, zero risk | §4 |
| C2 Removability | `CEE_ENABLE_FILAMENT` default ON, missing → NullBackend; engine headers don't escape `Backends/` | §4/§13 |
| C3 Reuse first | 100% existing framework; **zero new widgets** | §6/§10 |
| C4 Contract + no engine types in data plane | Filament types stay in `Backends/`; E2 slimming applied to all backends | §7 |
| C5 Cost explicit | Rendering 3 primitives + editing 7 pure-virtual + material 8+2 + 1 curated schema; **stub target 0** | §7/§10 |
| C6 Switch semantics | `--backend filament`; coexists at build time, mutually exclusive at runtime | §4 |
| C7 No O3DE internals | Reuse `cee_mark_third_party_subtree_cached` | §4 |

---

## §2 Verified Fact Inventory

### 2.1 Build & Toolchain

| # | Fact | Anchor |
|---|---|---|
| B1 | Windows requires MSVC, **rejects clang-cl** (FATAL_ERROR) | Root `CMakeLists.txt:364-368` |
| B2 | CI = VS 2022 generator + vcvars64 | `build/windows/build-github.bat:53,113-114` |
| B3 | **CRT defaults to static `/MT`** (`USE_STATIC_CRT=ON`) — must FORCE OFF for `/MD` | Root `CMakeLists.txt:263-284` |
| B4 | No CMake package export → `add_subdirectory` only | rbfx same pattern |
| B5 | `gltfio_core` **excludes filamat** (no glslang) — editor can link core only | `libs/gltfio/CMakeLists.txt:185-199` |
| B6 | Desktop builds always include tools tree (matc/uberz/cmgen/resgen) | Root `CMakeLists.txt:954-1010` |
| B7 | `-fno-rtti` default — no `dynamic_cast` on engine types | Root `CMakeLists.txt:59` |
| B8 | License Apache-2.0; `third_party/environments` CC0 | `LICENSE` |
| B9 | Version `rc/1.76.0`; `MATERIAL_VERSION = 75` (version-lock) | `MaterialEnums.h:32` |

### 2.2 Viewport & Frame Loop

| # | Fact | Anchor |
|---|---|---|
| E1 | `createSwapChain(void* nativeWindow, flags)`; Windows = HWND | `Engine.h:912`; `SwapChain.h:49-57` |
| E2 | WGL accepts external HWND: self `GetDC`/`SetPixelFormat`; GL context on filament's own hidden window | `PlatformWGL.cpp:226-245,104-138` |
| E3 | Windows backend: **OpenGL/WGL (default) or Vulkan (OFF)**; **no DirectX** | Root `CMakeLists.txt:609-613` |
| E4 | resize = **destroy + recreate SwapChain** (no resize API) | `FilamentApp2.cpp:316-326,928-936` |
| E5 | Frame: `beginFrame(sc)` → `render(view)` → `endFrame()`; also `skipFrame()` | `Renderer.h:365,497,591,302` |
| E6 | `Engine::create()` needs no surface (WGL builds dummy window) → engine init in `Initialize`, swapchain in `OnSurfaceCreated`; destroy: `flushAndWait` + `Engine::destroy` (thread-safe) | `Engine.h:158-175,711-712`; `PlatformWGL.cpp:104-105` |
| E7 | Offscreen: `RenderTarget::Builder` + `Renderer::readPixels` **async callback** (fires after multiple frames); COLOR needs `TextureUsage::BLIT_SRC` | `RenderTarget.h:39-141`; `Renderer.h:559-566,651-652` |
| E8 | Multi swapchain/multi View per Engine, official precedent | `samples/multiple_windows.cpp:113-141` |

### 2.3 Scene Model (ECS) & glTF

| # | Fact | Anchor |
|---|---|---|
| S1 | `Scene` = flat container of Renderable/Light, **NOT a scene graph**; hierarchy in `TransformManager` | `Scene.h:39-41`; `TransformManager.h:203-336` |
| S2 | `Entity` = 32-bit with generation counter (`FILAMENT_GENERATION_SHIFT=17`) — **not persistable**; stable keys only via glTF node name/index | `Entity.h:29-38,92,97-101` |
| S3 | `NameComponentManager` + `AssetConfiguration.names` for glTF node names | `NameComponentManager.h:33-50`; `AssetLoader.h:62-86` |
| S4 | gltfio: `AssetLoader::createAsset(bytes)` → `ResourceLoader::loadResources` → `scene->addEntities()`; `getSourceAsset()` returns cgltf (weak ref, lost after `releaseSourceData()`) | `AssetLoader.h:164-236`; `FilamentAsset.h:57-242` |
| S5 | **gltfio has zero writers**; only write path = vendored cgltf `cgltf_write` (C99, single-header) | `third_party/cgltf/cgltf_write.h:42-43` |
| S6 | `cgltf_write` boundary: GLB mode writes JSON + `data->bin`; external buffers/images **not written** | `cgltf_write.h:17-22`; `cgltf.h:1226-1238,1485-1492` |
| S7 | Coordinate system: **Y-up, right-handed, camera looks -Z** (matches glTF, zero axis conversion) | `Camera.h:67-72` |
| S8 | Lights: SUN/DIRECTIONAL/POINT/FOCUSED_SPOT/SPOT; **full runtime setters** (setPosition/setDirection/setColor/setIntensity/setFalloff/setSpotLightCone) | `LightManager.h:833-957`; `EngineEnums.h:107-112` |
| S9 | AABB: `RenderableManager::getAxisAlignedBoundingBox` returns **object-space/local AABB** | `RenderableManager.h:650-666` |
| S10 | GPU picking: `View::pick(x, y, handler, callback)` — async, returns entity + depth + fragCoords | `View.h:847-961` |
| S11 | No built-in primitive library; samples use hand-written quads | `libs/geometry/`; `samples/lightbulb.cpp` |

### 2.4 Material System

| # | Fact | Anchor |
|---|---|---|
| M1 | `.mat` = JSONish (material + fragment blocks; params have `{type,name}` only, **no default values**) | `Materials.md.html:952-1040` |
| M2 | Runtime load = `Material::Builder().package(payload, size).build(engine)`; deduplicating cache | `Material.h:133-207` |
| M3 | Reflection = `getParameters(ParameterInfo*)` + `hasParameter`; **no UI metadata, no color flags** — curated schema + cross-validation is the only path | `Material.h:97-116,364-395` |
| M4 | Instance: `setParameter` (scalars/vectors/arrays/textures + `RgbType::LINEAR|sRGB`) + runtime state overrides (`setDepthCulling/setDoubleSided/...`) | `MaterialInstance.h:140-257,462-529` |
| M5 | `.filamat` version lock: `MATERIAL_VERSION = 75`; matc and engine lib must be same-source build | `MaterialEnums.h:32` |
| M6 | In-process `.mat` compilation **unconditionally depends on glslang** | `filamat/src/MaterialBuilder.cpp:211-214`; `filamat/CMakeLists.txt:89-148` |
| M7 | JIT precedent exists (`JitShaderProvider` uses `MaterialBuilder`) — v2 path; v1 avoids M6 dependency weight | `gltfio/src/JitShaderProvider.cpp:384-408` |
| M8 | Ubershader: 19 precompiled variants, matc+uberz build-time | `gltfio/CMakeLists.txt:89-154` |
| M9 | glTF PBR parameter full set (curated schema vocabulary source) | `gltfio/materials/base.mat.in:1-58` |

### 2.5 Misconceptions Corrected

| Common Belief | Source Fact |
|---|---|
| "Filament requires Clang on Windows" | **1.76 reversed**: MSVC required, clang-cl `FATAL_ERROR` (B1) |
| "gltfio can write glTF" | **Zero writers** (S5); only cgltf_write in tree |
| "matc/material builder is lightweight in-process" | **glslang unconditionally linked** (M6); subprocess or precompile only |

---

## §3 Three-Backend Positioning

| | rbfx | Godot | **Filament** |
|---|---|---|---|
| Nature | Game engine | Game engine | **Rendering engine** (ECS + render, no serialization/reflection metadata) |
| Build integration | `add_subdirectory` (MSVC) | Runtime LoadLibrary | **`add_subdirectory` (MSVC)** — rbfx pattern |
| Viewport embed | HWND direct | `--wid` + reparent | **`createSwapChain(HWND)` direct** — rbfx pattern |
| Scene truth | Engine `Scene` + `.xml` | `SceneTree` + `.tscn` | **Live cgltf document + `.gltf/.glb`** |
| Property reflection | `GetAttributes` | `get_property_list()` | **None** — curated table |
| Material model | Engine `Material` + `.xml` | `BaseMaterial3D` + `.tres` | **Precompiled `.filamat` type + value JSON** |
| Preview readback | Sync + wait stall | Sync (async v1.5) | **Async native** — three-state, zero stall |
| Picking | Triangle (RaycastNode) | AABB (v2 triangle) | **AABB** (v2: `View::pick`) |
| Render API | D3D12 (Diligent) | Vulkan/D3D12 | **OpenGL/WGL** (Vulkan one-line switch) |

---

## §4 Build Integration (`External/CMakeLists.txt`) — revised 2026-09-10: PREBUILT consumption

**The original add_subdirectory design does not survive contact with this O3DE build** (collision-audit gate, R1): Filament's flat target namespace (`math`, `gtest`, `meshoptimizer`, `glslang`, `spirv-*`, `OSDependent`) collides with targets that already exist in the tree — O3DE's own FetchContent `gtest`/`meshoptimizer`, and rbfx's vendored embree `math` + glslang family. There is no knob that avoids the collisions (tools are hardwired through `IS_HOST_PLATFORM`).

**Landed design**: Filament is built **standalone once** (own CMake tree, e.g. `G:/o3de/build/filament-standalone`) and consumed through an INTERFACE facade that globs the prebuilt `.lib`s + tool exes — the out-of-tree pattern, same family as the Godot scons build. This keeps every backend in one editor binary (C6 intact) without patching Filament.

Bootstrap (one-time, MSVC; `--unset=MSYSTEM` because Filament rejects MSYS2 on sight and Git Bash sets it):
```cmake
cmake -E env --unset=MSYSTEM cmake -S I:/filament -B <prebuilt> -G "Visual Studio 18 2026"
  -DUSE_STATIC_CRT=OFF            # B3: /MD match Qt/O3DE
  -DFILAMENT_SKIP_SAMPLES=ON -DFILAMENT_SKIP_SDL2=ON
  -DFILAMENT_SUPPORTS_VULKAN=OFF  # v1 = WGL
  -DFILAMENT_ENABLE_MATDBG=OFF
  -DFILAMENT_SHORTEN_MSVC_COMPILATION=OFF  # else Debug libs bake _ITERATOR_DEBUG_LEVEL=0
                                           # (root CMakeLists.txt:328) -> LNK2038 against
                                           # the editor's IDL-2 objects
cmake --build <prebuilt> --config Debug --target
  filament gltfio_core uberarchive stb matc resgen filaflat filabridge math utils
  backend geometry image imageio ktxreader bluegl
```

Facade essentials (see `External/CMakeLists.txt` Filament section for the full probe logic):
- links the static-lib closure: `filament backend bluegl math utils filaflat filabridge gltfio_core uberarchive stb ktxreader ktx2 dracodec meshoptimizer webpdecoder uberzlib basisu image imageio geometry …` (probed by name under `<prebuilt>/*/<Config>/`, FATAL_ERROR listing what is missing)
- includes: `filament/include`, `filament/backend/include`, `libs/gltfio/include`, `libs/utils/include`, `third_party/cgltf`, `third_party/stb`
- **uberarchive is a runtime requirement, not an optimization** (review F3): `AssetConfiguration.materials` must be a valid `MaterialProvider` — `FAssetLoader` dereferences it in its constructor (`AssetLoader.cpp:263`). The ubershader path needs the `uberarchive` target linked and `createUbershaderProvider(engine, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE)` (`MaterialProvider.h:220`, gltfio_test.cpp:241), destroy via `destroyMaterials()` + `delete`; textures decode through `createStbProvider`/`createKtx2Provider` (+ `destroyStbProvider`... they are plain-deleted after the ResourceLoader dies).
- `.filamat` pipeline: CEE `.mat` sources → `matc` custom command → `resgen -qcx <dir> -p cee_filamat` → `cee_filamat.c/.h` compiled into `cee_filamat_resources` — matc and the engine libs come from the same checkout + build, so the version lock (M5) holds.
- `ResourceLoader` needs `ResourceConfiguration.gltfPath` set to the scene file (`ResourceLoader.cpp:953`) or external `.bin`/image URIs never resolve; the field is deprecated → C4996 → wrap in `AZ_PUSH_DISABLE_WARNING`.
- **Config pairing** (facade resolves per editor config with generator expressions): editor **Debug ↔ filament Debug**, editor **Profile/Release ↔ filament RelWithDebInfo** — "Profile" is an O3DE-only config name; Filament has the CMake standard four, and RelWithDebInfo (/O2 + /MD + /Zi) is the one whose CRT and semantics match O3DE Profile. Build BOTH configs in the prebuilt tree.
- Version bumps: re-run the bootstrap for both configs; matc and engine libs stay same-source (M5).

---

## §5 Viewport Embedding & Frame Loop

**Lifecycle**: `Initialize` → `Engine::create()` (WGL builds dummy window, E6) → `OnSurfaceCreated(hwnd,w,h)` → `createSwapChain(hwnd)` with `CONFIG_HAS_STENCIL_BUFFER` → each frame: `beginFrame(sc)` → `render(view)` → `endFrame()`.

Five Filament-specific considerations:

1. **`OnSurfaceResized` = destroy + recreate swapchain** (E4 official pattern, Filament has no resize API).
2. **`OnSurfaceAboutToBeDestroyed`** → `destroy(swapChain)` + `flushAndWait` (E6) then return — aligns with rbfx/Godot synchronous release discipline.
3. **Frame drive**: `beginFrame/render/endFrame` in **`EndOverlayFrame`** (same position as rbfx `RunFrame`, `RbfxBackend.cpp:591-602` precedent) — overlay vertices and scene present same frame; `Tick()` empty. Hidden pause = don't call `EndOverlayFrame` (`skipFrame` available, E5).
4. **Pixel format exclusive**: WGL does `SetPixelFormat` on the viewport HWND (E2, once per window) — CEE viewport is bare `QWindow` (no Qt GL config), Plan §B2 already forbids `QOpenGLWidget/QRhi`; satisfied by design.
5. **Camera**: `BeginOverlayFrame` receives worldToView → `EngineSpace::Filament` conversion → `Camera::lookAt(eye, eye+fwd, up)` + `setProjection(fov, aspect, near, far)`. **Never memcpy an AZ matrix into a `math::mat4f`** (row/column-major + basis + depth convention all differ — review F2): the landing is the rbfx/Godot pattern — inverse view → pos/rot → forward-fix quaternion (`Rx(90°)·Ry(180°)`, identical to Godot's since the spaces are identical) → `EngineTransformConverter::TransformToEngine` → `lookAt`; fovY recovered from `viewToClip[1][1]`; physical exposure set once (`setExposure(16, 1/125, 100)`, sample defaults) or glTF scenes render near-black.

---

## §6 Scene Model & Persistence (Key Design Decision)

### 6.1 Live cgltf Document = Scene Single Truth

```
[Disk]       .gltf/.glb
                │ AssetLoader::createAsset (S4)     ← never call releaseSourceData()
[Doc Layer]  cgltf_data (node tree / TRS / lights / material refs / extras…) + CEE-side created-node table
             ← SINGLE TRUTH; all editor writes land here first
                │ Mirror: each glTF node → AZ::Entity (EngineNodeComponent.nodeHandle = node index)
[Render]     Filament entities (TransformManager/RenderableManager/LightManager)
                │ Save = cgltf_write_file (S6)
[Disk]       .gltf (JSON; external .bin/images URI untouched) / .glb (JSON + data->bin re-embedded)
```

**Rationale** (each independently load-bearing):
- Plan §B4: "engine-native scene = single truth". Filament has no scene file — glTF **is** the native format (official viewer only loads glTF).
- S2: Filament `Entity` carries generation, not persistable — glTF node name/index is the only stable key.
- **No matrix decomposition needed**: `TransformManager` stores mat4; cgltf nodes store structured TRS — gizmo edit writes TRS fields, render side composes mat4 from TRS.
- **Unknown fields lossless**: skins/animations/cameras/extras/extensions survive in cgltf document, `cgltf_write` writes them back verbatim.
- **Index/material references intact**: mesh/accessor/buffer stay in original cgltf structures.

**Mirror direction**: `SyncToEditor` walks **cgltf default scene in pre-order** (document graph, not `Scene::forEach` which is unordered flat list S1; not TransformManager which has synthetic root with no glTF node S4). `FinishSync` wires parent-child via `TransformBus::SetParent`.

**Entity round-trip**: `utils::Entity` has a **private constructor** (`Entity.h:94`); public reconstruction uses `Entity::import(int32_t)` (`Entity.h:75-79`). For EngineNodeComponent, store `getId()` as `uint64_t`, reconstruct via `filament::Entity::import(static_cast<int32_t>(handle))`.

**EngineNodeComponent handle storage**: Store glTF node **index** (not pointer) in `m_nodeHandle` — index is stable across the session, pointer could dangle if cgltf_data were ever freed (though we never call `releaseSourceData()`). At runtime, resolve via `asset->getSourceAsset()->nodes[index]`.

### 6.2 Rejected: Custom Scene Format (CEE sidecar JSON)

Same rationale as `.cematerial` rejection (material_migration.md §9.1): third truth source + unknown field loss + bidirectional sync discipline.

### 6.3 Edit Chain (per-operation landing)

| Operation | Doc Layer (cgltf) | Render Layer (Filament) |
|---|---|---|
| Gizmo drag | Node TRS fields (§8 conversion) | `TransformManager::setTransform(TRS→mat4)` (batch via `openLocalTransformTransaction`) |
| Inspector light edit | `cgltf_light` fields | `LightManager::set*` (S8 full setter set) |
| Rename | `cgltf_node.name` | `NameComponentManager::setName` (S3) |
| Create (empty node / light) | CEE-side created-node table | `EntityManager::create` + Transform/Light components |
| Delete | Remove from parent children | `scene->remove` + `Engine::destroy(entity)` |
| Save | `cgltf_write_file` (S6) | — (render layer has no persistence) |

**Create/delete implementation (cgltf structural constraint)**: `cgltf_node` cross-references (`parent`/`children`/`scene->nodes`/skin joints/animation targets) are all **raw pointers** (`cgltf.h:688`), and `data->nodes` is allocated at file-precise size — direct realloc would dangling all existing pointers. Therefore:
- **Created** nodes live in **CEE-side separate table** (`m_createdNodes`), never enter `data->nodes` at runtime
- **Delete** = compress parent's `children` pointer array in-place (memmove + count--) + mark node dead
- **Save-time one-pass reachability merge**: allocate merged "old + new" array, remap old→new pointers, simultaneously **prune unreachable (deleted/orphan) nodes** — ~120 lines, concentrated in Save path, zero runtime pointer risk

### 6.4 Property Reflection = Curated Table

Filament has no `get_property_list`-style reflection. Inspector `PropertyBag` comes from **backend-internal curated tables** (aligned with material_migration.md §7.1):

- **All nodes**: `name` (String)
- **Light entities** (determined by `LightManager::hasComponent`): `color` (Color, linear), `intensity` (Float, unit suffix by light type), `range` (Float; edit-time `setFalloff` mirrors gltfio's `range==0` heuristic `(I/0.05)^(1/4)`), `spotCone` inner/outer (Float, spot only) — all have runtime setters (S8). Light **direction** is the node rotation (gizmo), not a separate Vector3 property
- **glTF material parameters**: **v1 not in Inspector** (P2 via `cgltf_material` + `setMaterialInstanceAt` dual-write)
- Ranges/units output as-engine-native, not normalized (C4 scope clarification)

### 6.5 Known Limitations (v1 accepted)

| # | Limit | Disposition |
|---|---|---|
| L1 | gltfio loads only default scene (upstream `\todo`, `FilamentAsset.h:57`) | Accept; multi-scene v1 compiles default only |
| L2 | GLB embedded images preserved, but new geometry in GLB requires extending bin — v1 spawn only supports .gltf | P2 |
| L3 | glTF lights/cameras load one-way; writeback = hand-written cgltf fields | Covered in implementation |
| L4 | Animation not playing (Animator exists but not driven) — same as rbfx/Godot v1 | v2 PIE |
| L5 | Live document resident in memory (cost = JSON structure + name strings, not geometry) | Accept (geometry on GPU/engine side) |
| L6 | `readPixels` GL y-flip (`Renderer.h:647-649`) | Ruler 5 empirical calibration |
| L7 | WGL = system GL 4.x driver path (VM/remote desktop weaker than D3D12) | Record; Vulkan one-line switch (§4) |
| L8 | Saving a `.gltf` with external URIs writes only the JSON (autosave to `<exe>/autosave/` breaks relative `.bin` refs; Save As `.gltf`→`.glb` drops external buffers) | Accept v1; single-file `.glb` unaffected |
| L9 | Deleting a node that is a skin joint / animation target resurrects it at save (reachability closure) | Accept; by design of the save-time merge |

---

## §7 IEntityMirror Implementation + E2 Prerequisite

### 7.1 Phase 0: E2 Contract Slimming — EXECUTED 2026-09-10

- `CreatePrefabFromNodes` / `AssignMaterial` / `AssignAnimation` / `SerializeNodes` / `PasteNodes` migrated to `IEngineBackend::InvokeCustom(command, args, payload)` (string args = entity ids + paths, payload = binary clipboard bytes; default false = unsupported). rbfx keeps its five real implementations there; Godot/Null drop to the default; Filament never had them.
- `IEntityMirror` 12 pure-virtual → **7 pure-virtual**: `SyncToEditor` / `EnumerateObjectTypes` / `OnEditorTransformChanged` / `OnEditorPropertyChanged` / `CreateObject` / `DestroyObject` / `RaycastScene` (+4 sentinels unchanged).
- CI assertion added: `check_no_atom.ps1` now asserts `IEntityMirror` pure-virtual == 7 (Ruler 3).
- **BackendCaps deferred**: no consumer exists yet (level routing, asset filters), so adding the struct would be dead code — a C3 violation, not a KISS win. Revisit when the shell actually keys off engine capabilities.

### 7.2 Filament Per-Method Landing

| Method | Landing | Status |
|---|---|---|
| `SyncToEditor` / `FinishSync` | cgltf default scene pre-order walk → AZ entities + `EngineNodeComponent` (nodeHandle = glTF node index) | Real impl |
| `EnumerateObjectTypes` | Static curated table: Empty Node / Directional / Point / Spot (4 items; cgltf has no focused-spot distinction — FOCUSED_SPOT is a Filament runtime nuance) | Real impl |
| `OnEditorTransformChanged` | §6.3 table (TRS dual-write) | Real impl |
| `OnEditorPropertyChanged` | §6.4 curated table → LightManager setters | Real impl |
| `CreateObject` / `DestroyObject` | §6.3 table | Real impl |
| `RaycastScene` (pure-virtual) | Self-written ray-AABB traversal of the default scene (local box × world matrix, S9) → O3DE space conversion; overlay excluded by construction (it lives outside the cgltf doc), deleted subtrees unreachable (detached at destroy) | Real impl (AABB) |
| `GetWorldBounds` (sentinel) | Local box × world matrix → `ConvertAabb` (§8) | Real impl |
| `RaycastNode` (sentinel) | Default false (AABB-level, Godot v1 baseline); v2 candidate = `View::pick` (S10) | Default |
| `SaveScene` (sentinel) | `cgltf_write_file` (§6.1) | Real impl |
| `InvokeCustom:AssignMaterial` | P2: `.filamat` → `createInstance` → `setMaterialInstanceAt` | P2 |
| `InvokeCustom:CreatePrefabFromNodes` | P2: cgltf subtree serialization | P2 |
| `InvokeCustom:SerializeNodes/PasteNodes` | v2 | v2 |
| `InvokeCustom:AssignAnimation` | Not implementing (L4) | — |

**Stub rate = 0/7** (C5 ruler first满分).

---

## §8 Coordinate Conversion (zero new math)

Filament = right-handed Y-up, camera -Z forward (S7) — **identical to Godot**. Add `EngineSpace::Filament` to `EngineTransformConverter`:

```cpp
enum class EngineSpace { Rbfx, Godot, Filament };  // Filament uses Godot branch constants
// PositionToEngine: [x, z, -y]; C matrix det=+1 (pure rotation)
```

AABB via 8-corner reconstruction (existing `ConvertAabb`). Verification = Ruler 7 (§13).

---

## §9 Overlay Primitives (one material + runtime depth toggle)

```
cee_overlay.mat:
  material { name: "CEE Overlay", shadingModel: unlit, blending: opaque, requires: [ color ] }
  fragment { void material(inout MaterialInputs m) { prepareMaterial(m);
              m.baseColor = float4(getColor().rgb, 1.0); } }
              → matc (CMake DEPENDS matc, §4) → cee_overlay.filamat
```

- **`SetDepthTest(bool)` = submission-time flag** (rbfx/Diligent semantics: the shell calls it, then submits that depth state's batch — GenericDebugDisplay submits a depth batch then an always-on-top batch each frame). Landed form: **four renderables** (LINES/TRIANGLES × depth-tested/always-on-top), each batch owning its VertexBuffer/IndexBuffer — `setBufferAt` is a deferred command, so batches sharing one VB would both draw the last upload. The two material instances (`on` default / `off` with `setDepthCulling(false)`, M4 `MaterialInstance.h:524`) are bound per renderable at creation; no per-frame instance swap.
- **Dynamic vertices**: per-batch `VertexBuffer::setBufferAt` streaming upload each frame (KB-scale); `RenderableManager::Builder().culling(false).priority(7)`.
- **Isolation**: no layer mask needed — `RaycastScene` walks the cgltf document's default scene and the overlay lives outside the document, so picking skips editor geometry by construction (§7.2).
- `getColor()` reads the `COLOR0` vertex attribute directly (`bakedColor.mat` pattern). The `requires: [ color ]` declaration tells Filament the geometry provides this attribute as `FLOAT4` (`MaterialBuilder.cpp:91`).

---

## §10 Material: see material_migration.md §7.4

The filament material integration (type = CEE-compiled `.filamat`, document = `MaterialSchema`
two-layer value JSON `.fmat.json`, curated schema + `hasParameter` cross-validation, offscreen
RenderTarget + async `readPixels` three-state preview) lives in the unified material plan:
**`material_migration.md` §7.4**. This doc keeps only the backend-side seams: the matc/resgen
build pipeline (§4) and the overlay material (§9).

---

## §11 IAssetSource (2 pure-virtual)

Project directory `QDir` walk (rbfx/Godot same pattern); extension whitelist via `BackendCaps::assetExtensions`. Thumbnails v1 = extension icons.

---

## §12 Phases & Effort

| Phase | Content | Exit |
|---|---|---|
| **P0** (~1w) | E2 slimming (all backends + CI) ‖ build integration (facade/CRT/WX strip/provenance/collision audit) + `FilamentBackend` empty viewport (HWND swapchain + clear color + resize recreate + destroy sync) + overlay primitives (§9) | `--backend filament` shows clear viewport + gizmo/grid; `check_no_atom` PASS; NullBackend single-backend build OK |
| **P1** (~2w) | Scene loop: gltfio load + live doc mirror (Outliner/name) + Inspector (transform/name/light curated table) + gizmo bidirectional + camera sync + AABB picking + Create(light/empty)/Delete + `SaveScene` round-trip + New/Open Level routing `.gltf/.glb` | Rulers 1/2/6/7 |
| **P2** (~1.5w) | Material: `FilamentMaterialSource` (8+2, material_migration.md §7.4) + curated schema + value JSON round-trip + RT preview (async three-state) + mesh spawn + `InvokeCustom:AssignMaterial` | Material ruler (§13 ruler 5) |
| **P3** (~0.5w) | IBL environment assets (cmgen one-time) + preview orientation calibration (L6) + all rulers pass + doc update | §13 all pass |

**Total ≈ 5.5–6 person-weeks** (including E2 prereq).

---

## §13 Verification Rulers (executable, falsifiable)

1. **Byte round-trip**: Open `.gltf` and `.glb` each one → no edits → Save → reload mirror consistent; `.gltf` JSON unchanged except formatting; `.glb` opens in official `gltf_viewer`.
2. **Edit round-trip**: Move/rotate 3 objects + change 1 light intensity/color + delete 1 create 1 → Save → restart reload → scene matches edit final state (TRS no decomposition error, light params preserved). **Rotation direction must match the gizmo** (early probe for transpose-class regressions in the cgltf↔AZ basis extraction).
3. **C2/C5**: NullBackend single-backend build OK + Filament headers don't escape `Backends/`+`MaterialEditor/` (grep); `IEntityMirror` pure-virtual == 7 (CI assertion, `check_no_atom.ps1`); `EngineSpace::Filament` takes the Godot branch in all three `EngineTransformConverter` conversion points; Filament **stub = 0**.
4. **Coordinate**: O3DE viewport drag gizmo +Y (up) → Filament world +Z up (same as Godot, §8); camera forward un-mirrored, left-right un-flipped.
5. **Material**: New → edit baseColor/metallic/roughness/texture → save reopen all values preserved; undo×20 returns to initial; **interaction perf: drag roughness 3s, main viewport FPS drop <10%**; preview sphere orientation correct.
6. **Picking**: Click mesh to select (AABB precision), light selectable via wireframe gizmo, light color change immediate in viewport; `RaycastScene` landing point doesn't float.
7. **Version lock**: `.filamat` from same-checkout build; manual old `.filamat` swap produces clear error with version number.
8. **Build**: All backends default config configure + build 0 error; `-DCEE_ENABLE_FILAMENT=OFF` single rbfx/godot/null build unaffected.

---

## §14 Risk Table

| # | Risk | Mitigation |
|---|---|---|
| R1 | tnt bare-name target collision with O3DE/other backends | §4 collision audit gate |
| R2 | WGL `SetPixelFormat` once-per-window | CEE viewport is GL-virgin by design; checklist |
| R3 | `/MT`→`/MD` forced, filament-side link warnings | CI md variant = official build matrix (B2/B3) |
| R4 | GL backend depends on user GL 4.x driver (L7) | Vulkan switch reserved (one line, E3); error path = `BackendError::RenderDeviceUnavailable` |
| R5 | `cgltf_write` fidelity for edge cases (sparse accessors, KHR extensions) | Ruler 1 tested with real industrial assets; non-fidelity items go to L-table |
| R6 | Live document memory (L5) with large scenes | O(N) same as rbfx/Godot; accept, v2 incremental sync |
| R7 | Filament upgrade breaks API/`MATERIAL_VERSION` | facade + `CEE_FILAMENT_ROOT` single point; ruler 7 |

---

## §15 Explicit Rejections (preventing over-design)

| Item | Rejection Reason |
|---|---|
| Custom scene format / sidecar scene schema | §6.2: third truth source (`.cematerial` precedent) |
| Fork Filament (add writer to gltfio) | cgltf_write in-tree sufficient (S5/S6) |
| In-process filamat/JIT material compilation v1 | M6: glslang unconditional + crash surface; precompile + (v1.5) matc subprocess covers all v1 needs |
| `.mat` text/shader/node-graph editor | material_migration.md §9.7 rejected; matdbg is separate tool |
| `camutils::Manipulator` camera | CEE already has `EditorViewportCameraController` (Plan §B2) |
| `filamentapp`/SDL | samples windowing dependency; CEE viewport layer already solved |
| `View::pick` for v1 | Changes picking framework flow (async vs sync); AABB-level = Godot v1 baseline |
| Vulkan backend v1 | WGL sufficient; switch one line; don't verify early |
| Runtime hot-swap / dual-backend same-process rendering | C6 verdict (engine singleton + swapchain handle) |

---

## §16 Documentation Authority Chain

`Plan.md` (§A6 C1–C7/§B12) > `rbfx_migration.md` §4 (roadmap, E2 gate) > `material_migration.md` (material contract §4; filament landing §7.4) > **this document** (Filament backend sole plan) > `Progress.md`.

---

## §17 Implementation Revision Log (reserved)

- 2026-09-08: Initial draft (pre-implementation, merged from kimi + glm reviews).
- 2026-09-10: §4 rewritten — add_subdirectory failed the collision audit (R1 realized: O3DE's `gtest`/`meshoptimizer` + rbfx's embree `math`/glslang collide); landed the prebuilt-standalone + INTERFACE facade. §5 camera landing rule, §9 instance-equivalence note, §7.1 E2 executed (12→7 + InvokeCustom, BackendCaps deferred), Ruler 3 updated. Runtime deps recorded: uberarchive provider mandatory, `gltfPath` for external URIs, `setExposure` for physical camera.
- 2026-09-10 (implementation session): **P0+P1+P2 implemented and COMPILED + LINKED + smoke-passed on the team Profile config** (`Scripts/cee_smoke_filament.ps1`: alive + no assert dialogs; `check_no_atom` PASS with the new IEntityMirror==7 assertion). Contract lesson landed: backend `Initialize` must not reject an empty `--project` (rbfx/Godot do not; rejecting it left the whole editor on empty contracts). Three new build gates discovered and landed: (a) rbfx's public `DESKTOP=1` define rewrites `ShaderModel::DESKTOP` — the two include islands park it via `#pragma push_macro`; (b) rbfx INTERFACE include dirs hijack `<math/...>`/`<stb_image.h>` — filament roots forced to the front with `target_include_directories(BEFORE)`; (c) the standalone bootstrap must pass `-DFILAMENT_SHORTEN_MSVC_COMPILATION=OFF` or Debug libs bake `_ITERATOR_DEBUG_LEVEL=0` and MSVC rejects the link (LNK2038). Full pitfall table in filament_migration_progress.md §4.
- 2026-09-11: **Second-round dual review absorbed** (`review_filament_migration_glm.md` 3🔴 + `review_filament_migration_kimi.md` 7🔴, all source-verified; details in progress §7). Landed: (1) rotation extraction from `MatrixFromCgltf` results now stores basis vectors in AZ COLUMNS (`SetColumn`, was `SetRow` → inverse rotations in mirror seeding / gizmo write-back; the point-transform path was already correct and is unchanged); (2) overlay reworked to 4 depth-bucketed batches, each with own VB/IB — `setBufferAt` is a deferred command, so a shared VB drew the last upload for both primitive types, and `SetDepthTest` is submission-time (rbfx semantics), not a global switch; (3) preview sphere null-check inverted (never created) fixed + sphere buffers owned by the preview scene; (4) created-node lights now drive LightManager on property edit (shared `ApplyLightToRender`, incl. gltfio-parity falloff heuristic); (5) created nodes carry stable ids (multi-select delete no longer misaligns); (6) filament `quatf(w,x,y,z)` arg order fixed; (7) save merge remaps `skin->skeleton`; (8) `RaycastScene` walks the default scene (deleted subtrees unreachable) + accepts `SA_INSIDE`; (9) surface rebuild (dock float) destroys/recreates only the swapchain — view/scene/camera/gltfio stack survive, unsaved edits ride through; (10) material source constructed in the backend ctor (GetMaterialSource safe after failed Init), texture swaps owned (no leak), dtor sweeps instances; (11) CMake: facade closure check covers the full link set in both configs, dummy.c written at configure time, provenance message var fixed. Recorded deviations: value JSON uses the two-layer MaterialSchema format (rbfx-consistent, supersedes the original single-layer JSON sketch); `SetPreviewModel` stays a false-sentinel (fixed sphere, v1 scope). Ruler 1/2/4/5/6 runtime eyeball checks remain the gate.
