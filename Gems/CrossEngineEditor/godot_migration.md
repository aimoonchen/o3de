# godot_migration —— Godot 后端接入终案与实现记录

> 定位（2026-08-20 定稿）：本文 = Godot 后端（**进程内 libgodot**）接入的**唯一终稿**——架构裁定 + 已落地实现记录 + v1.5/v2 路线 + 已知限制与验收清单。与 `rbfx_migration.md`（rbfx 后端终稿）平行，共享总路线（`rbfx_migration.md` §4）。
> 权威关系：愿景硬规则与路线 = `Plan.md` §A6/§B12；进度与踩坑 = `Progress.md`。
> 来源：既有实现（`Backends/GodotBackend.{h,cpp}` / `GodotApi.h`）+ `Plan.md`/`Progress.md` 已核实锚点 + 2026-08-20 双方案评审（deepseek_1 的"Godot 独立进程 + IPC"方案已证伪，见 §1）。

## 1. 架构裁定：进程内 libgodot（否决进程外 IPC）

- **已落地事实**：Godot 4.8 以 out-of-tree scons 产 `godot.<cfg>.dll`，CEE **运行时 `LoadLibrary` + `GetProcAddress` 加载**（构建期不链接，仅需 checkout 头存在）；C-API 入口 `libgodot_create_godot_instance(argc, argv, init)`（Plan §B1）。帧驱动 = `Main::iteration()` 在 `Tick()`（Plan §B2）。
- **进程外方案（deepseek_1 §7）已证伪**：该稿主张"godot 无官方静态库/embed 构建 → 必须独立后端进程 + JSON-RPC 管道"，并断言"同进程 SDL/单例冲突"。实测：libgodot 与 rbfx（EASTL + SDL）均已与 AzCore **同进程共存运行**；进程外为此付出的 IPC 帧协议 / JSON 编解码 / 崩溃恢复 / 断线检测全是为不存在的问题付的代价（opus5 裁决 D1/D14）。
- **Godot 侧唯一真实的架构约束** = libgodot **无 "render into external surface" API**（只能自建窗口）→ 视口嵌入走 reparent（§2.2）。这是渲染面约束，不是数据面约束，与"是否跨进程"无关。
- **崩溃语义（接受）**：引擎崩溃 = 编辑器崩溃，无隔离；缓解 = `AZ::ToolsCrashHandler`（已链接）+ 崩溃前落盘，**不做进程隔离**（代价论证见 `rbfx_migration.md` §4 防复发清单）。

## 2. 已落地能力（实现锚点，均 profile 编译通过）

| 面 | 落点 |
|---|---|
| 构建接入 | `External/CMakeLists.txt` `CEE_ENABLE_GODOT`；DLL 运行时加载，构建期零链接依赖 |
| 视口嵌入 | `--wid <hwnd>`（4.8 `CreateWindowExW` 的 hWndParent，WS_POPUP 非 WS_CHILD）+ `set_embedded_in_editor(true)` → CEE 取回 Godot 主窗 HWND 做一次 reparent：`SetParent` + `WS_CHILD\|WS_VISIBLE\|WS_DISABLED` + `WS_EX_NOACTIVATE`（`EmbedGodotWindow`，`GodotBackend.cpp:95`；owned-popup 转真子窗口；**不加 `WS_EX_TRANSPARENT`**——对普通子窗口无效且吞输入） |
| 输入 | `WS_DISABLED` 子窗口不收输入 → Windows 路由回 Qt 父窗口，零延迟 |
| 帧循环 | `Main::iteration()` 在 `Tick()`（present 在此；`EndOverlayFrame` 空）；vsync 一次性关（`window_set_vsync_mode(VSYNC_DISABLED)`，`GodotBackend.cpp:389`），宿主 60fps 节流门为唯一限帧源 |
| 镜像 | 每引擎节点 = 一个 AZ::Entity（`TransformComponent` + `EngineNodeComponent`）；`get_property_list()` → `PropertyBag`（10 子类）；`m_category` = 类名 + GROUP/SUBGROUP 段名（CATEGORY 行仅清分组；`GodotBackend.cpp ReadProperties`） |
| 回写 | `set(name, value)`（含 _set/_get 虚拟路径）；transform-only 走 `OnEditorTransformChanged`（`Node3D::set_global_transform` / `Camera3D` 挂场景根 `make_current`）；`m_syncing` 抑制位防回显死循环 |
| 点选 | AABB 级（仅 `is_class("GeometryInstance3D")` 者，`get_aabb` × global transform）；`RaycastNode` 走哨兵默认（false = 无精拾路径）——三角精拾留 v2（同一接缝，不返工，Plan §B5b） |
| overlay | 常驻 `MeshInstance3D` + **`ArrayMesh::add_surface_from_arrays`** 每帧批量提交（官方 gizmo 路径，每 surface O(1) 次 GDExtension 调用；ABI 缺入口回落 `SurfaceTool`）；unshaded + `FLAG_ALBEDO_FROM_VERTEX_COLOR` + `FLAG_SRGB_VERTEX_COLOR` 材质（关键坑：RefCounted 资源须先 `set_material_override` 保活再配 flag） |
| 持久化 | `SaveScene` = `PackedScene::pack` + `ResourceSaver::save`（`GodotBackend.cpp SaveScene`）；镜像实体不写 O3DE prefab（引擎原生场景为唯一真相） |
| 资产 | Godot 项目目录 + 扩展名过滤枚举（.tscn/.gd/.res/…）；缩略图 v1 = 扩展名图标 |

## 3. 契约实现现状（stub 记账）

`IEntityMirror` = 19 方法（15 纯虚 + 4 哨兵默认）。Godot 真实现 7：纯虚 5（`SyncToEditor` / `OnEditorTransformChanged` / `OnEditorPropertyChanged` / `CreateObject` / `DestroyObject`）+ 哨兵默认 2（`GetWorldBounds` `GodotBackend.cpp:750`、`SaveScene` `:1230`，`FinishSync` `:1038`）；**stub 10 纯虚**（`GodotBackend.cpp:1306-1378`，标注 "rbfx-first"）：`EnumerateObjectTypes` / `RaycastScene` / `CreatePrefabFromNodes` / `AssignMaterial` / `AssignAnimation` / `SerializeNodes` / `PasteNodes` / `SaveResource` / `ReadResourceProperties` / `WriteResourceProperties`。

> **10/15 = 67% 纯虚在第二后端上是空壳**——这是"契约偏向 rbfx"的报警（Plan §A6 C5 尺子的基线读数）。处置 = `rbfx_migration.md` §4：v1.6 `InvokeCustom` 阀门 + 可缓项契约瘦身（目标 ≤3）+ E5 第三后端 PoC 证伪。v1 不修（stub 属 C4 允许态，哨兵语义正确）。

## 4. v1.5 / v2 路线（Godot 视角；总路线 = `rbfx_migration.md` §4）

- **v1.5（两后端共享，同批）**：undo 命令化 + 镜像身份稳定化。Godot 侧落点：`ObjectID`（实例 id，进程内稳定）→ EntityId 确定性派生；Delete 的 Undo = `PackedScene` 场景片段打包预存 + 重建（重建后更新身份映射表）；**不引入** godot 自身 `UndoRedo` / `EditorUndoRedoManager`（撤销是编辑器侧概念，后端保持无状态）。注意：重载场景产生新 ObjectID → 差集重镜像自然销毁旧镜像（身份映射表随同步更新）。
- **v2**：Godot 三角精拾（`RaycastNode` 真实现，网格三角测试；**不引入物理系统 `intersect_ray`**，Plan §B11 已裁）；资源编辑闭环（`Read/WriteResourceProperties` + `SaveResource` 真实现——E2 契约瘦身前不动契约）；`EditorFileSystem` 导入管线进程内（文档流程缓解：首次先跑 `godot --headless --editor --quit-after 1` 生成导入缓存）；IAssetSource 升 SQLite（共用）。NodePath 编辑（v2 需要时）：`node_path_cache`（`scene/resources/packed_scene.h:47`，编辑态构建）提供运行时 NodePath ↔ 场景节点索引的可逆映射，是"外部 UI 改场景属性"的落点参照。
- **v1.6 E3 元信息（Godot 映射，随 `rbfx_migration.md` §4 v1.6）**：`PROPERTY_HINT_RANGE` 的 `"min,max,step"` → `m_min/m_max/m_step`（官方 Slider handler）；`PROPERTY_HINT_RESOURCE_TYPE` / `FILE/DIR` 的 `hint_string` → `m_resourceTypeFilter`；`class_get_default_property_value` → 恢复默认值（v2 按需）；description/metadata → tooltip（`m_description`）。

## 5. 已知限制与风险

| # | 项 | 处置 |
|---|---|---|
| G1 | `--wid` 为 owned top-level 语义（非 SetParent/WS_CHILD 真子窗口），reparent 后的几何/Z 序由编辑器管理，嵌入窗口有若干限制 | 已落地 `EmbedGodotWindow` 转真子窗口；B10 冒烟实测（resize / 最小化 / dock 浮动 / 关闭） |
| G2 | Inspector transform 显示 O3DE Z-up 契约值，与 godot 原生编辑器（Y-up）数值不一致 | 接受（C4 决定，Plan §B6 已知限制）；必要时加"引擎原生坐标"只读行，v1 不做 |
| G3 | 引擎崩溃 = 编辑器崩溃 | §1 崩溃语义 |
| G4 | 全量重镜像 O(N·P) + undo 历史湮灭（创建/删除触发） | v1.5 身份稳定化直接消灭（`rbfx_migration.md` §4） |
| G5 | Godot 升级破坏 C-API | `GodotApi.h` 集中封送（`Call` / `AsAabb` / `AsTransform3D` / `BuildSurfaceArrays`），适配层只依赖公开稳定 API（PropertyInfo / SceneTree / 资源格式） |
| G6 | Godot 源资源需 `.import` 缓存（`EditorFileSystem`） | §4 v2；文档流程缓解 |

## 6. 待人工验收（Godot 相关项；总清单 = Plan §B10）

- 点选全链路（Godot）：点 mesh 精确选中、橙框、Outliner/Inspector 联动、gizmo 拖拽框跟随、Ctrl 多选；**Godot 删除后 Outliner 不得残留**（同步 free 修复，B10 必测项 7）
- 回写引擎侧：拖 gizmo / 改属性 / Undo → Godot 物体真实响应
- B2 快捷键冒烟（Godot 视口聚焦时）、B3 文字标签可见性、resize / 最小化 / dock 浮动 / reparent 边界
- 保存重开：`SaveScene` → 重启 → 重载，场景一致

## 7. 文档去向

- 双方案裁决（deepseek_1 / opus5）已并入 `rbfx_migration.md` §4；两稿吸收完毕已删除，不得重新出现第三份"当前方案"。
- 本文任何内容若与 `Plan.md` §A6（C1–C7）冲突，以 `Plan.md` 为准。
