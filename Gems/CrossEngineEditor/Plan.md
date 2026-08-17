# CrossEngineEditor 最终方案（Plan）

> 跨引擎 All-in-One 游戏引擎编辑器：以 O3DE `AzToolsFramework` 为编辑器内核，引擎经"后端契约"可替换接入。
> 本文 = 项目**初始方案**（Part A，设计原则与领域模型，历久不变）+ **当前落地方案**（Part B，反映当前仓库代码实现）。
> 全部内容已与 `Gems/CrossEngineEditor/Code/Source` 逐一核对，消除与实现不符的历史表述。
> 铁律：不臆想 API（均已 grep 真实头核实）；KISS，不过度设计；能用原版语义就用原版。
> 愿景（2026-08-14 定稿）：完全复用 O3DE 原版编辑器的框架和控件库；编辑器框架依赖的 O3DE 基础模块可以引入；底线 = Atom 渲染模块完全剔除；编辑器后端可自由切换到其他第三方引擎。
> 总则：在复用 O3DE 原版编辑器框架的过程中，实现方案与代码在完整保留功能的同时必须简洁、KISS、不过度设计。
> 愿景落地规则见 §A6，护栏与路线见 §B12；AssetBrowser 100% 复用终案与实现见 `rbfx_migration.md` §2；Godot 后端接入终案见 `godot_migration.md`（双方案评审裁决并入 `rbfx_migration.md` §4）。

---

# Part A — 设计原则与领域模型（初始方案，仍有效）

## A0. 硬性约束
- **第一方新代码强制 C++23**（`CrossEngineEditor` target `CXX_STANDARD 23`，`Code/CMakeLists.txt:50`）。
  优先用 C++23 特性（`std::expected`/`std::span` 等）但不为用而用；与 AZ 反射/EBus 交互处遵循 O3DE 约定。
  引擎子模块（rbfx `Urho3D` target）为 C++17，经 ABI 边界隔离，不受影响。
- 与 O3DE 框架只走公开头文件与反射，不依赖其内部 C++ 版本。
- **顶层边界**（2026-08-19 落账）：**CEE = 跨引擎的编辑器前端（UI/交互/工具链可复用），不是跨引擎的内容（场景仍引擎原生、不可移植）**。一条裁决三处下游：镜像实体不写 O3DE prefab（§B4）；撤销不能只靠 O3DE 实体状态快照（`rbfx_migration.md` §4 v1.5）；New/Open Level 走引擎场景（v2 首选）。

## A1. 设计原则（KISS 边界）
| 原则 | 含义 |
|---|---|
| 靠不依赖来删除 | 不 fork 遗留 Editor（Strategy B），从零依赖开始（无 gEnv/CryCommon/Atom）|
| 复用 > 抽象 > 新建 | 编辑器内核直接用 `AzToolsFramework`；只在引擎边界建抽象 |
| 一次写好，处处复用 | 引擎无关的部分（GenericDebugDisplay）写一次，所有引擎复用 |
| 后验证 > 先设计 | 先 NullBackend 跑通，再抽象接口 |
| 反射驱动 | 属性/序列化全走 AZ 反射，不手写 UI |

**核心判断**：O3DE `AzToolsFramework` 在编辑器领域模型上已达 Unity/UE 同级（反射 Inspector、Gizmo、EditorMode、Transaction 撤销、ActionManager 命令、Prefab）。**不重造这些轮子**，只补两样 AzToolsFramework 框架层缺失的 UX：**Workspaces**（官方遗留 Editor 有 "Layouts" 菜单但依赖遗留代码，Strategy B 不采纳）与**命令面板**（O3DE 上游全仓核实无此设施）。（初始方案承诺的"调整上一步"浮动面板**已取消**——未实现，Undo 栈已覆盖核心需求。）

## A2. 领域模型 → O3DE 设施映射（100% 复用，零重写）
| 领域概念 | O3DE 现成设施 |
|---|---|
| 应用/生命周期 | `AzToolsFramework::ToolsApplication`（继承）|
| 文档/场景 | 关卡=根 Prefab（v1 框架占位壳；引擎原生场景=唯一真相，引擎场景 Prefab 化=v2）+ `EditorEntityContext` |
| 对象 | `AZ::Entity` + Component（引擎对象经镜像层映射）|
| 命令 / 撤销 | `ActionManagerInterface` / `UndoSystem` |
| 属性面板 / 大纲 / 资产 | `EntityPropertyEditor`+`EditContext` / `EntityOutlinerWidget` / `AssetBrowser` |
| Gizmo / 视口交互 | `Manipulators`+`DebugDisplayRequests` / `ViewportInteractionRequestBus` |
| 停靠 / 主题 | vendored Qt-Advanced-Docking-System(默认) · `AzQtComponents::FancyDocking`(fallback) / `StyleManager` |

我们只写"外壳 + 引擎后端边界 + 两个 UX 增强"。

## A3. 关键架构杠杆：DebugDisplay 三段分解（63 → 3）
`DebugDisplayRequests` 有 63 个高层绘制方法（`EntityDebugDisplayBus.h:186-272`）。**不让每个引擎实现 63 个**，而是分三段：
```
[AzToolsFramework Gizmo/Shape]  调用 63 个 DebugDisplayRequests 方法（零改动）
        ▼
[GenericDebugDisplay]（引擎无关，写一次）  实现 63 方法（重载对经 `using` 复用，CullOn/Off 空实现）→ 降解为 线/三角 顶点批次
        ▼
[ISceneRenderer 3 原语]（后端实现）  SubmitLines / SubmitTriangles / SetDepthTest（文本走 Qt 覆盖层）
```
把"每引擎接入 Gizmo 渲染"成本从 63 方法降到 3 原语——本方案最关键的 KISS 杠杆。

## A4. 非目标（防过度设计）
- 不重写停靠系统；不做 Blender 式任意区域分割；不内置节点图/蓝图；不做包管理器。
- 不做跨 API GPU 纹理互操作（v1）；不引入遗留 Editor / CryCommon / CrySystem / Atom（CryCommon 位于 `Code/Legacy/`，现代框架零依赖，且含遗留渲染器接口 `IRenderer.h`）；不做多线程渲染管线（v1）。

## A5. 分阶段路线（历史里程碑，均已完成主体）
阶段 0 骨架 → 1 面板+NullBackend（M1）→ 2 GenericDebugDisplay+相机+网格（M2 解耦验收）→
3 Prefab/关卡工作流 → 4 对象模型+资产 → 4.5 gizmo 自绘子系统 → 5 首个真引擎后端（rbfx 先、Godot 随）→
6 打磨/多引擎/Play-in-Editor。当前进度见 `Progress.md`。

## A6. 愿景落地规则（2026-08-14 落账）

- **C1 依赖边界 = 单底线**：O3DE Atom 渲染模块完全剔除（渲染类模块一律不进依赖图）；编辑器框架所需的**基础模块依赖可以引入**（AzCore / AzFramework / AzToolsFramework / AzQtComponents / Thumbnailer / SQLite 3rdParty 等，以框架实际需要为准，不列白名单）。检验手段：链接产物无 `Atom*` 模块、源码 grep 无 Atom include、`BUILD_DEPENDENCIES` 变更须在 review 中说明理由（护栏脚本见 §B12 E1）。
- **C2 可移除性验收**：任意时刻 NullBackend 单后端构建 + 运行必须通过；引擎头文件/符号不得逃出 `Backends/`；两项纳入 B10 验收清单，把"当前成立"固定成"持续成立"。
- **C3 控件 100% 复用（硬规则）**：编辑器框架与控件库 100% 复用原版，不自建控件；复用控件与其数据来源冲突时**只替换数据源，不替换控件**（第一个判例：AssetBrowser，见 `rbfx_migration.md` §2）。确需偏离时升级为愿景级决策并在此落账。**判定手段**：新继承 QWidget 一族的自建类须能对上既有落账（判例或临时脚手架+删除计划，如 §B8 B3），否则视为违例。
- **C4 后端契约 = 愿景核心资产**：引擎能力须先进契约再进后端；契约变更须在同一变更内更新全部后端。**核心方法 = 纯虚**（漏改后端在编译期爆炸）；**能力接缝 = 哨兵默认**（`FinishSync`/`SaveScene`/`GetWorldBounds`/`RaycastNode`），默认值只能是"不支持"语义、不得是真实行为——新增任何默认实现须 review 标注（E6）。契约禁止引擎专属语义（空间约定 O3DE Z-up RH 米，转换收口后端边缘）。**IPC 数据面红线**：数据面契约（`IEntityMirror`/`IAssetSource`）不得引入 Qt/窗口系统类型（保持 POD/可序列化——未来 Unity 托管桥/沙箱后端 = 传输层替换而非重设计）；渲染面（`ISceneRenderer` 的 `OnSurfaceCreated(void*)` 表面共享是进程内固有语义）显式豁免。`ISimulation` 契约补全见 §B12 E3。
- **C5 接入新引擎成本显式化**：接一个新引擎 = 实现 `IEngineBackend` + 三子契约 + 坐标转换收口调用。**渲染面**成本 = DebugDisplay 三段分解（63 方法 → 3 原语）；**编辑面**（`IEntityMirror` **19 方法** = 15 纯虚 + 4 哨兵默认——镜像/资源/剪贴板/拾取/创建销毁）才是接入成本大头，纯虚允许 stub（基线：Godot stub 10/15；v2 契约瘦身目标 ≤3，E2 方案见 `rbfx_migration.md` §4）；`IAssetSource` = 2 枚举纯虚。踩坑沉淀模板见 §B12 E5。
- **C6 切换语义**：v1 切换 = 启动参数 `--backend`（重启切换）；**运行时热切换为非目标**——技术事实：rbfx/Godot 均在进程内持有 swapchain + native 窗口句柄 + 全局单例状态，热切需销毁重建整个视口表面与镜像层，且 `AZ::Interface<IEngineBackend>` 单实例注册模型不支持并存。
- **C7 不依赖 O3DE 内部（A0 扩展）**：从"公开头文件与反射"扩展到 CMake/构建层——不依赖 O3DE 内部私有属性名。现行违例：外部依赖运行时遍历器预缓存 hack（依赖 `O3DE_DEPENDENCIES_CACHED_*`，Diligent/rbfx/tracy 三处，见 B1），列为技术债（§B12 E4）。

---

# Part B — 当前落地方案（以仓库代码为准）

> 标注：**✅ 已落地** / **⏳ 待办或待实测** / **v2 后续**。

## B1. 总架构与契约 ✅

编辑器壳（Outliner/Inspector/Gizmo/Undo/Prefab/工具栏 + Workspaces + 命令面板）全部复用 O3DE 框架。引擎以**进程内库**接入：

- `IEngineBackend`（`Initialize`(→`std::expected`) / `Shutdown` / `Tick`）+ 三子契约（经 `AZ::Interface<IEngineBackend>` 单实例注册）：
  - `ISceneRenderer` — 表面生命周期（`OnSurfaceCreated` / `OnSurfaceResized`(uint32,uint32；跨线程原子快照走 `PackViewportSize` 辅助) / `OnSurfaceAboutToBeDestroyed`）+ overlay 3 原语（`SubmitLines` / `SubmitTriangles` / `SetDepthTest`，包在 `Begin/EndOverlayFrame` 内）；
  - `IEntityMirror` — 场景树镜像、双向编辑、`GetWorldBounds`、`RaycastNode`、`SaveScene`、`CreateObject`/`DestroyObject`；
  - `IAssetSource` — 资产枚举 + 缩略图。
- 启动 `--backend rbfx|godot|diligent|null` + `--project <path>` + `--scene <相对资源根路径>`。
- 坐标系转换单点收口 `EngineTransformConverter`（§B6），后端只在边缘调用。

**目录布局（实际）**：`Code/Source/{Application, Window, Viewport, Backends, BackendAPI, Framework, Profiling}`
（初始方案设想的 `EditorShell/`、`RenderCore/` 未采用——`GenericDebugDisplay` 在 `Viewport/`，命令面板/Workspaces 在 `Window/`）。

**工程构建**：CMake 开关 `CEE_ENABLE_RBFX / CEE_ENABLE_GODOT / CEE_ENABLE_DILIGENT / CEE_ENABLE_TRACY`（默认全 ON）。
**rbfx 与 Diligent 编译期互斥**（rbfx 开启时 Diligent 自动关，默认走 rbfx；`External/CMakeLists.txt:103`）。
- rbfx：`add_subdirectory(I:/rbfx)`，target `Urho3D`（C++17）；剥 O3DE `/WX`、预置 `O3DE_DEPENDENCIES_CACHED_*`；关 NAVIGATION/SYSTEMUI/RMLUI/PROFILING/NETWORK、只留 D3D12；EASTL（`ea::`）容器在后端边缘转 AZStd。
- Godot：out-of-tree scons 产 `godot.<cfg>.dll`，**运行时 `LoadLibrary` + `GetProcAddress` 加载**（构建期不链接，仅需 checkout 头存在）；C-API 入口 `libgodot_create_godot_instance(argc, argv, init)`（Godot 4.8）。
- Diligent：作"无引擎演示后端"；O3DE 运行时依赖遍历器预缓存 hack 规避静态库循环依赖爆栈（依赖 O3DE 内部属性名，升级有碎裂风险，已注释）。

## B2. 视口与渲染嵌入 ✅

Qt 三层结构：`EditorViewportWidget`(控制器) → `EngineViewport`(容器 QWidget + `createWindowContainer`) → `EngineViewportWindow`(QWindow) → 后端 swapchain 直出（**Diligent D3D12 直出，非初始方案设想的 GL FBO / QOpenGLWidget**）。
**表面工程要点（全部落地）**：首曝光懒建 swapchain、物理像素尺寸 + DPI 去抖、0×0 双重防御、
表面销毁 latch 重置 + DirectConnection 同步 `Flush+WaitForIdle+释放`、隐藏暂停出帧、native handle 缓存、D&D native 层单路径转发。
反例全回避（无 `QOpenGLWidget` / `QRhi` / `QImage` 拷贝；父级不设 `WA_NativeWindow`）。
Diligent 后端：FLIP_DISCARD 三重缓冲 + RGBA8_UNORM_SRGB + D32_FLOAT + 4x MSAA offscreen resolve + `Present(1)`，不写 `swapInterval`。

**世界网格 / 相机**：`EditorGrid`（世界 overlay，EditorShell 每帧调 `GenericDebugDisplay` 画，非 backend）；相机用**自建 `EditorViewportCameraController`**（包 `AzFramework::CameraSystem`，orbit/pan/dolly/fly；非初始方案设想的 `ModularViewportCameraController`）。

**两后端嵌入方式不同且不统一（架构决定）**：

| | rbfx | Godot |
|---|---|---|
| 方式 | `params[EP_EXTERNAL_WINDOW]=HWND`（`RbfxBackend.cpp OnSurfaceCreated`）→ SDL 直接挂 Qt 表面，**无子窗口、无 reparent**（最佳实践路径） | `--wid <hwnd>` = 4.8 `CreateWindowExW` 的 hWndParent（WS_POPUP 非 WS_CHILD）+ `set_embedded_in_editor(true)` → CEE 取回 Godot 主窗 HWND 做一次 reparent：`SetParent` + `WS_CHILD\|WS_VISIBLE\|WS_DISABLED` + `WS_EX_NOACTIVATE`（把 owned-popup 转真子窗口；`EmbedGodotWindow`，`GodotBackend.cpp`；去掉 `WS_EX_TRANSPARENT`——对普通子窗口无效且吞输入） |
| 输入 | 编辑器全权（无子窗口） | `WS_DISABLED` 子窗口不收输入，鼠标/键盘由 Windows 路由回 Qt 父窗口 → emit `InputEvent`，窗口仍正常渲染 |
| 帧驱动 | `RunFrame()` 在 `EndOverlayFrame`（渲染 + present 一步）；`Tick()` 空 | `Main::iteration()` 在 `Tick()`（present 在此）；`EndOverlayFrame` 空 |
| 相机 | 场景根相机节点每帧 `SetWorldPosition/SetWorldRotation` + `Renderer::SetViewport` | `Camera3D` 挂场景根，`set_global_transform` + `make_current()` |

> Godot libgodot 无 "render into external surface" API（只能自建窗口），reparent 是架构强加的最优路径。
> Godot vsync 已一次性关（`window_set_vsync_mode(VSYNC_DISABLED)`，`GodotBackend.cpp:389`），由宿主节流门限帧（§B3）。

**Overlay（3 原语）**：AzToolsFramework gizmo/manipulator → `GenericDebugDisplay`（`Viewport/GenericDebugDisplay`，实现 63 方法 → 线/三角批；>1.5px 线展开为相机朝向 quad + 近平面裁剪）→ 后端：
- rbfx：场景根 `DebugRenderer`（`AddLine/AddTriangle` + depthTest），随管线绘制、帧尾自动清。
- Godot：常驻 `MeshInstance3D` + **`ArrayMesh`**，每帧 `add_surface_from_arrays` 批量提交（官方 gizmo 路径，每 surface O(1) 次 GDExtension 调用；ABI 缺入口回落 `SurfaceTool`）；unshaded + `FLAG_ALBEDO_FROM_VERTEX_COLOR` + `FLAG_SRGB_VERTEX_COLOR` 材质（关键坑：RefCounted 资源须先 `set_material_override` 保活再配 flag）。
- Diligent：line/triangle 各两个 PSO 变体（depth-on LE / depth-off，`DepthWriteEnable=False`）；常量缓冲一帧一次上传；MSAA 目标创建失败则**跳过帧**（无回退直绘，`DiligentBackend.cpp:249`）。
- 文本走 Qt `ViewportOverlayLabels`（临时脚手架，见 §B8 B3）。

## B3. 帧循环与输入 ✅

**单循环**（对齐 Unreal/Godot/rbfx/O3DE 原生"一帧一次 present"）：`OnIdle` 由 `QTimer::singleShot` 自旋（激活 1ms / 非激活 100ms）：
```
TickSystem() → Tick() → 60fps 节流门 { TickRender(相机+overlay, rbfx present) → backend->Tick(Godot present) → FrameMark }
```
- **不手写 Win32 消息泵**（已删，`CrossEngineEditorApplication.cpp OnIdle`）：OS 消息归 Qt 事件循环，与 O3DE 原生 `IdleProcessing` 同构。曾有的手写 `PeekMessage/DispatchMessage` 全量泵是"选中后 orbit 卡顿"的架构级放大器（1ms 回调 × drain-until-empty，与 Qt dispatcher 双重泵竞争同一线程队列）。
- **鼠标 move 每帧合并**（正式修复）：`HandleNativeInput` 的 MouseMove 分支只记录最新 pos/buttons/modifiers；`ApplyPendingMouseMove` 每帧在 TickRender 开头调一次（相机 + hover）。非 move 事件即时处理、处理前先 flush。对齐 Godot `Input::accumulate` / O3DE `MoveX/Y→OnTick` / Blender `INBETWEEN_MOUSEMOVE` / Unreal `CapturedMouseMoves` 共识；orbit 精度不变（相机见帧总增量）。
- **60fps 节流门保留**：Godot 关 vsync 后它是唯一限帧源；否决"删节流 + 恢复 vsync"（会重现空转 ~500fps 与 present 拍频尖刺）。一帧一次 present（顺序 TickRender 先、backend Tick 后，各后端一次，无双 present）。
- **不上 dirty 按需渲染**（O3DE 新版视口同样持续 60fps；接缝 `RenderPipeline::AddToRenderTickOnce` 已留）。
- Tracy（`CEE_ENABLE_TRACY`，采样默认 OFF）已接入，FrameMark 在节流门内（一次 = 一次真实 present 帧）。

## B4. 对象镜像与属性反射 ✅

**每个引擎节点 = 一个 AZ::Entity**，挂两个组件：`TransformComponent`（标准版：Outliner 取 parentId / Gizmo 读写 / Undo）+ 自建 `EngineNodeComponent`（引擎无关，两引擎共用）：
- `m_className` — 引擎类名（只读显示）；
- `m_properties` — `PropertyBag`：多态 `EngineProperty` 容器，**10 个 RTTI 子类**（**Bool / Int(enum 非空走 ComboBox) / Double / String / Vector3 / Color / Quaternion / ResourceRef / ResourceRefList / Variant 只读兜底**，`EngineProperty.h`），只承载 O3DE 属性网格有 stock 控件的类型（网格无法原生编辑 variant 活动成员，故"基类 + 每基元一子类"，ScriptProperty 同法）；
- `m_nodeHandle` — 引擎侧句柄（rbfx `Node id` / Godot `ObjectID`），**反射（Version 2）以经序列化状态捕获在撤销恢复/根实例 overlay 接管时不被清零、但不落盘**（`EngineNodeComponent.cpp:31-37`）。

**进 Inspector 零自定义 handler**：`m_properties` 声明为一个 `DataElement`（ShowChildrenOnly）+ `SetDynamicEditDataProvider`（`ScriptEditorComponent` 范式，`EngineNodeComponent.cpp Reflect`）；provider 按元素地址返回运行时 `ElementData`（名称/UIHandler/枚举值/只读/category 分组）。Undo/控件/布局全由框架免费提供。引擎类**不逐个静态注册进 AZ 反射**（数量大/动态演化）。

- **枚举/写值**：rbfx `Serializable::GetAttributes()/GetAttribute(i)/SetAttribute(i,Variant)`；Godot `get_property_list()/get(name)/set(name,Variant)`。
- **回写**：Gizmo 拖拽 → `TransformBus` → `EditorTransformChangeNotificationBus` → `OnEditorTransformChanged`；Inspector 编辑 → `PropertyEditorEntityChangeNotificationBus` → `OnEditorPropertyChanged`（变更总线只报 componentId → 后端重读整个 PropertyBag 回推，`IEntityMirror.h:73-77`）。**桥接层 `m_syncing` 抑制位**：SyncFromEngine 期间的同步写与 gizmo 拖拽的 transform 广播不回推（transform 事件路由到 transform-only 路径），消除拖拽期每帧全 bag 回推冗余。Undo/Redo、Outliner 拖拽同链路天然覆盖（`EntityMirrorBridge` 挂总线统一驱动）。
- **节点解析统一 `ResolveNode(entityId)`**：所有实体→引擎节点操作（bounds/transform/property/destroy）一律读 live `EngineNodeComponent` 的 `nodeHandle`（prefab 接管换 id 后仍有效），miss 打 `AZ_Warning`。消除双轨解析矛盾；`m_entityToNode` 仅保留给 `FinishSync` 同批次 seed 变换用。
- **场景树同步**：surface 就绪后 `SyncFromEngine` **一次性全量**镜像（后端递归建实体，桥接层走 `AddRequiredComponents → AddEditorEntity → FinalizeEditorEntity`，`FinishSync` 里 `TransformBus::SetParent` 建 Outliner 树）。引擎侧增量变化、复用既有镜像实体 = v2（重同步须先清旧镜像再重建，否则 Outliner 重复建树）。
- **持久化**：`SaveScene` 已实现（rbfx `Scene::SaveXML` / Godot `PackedScene::pack` + `ResourceSaver::save`）并接 Save 菜单（`EditorMainWindow.cpp OnSaveLevel`）；**镜像实体不写 O3DE prefab**（引擎原生场景为唯一真相，两套序列化不竞争；`m_nodeHandle` 未落盘）。无后端场景时 Save/Open Level 回落 `.prefab`（`LoadFromStream`/`SaveToStream`）。
- ✅ **Create/Destroy 已接线**（`EditorMainWindow.cpp:444/472/506`）：Create Entity / Create 菜单 → `CreateObject(ObjectSpec)` + 重镜像；Delete → 先 `DestroyObject`（引擎节点仍可解析时）再 `DeleteSelected` + 重镜像；资产拖放 spawn 同链路（`EntityMirrorBridge.cpp SpawnAssetAtOrigin`）。

## B5. 停靠 / Workspaces / 命令面板（UX 增强）✅

- **停靠双路径**（`EditorMainWindow.cpp` 构造函数）：`CEE_HAVE_ADS` 定义时用 vendored **Qt-Advanced-Docking-System**（`ads::CDockManager` 注册为 central widget，先于任何面板创建；OpaqueSplitterResize + FocusHighlighting 等配置）；否则 fallback `AzQtComponents::FancyDocking`。两条路径均提供 `saveState/restoreState`，Workspaces 接口一致。
- **Workspaces**：`SaveWorkspaceLayout/RestoreWorkspaceLayout`（`saveGeometry` + 停靠 `saveState` 双路径 + `QSettings` 分组，菜单 Save/Restore 默认 "Default"）。⏳ 待肉眼验收。
- **命令面板**：`Ctrl+P`（`Qt::ApplicationShortcut`）模糊搜索全部已注册动作并执行（`EditorMainWindow.cpp BuildMenuBar`）。

## B5b. 点选与选中 ✅

**框架发起，后端只回答包围盒**：
```
点击 → FindEntityIdUnderCursor（框架，现成）
  → 可见实体 cache = EditorEntityViewportInteractionRequestBus::FindVisibleEntities
       （EditorViewportWidget 实现：返回全部带 EngineNodeComponent 的根实例 overlay 实体，不做视锥剔除、不查 octree；API 名 `GetLooseEditorEntities` 已随 O3DE prefab ownership 演进变义）
  → PickEntity → EngineNodeComponent 的 EditorComponentSelectionRequestsBus（4 方法，CEE 实现 3 个——`SupportsEditorRayIntersect` 走默认，`ComponentEntitySelectionBus.h:63`）
       （SupportsEditorRayIntersectViewport=true；EditorSelectionIntersectRayViewport = AABB 粗剔 → 三角精拾(有则权威) → 回落 AABB）
  → IEntityMirror::GetWorldBounds(entityId) → ConvertAabb → O3DE 空间 → SetSelectedEntities → Outliner/Inspector/Gizmo/Undo 全联动
```
两引擎 v1 拾取公共最低精度 = **AABB 级**（Godot 4.8 MeshInstance3D gizmo 实为三角精拾，但 libgodot GDExtension 表面无编辑器 TriangleMesh 设施，v1 同 AABB；Godot 三角精拾留 v2）。

- **bounds 两条语义拆分**（实测教训）：`GetEditorSelectionBoundsViewport`（selection/ray-pick）经私有 `GetBackendVisualBounds()` 取**原始**视觉 bounds——非可视 node 返回 **null**，直接不可拾取（修复"隐形小盒抢选"）；`BoundsRequestBus::GetWorldBounds`（visibility/icon）对非可视 node fallback pivot 小盒（进 octree + icon 有坐标）。非可视 node 靠 editor icon + Outliner 选中，业界一致。
- **仅本节点**：后端 `GetWorldBounds` 只取该节点自身可视几何（rbfx `FindComponents<Drawable>(SelfDerived)` / Godot 仅 `is_class("GeometryInstance3D")` 者），union 子树会吞成覆盖全场景巨盒抢选小物体。rbfx 另用 `IsInstanceOf(StringHash("Skybox"|"Zone"|"Light"))` 排除环境类 Drawable（覆盖子类；Godot 侧天然免疫）。
- **三角精拾接缝**（`RaycastNode` 三态：false=无精拾路径回落 AABB；true+hit/miss=权威）：**rbfx 已实现**——`Drawable::ProcessRayQuery(RayOctreeQuery(RAY_TRIANGLE))` 逐 drawable 三角测试（不依赖 octree 填充），修复旋转大 mesh 膨胀 AABB 抢选；**Godot/null 走默认（false，仍 AABB 级）**，Godot 三角精拾留 v2（同一接缝，不返工）。
- **bounds 缓存**：按需现算 + **transform 失效缓存**（`GetBackendVisualBounds` memoise，连 `TransformNotificationBus`，`OnTransformChanged` 置脏；相机 orbit 不触发）。v1 编辑器内动画不播放（`AssignAnimation` 只赋值不播放，`IEntityMirror.h:109-113`）→ 静态场景下世界 AABB 唯一变化源是 transform，缓存永不过期；动画/引擎侧自变几何 = 已知滞后，与 v2 增量同步一并解决。
- **选中框**：`DisplayViewportSelection` 用与拾取**同一 bounds 源**（`CalculateEditorEntitySelectionBounds`）`DrawWireBox` 橙色(1,0.6,0.15)、DepthTestOn、画在 gizmo（DepthTestOff）之前；多选多框；走现有 3 原语管线，零后端新代码。该类替换官方 `EditorDefaultSelection`，须接 `ViewportEditorModeTrackerInterface` 构造 `ActivateMode(Default)` / 析构 `DeactivateMode`。

## B6. 坐标转换 ✅

BackendAPI 边界恒 O3DE(Z-up/RH/米)；`EngineTransformConverter` 单点收口，后端只调用：

| 方向 | 映射 |
|---|---|
| O3DE → rbfx(Y-up/LH/+Z fwd) | `[x, z, y]` |
| O3DE → Godot(Y-up/RH/−Z fwd) | `[x, z, -y]` |

旋转走矩阵相似变换 `C·R·Cᵀ`（rbfx 的 C 是 det=−1 反射，结果仍 det=+1 免镜像）；整段变换含 Godot Transform3D 12-float 布局收口在 `TransformToEngineRaw12/FromEngineRaw12`；AABB 经 8 角点各自转换后重建 min/max。缩放 1:1。纯静态数学不单拉测试 target，关键不变量（朝向/左右不镜像、位置往返可逆）由两引擎端到端验收覆盖。
**已知限制（接受，2026-08-20 落账）**：BackendAPI 恒契约值 → Inspector 显示的 transform 为 O3DE Z-up 契约值，与引擎原生编辑器（rbfx/godot 均 Y-up）数值不一致。这是 C4"契约禁引擎专属语义"的既定代价（换转换单点收口与契约纯净）；必要时 Inspector 加"引擎原生坐标"只读显示行，v1 不做。

## B7. Gizmo 双风格（Blender / Unreal，视觉 100% 对齐）✅ + 状态账

用户可切换两套 gizmo 风格。**风格 = 主题（配色/尺寸常量）+ 几何策略（拓扑/裁剪规则）**，二者都取官方源码逐值真值，不臆测。
真值来源：Blender = `arrow3d_gizmo.cc`/`dial3d_gizmo.cc`/`transform_gizmo_3d.cc`/`transform_constraints.cc`；Unreal = legacy `FWidget`（`UnrealWidgetRender.cpp` 等，非 InteractiveToolsFramework）。
文件职责：`GizmoTheme`（per-style 几何常量+配色，每字段带源码出处）/ `GizmoViews`（自绘 view 继承 `ManipulatorView`，复用底层拾取/hover/拖拽/Undo）/ `GizmoManager`（组装 Linear/Planar/Angular manipulator + `DrawOverlay` + 拖拽反馈）/ `CrossEngineViewportSelection`（替换原版 handler）。
工具栏经 `GizmoControlRequestBus`（`SetGizmoMode` / `SetGizmoStyle` / `SetGizmoSpace` / `SetSnapEnabled`）；`GizmoMode` = Select(默认，不建手柄) / Move / Rotate / Scale / Combined，键位 **Q/W/E/R/T + 1/2/3/4 兼容**；空间 World/Local；Snap（只翻 enabled，Blender 5° / Unreal 10°）。

对齐状态账（round3 落账）：
- ✅ **N1** 旋转 ghost 扇形与起始 helpline 锚定**抓取角 θ0**（`m_dragStartHitWorld` 投影，非环 0°；UE 风格 band 锚定 0° 正确）；
- ✅ **N2** UE 吸附步长 = 10°（Blender = 5°），刻度循环与吸附共用该步长；长刻度保留独立 `fmod(deg,22.5)`，22.5 死代码消除；
- ✅ **N3** 组合 gizmo 拖拽时被拖轴画**全长 stem + ORIGIN 圆点**（move 1.415 / scale 0.775），空闲仍无杆；
- ✅ **N8** 四个 90° 宽标记用**被拖轴颜色**（RGB 轮换语义）；
- ⏳ P3 未做：**N4** 缩放拖拽 handle 随因子伸缩（可接受差异）、**N5** UE 中心球正交视图门控、**N6** Blender 头部文本格式（现 `"Rotation: %.2f deg"`，未去 deg/未加约束轴）、**N7** UE 旋转 HUD 锚点随 Δ 旋转；
- ⏳ P3 新增（2026-08-20 双方案评审吸收）：**Blender 模态变换（G/R/S）**——`ModalTransformController` 纯输入状态机（Idle→Active：鼠标实时更新 transform、X/Y/Z 约束轴（再按 = 局部轴，对齐 Blender）、数字键精确输入、LMB/Enter 提交、RMB/Esc 回滚到进入前 transform），与 `ManipulatorManager` 互斥激活，仅 Blender 风格启用（Unreal 保持点柄拖拽）；复用 `GizmoDragState` / `ViewportOverlayLabels`。**硬前置 = §B8 B2 快捷键冒烟通过**。反过度设计：不做 gizmo 风格接口化、不做 trackball/±360 clamp（F.7）；
- ⏳ F.7 backlog：trackball、拖拽隐藏其他轴+灰残影、UE 外圈环、象限 tick A=255 对齐、2D 虚线光标、±360 clamp/多圈 ghost；中心柄/view ring 真正可拖拽化；多选枢轴平均（现取 selection.back()）；主题/空间/snap 的 SettingsRegistry 持久化。F.7a 中心圆按用户约定不追。

## B8. Qt6 视口设计对照（状态账）

✅ 三层结构、表面生命周期、物理像素、DPI 切换、隐藏暂停、D&D 转发、MSAA resolve、常量缓冲一帧一次、depth 双 PSO 变体、`EngineViewportWindow::focusIn/focusOut → FocusChanged`（已补）等均落地；§B2 反例全部回避。原独立渲染 QTimer（含 `Qt::PreciseTimer`）已随单循环重构删除。

⏳ 待实测 / 待办偏差（均已决策）：
- **B1 主线程 present**（非 v1 独立渲染线程）：overlay/gizmo 顶点非线程安全，主线程同线程直画最快跑通；接口按渲染线程预留（生命周期钩子 + snapshot 括号 + `PackViewportSize` 原子字），切换不返工。切渲染线程时必须与 B4-InputPacket 一并落地。
- **B2 快捷键上下文**（冒烟测试第一优先）：Create Entity / Delete / 模式键用 `Qt::WidgetWithChildrenShortcut`（对齐 O3DE `EditorAction`，靠主窗口 `ActionContextWidgetWatcher` 拦截冒泡的 `ShortcutOverride` 触发）；CommandPalette 用 `ApplicationShortcut`。**架构风险**：本项目视口是 native QWindow 藏在 `createWindowContainer` 后，需实机验证 `ShortcutOverride` 能否穿过 window-container 边界冒泡到主窗口——到达则天然兼容；不到达则在 `EngineViewportWindow` 增设 QWindow→主窗口快捷键桥接层（或退而切 `ApplicationShortcut`）。
- **B3 文字标签 overlay**：`ViewportOverlayLabels` 透明 QWidget 叠 native 表面（与"native 表面上不叠 Qt widget"约束冲突，临时脚手架），Qt 是否提升、文字是否可见**未实机验证**；引擎字体图集 immediate-mode 落地后删除。
- **B4** InputEvent 仍持 `QEvent*` 同线程直传（v1 安全；上独立渲染线程前改值语义 POD，与 B1 捆绑，对齐 O3DE `QtEventToAzInputMapper` 边界）。
- **B5** macOS/Wayland `platformHandle` 分支**从未编译**（本地 Qt 6.11.1 Windows 包不含相关头）；收窄为 `Q_OS_WIN` 或标注。
- **B7** mime 白名单已接 ✅（`IsWhitelistedAssetPath`：rbfx `.mdl/.xml/.mat/.material/.ani`；Godot 资源待后端真实现后并入）；**B8** 视口锚定不可浮动（是意图，待写明）。

## B9. 资产 ✅/⏳

- 枚举 ✅：rbfx 项目目录 QDir 遍历；Godot 项目目录 + 扩展名过滤（.tscn/.gd/.res/…）。缩略图 v1 = 扩展名图标。
- ✅ **AssetBrowser 控件替换**（2026-08-17 落地）：官方 `AssetBrowserTreeView` + 注入式 entry 树 100% 复用，旧自建 QTreeView 已删除——终案与实现见 **`rbfx_migration.md`** §2。
- ✅ 拖放 → 视口 spawn（2026-08-17 落地）：`HandleAssetDrop` 落点 → `CreateObject` → 全量重镜像（`RefreshFromEngine`）；`.mat/.material/.ani` 走选中赋值。
- ⏳ Godot 源资源导入依赖 `EditorFileSystem`：文档流程缓解（首次先跑 `godot --headless --editor --quit-after 1` 生成导入缓存）；进程内跑 EditorFileSystem 为独立项。IAssetSource 升 SQLite 留后续。

## B10. 待办与验收（唯一清单）

**实测验收（最优先）**：
1. 点选全链路（rbfx + Godot）：点 mesh 精确选中、橙框、Outliner/Inspector 联动、gizmo 拖拽框跟随、Ctrl 多选、Select 模式纯净点选；确认修 8/9/10 后天空盒/灯光/旋转大盒不再抢选（含 "Geometry 100" 茶壶本体命中而旁边空白不误选）。
2. **回写引擎侧**（`ResolveNode` 统一 handle 路径后应 OK）：拖 gizmo / 改属性 / Undo → 引擎物体真实响应。
3. 非可视 node 靠 editor icon 点选（镜像实体需带 `EditorEntityIconComponent` 且 icon 未 hidden）。
4. Tracy 复测 pick-then-orbit：`ApplyPendingMove` 每帧 1 次、Dispatch 总耗时骤降、300-490ms 卡顿帧消失。
5. B2 快捷键（视口聚焦时 Ctrl+Shift+N / Delete / Q-W-E-R-T）、B3 文字标签可见性；及 dock 浮动/恢复、resize、最小化、HiDPI、关闭无 validation error 冒烟测试。
6. Gizmo 双风格 100% 对齐肉眼验收（对照 Blender 5.x / UE；含线宽可辨、UE 蓝 Z 不被 remap、Undo/Outliner 拖拽后 gizmo 归位）。
7. 幽灵镜像（`rbfx_migration.md` §3.6 已知限制）：删除→撤销→再次删除，`ResolveNode` no-op 无崩溃；重镜像/切场景后幽灵清除；对幽灵改属性不误写引擎；Godot 删除后 Outliner 不得残留（同步 free 修复）。

**功能接线**（✅ 2026-08-17 全部落地）：Create Entity / Delete 接 `CreateObject` / `DestroyObject`（桥接层监听实体删除回写引擎）；资产拖放 spawn；D&D mime 白名单；AssetBrowser 官方控件替换（`rbfx_migration.md` §2）。

**P3 打磨**：Gizmo N4-N7、F.7 backlog；Qt6 B4/B5/B7/B8。

**v2 及以后**：引擎侧增量场景同步（重同步先清旧镜像）；Godot 三角精拾（同一 `RaycastNode` 接缝）；dirty 按需渲染；独立渲染线程（与 B4 捆绑）；Play-in-Editor（`ISimulation`）；Godot 导入管线进程内；IAssetSource 升 SQLite；引擎场景 Prefab 化。

## B11. 非目标（v1，防过度设计）

- 不做跨平台（macOS/Linux）嵌入分支（HWND-only）；不做跨 API GPU 纹理互操作；不做多线程渲染管线（v1）。
- 不重写停靠系统；不做任意区域分割；不内置节点图/蓝图；不做包管理器；不引入遗留 Editor/CryCommon/Atom。
- 不把引擎类逐个静态注册进 AZ 反射；不自建属性 UI；不把属性值字符串化；不引入 entt::meta（引擎侧与 O3DE 侧各有成熟反射，数据流"引擎反射 → 类型化 PropertyBag → DynamicEditDataProvider → 网格"只一跳，entt 是冗余第三套且无法表达 per-instance 动态属性集）。
- 不把镜像实体写进 O3DE prefab（引擎原生场景为唯一真相）。
- 不做 hover 预高亮/框选/逐帧脏标记轮询；不缓存 selection AABB（点击低频、按需算）。
- 不引入 Godot 编辑器 gizmo BVH / 物理 `intersect_ray` 拾取（Godot 三角精拾留 v2）。
- 不上 dirty 按需渲染 / 不删 60fps 节流门（对齐 O3DE 新版视口；接缝已留）。
- 不为坐标转换器单拉 AZ 测试 target（端到端验收覆盖不变量）。
- 不做运行时热切换（切换 = 重启换 `--backend`；架构理由见 §A6 C6）。
- 不自建 O3DE 已提供的编辑器控件（§A6 C3；仅允许替换数据源）。

## B12. 愿景护栏与路线（2026-08-14）

- **E1 Atom 底线脚本化**：源码检查 `grep -rn "Atom" Code/Source`（应仅命中注释）+ CMake 依赖图检查（配置后核 target 的 LINK_LIBRARIES 导出，确认无 `Atom_*`）；固化为 `Scripts/check_no_atom.ps1` 纳入 pre-merge，把"当前成立"变"持续成立"。**已落地（2026-08-17，AssetBrowser review §2.6 驱动）**：三层断言 = 源码（去注释后区分大小写匹配）/ 声明（gem.json + Code/CMakeLists.txt）/ 构建（vcxproj 链接输入无 `Atom_*.lib` + Atom 工程引用仅限 `ReferenceOutputAssembly=false` 仅构建顺序引用），自验 PASS。
- **E2 可移除性固化**：C2 两项纳入 B10 实测验收（引擎头逃逸 grep + NullBackend 单后端冒烟）。
- **E3 `ISimulation` 契约草案**（占位；进契约前须 grep rbfx/Godot 各自的 play/pause 入口验证可实现）：`Play() / Pause() / Stop() / IsPlaying() / StepFrame()`；与 v2 清单（Play-in-Editor）对齐，变更同步全部后端（C4）。
- **E4 外部依赖预缓存技术债**：`O3DE_DEPENDENCIES_CACHED_*` hack 三处同机制（`External/CMakeLists.txt:191/289/411`：Diligent、rbfx `Urho3D`+`CrossEngine_Rbfx`、tracy），rbfx 是主后端、其预缓存最常用 → **rbfx 处优先验证**，O3DE 升级时全部验证/清除（C7）。同类升级敏感点：`GetLooseEditorEntities` 术语演进（§B5b）。
- **E5 新引擎接入 checklist 文档化**：按 C5 固化，已沉淀模板——嵌入两分支（SDL 直挂 vs reparent）、坐标转换单点收口、拾取三态接缝、bounds 语义拆分（选择/可见性两路径）、临时对象跳过落盘；下一个新引擎后端按 checklist 执行并回填文档。
- **E6 契约演进**：契约变更保持纯虚化强制（C4 硬手段），新增默认实现视为审查对象。

> 关键杠杆：DebugDisplay 三段分解（63→3）、领域模型 100% 复用 AzToolsFramework、靠不依赖来删除。
> 定稿：初始方案的设计原则不变，落地细节全部以当前仓库代码为准（与实现不符的历史表述——GL FBO/QOpenGLWidget、AttachToWindow/RenderFrame 旧签名、Godot 先/rbfx 后、ImmediateMesh 逐顶点、`BoundsRequestBus` 拾取、`PumpSystemEventLoopUntilEmpty` 还原、PreciseTimer 定时器、子树 union bounds——均已修正或删除）。
> 2026-08-14：愿景定稿（序言）、落地规则 §A6、护栏与路线 §B12 并入；AssetBrowser 100% 复用方案外链 `AssetBrowersPlan.md`。
