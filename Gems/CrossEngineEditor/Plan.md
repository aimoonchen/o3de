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
- **顶层边界**（2026-08-19 落账）：**CEE = 跨引擎的编辑器前端（UI/交互/工具链可复用），不是跨引擎的内容（场景仍引擎原生、不可移植）**。一条裁决**四处**下游：镜像实体不写 O3DE prefab（§B4）；撤销不能只靠 O3DE 实体状态快照（`rbfx_migration.md` §4 v1.5）；New/Open Level 走引擎场景（v2 首选）；**材质直接读写引擎原生文件，中立格式 `.cematerial` 与语义标签 `m_semantic` 驳回**（`material_migration_final.md` §9.1/§9.2，2026-08-25）。
  - **适用范围（2026-08-25 澄清）**：本条约束的是**编辑器运行时与后端契约**。**独立离线转换工具不受此限**——不进编辑器运行时、不进任何契约、不新增第三真相源，产出的是一次性内容而非常驻编辑器能力。跨引擎材质转换即按此归位（v2，映射表 `material_migration_final.md` §8.2）。

## A1. 设计原则（KISS 边界）
| 原则 | 含义 |
|---|---|
| 靠不依赖来删除 | 不 fork 遗留 Editor（Strategy B），从零依赖开始（无 gEnv/CryCommon/Atom）|
| 复用 > 抽象 > 新建 | 编辑器内核直接用 `AzToolsFramework`；只在引擎边界建抽象 |
| 一次写好，处处复用 | 引擎无关的部分（GenericDebugDisplay）写一次，所有引擎复用 |
| 后验证 > 先设计 | 先 NullBackend 跑通，再抽象接口 |
| 反射驱动 | 属性/序列化全走 AZ 反射，不手写 UI |

**核心判断**：O3DE `AzToolsFramework` 在编辑器领域模型上已达 Unity/UE 同级（反射 Inspector、Gizmo、EditorMode、Transaction 撤销、ActionManager 命令、Prefab）。**不重造这些轮子**。UX 缺口分两类（2026-08-27 复审修正，见 `editor_polish.md` §5）：**框架层缺失** = **Workspaces**（官方遗留 Editor 有 "Layouts" 菜单但依赖遗留代码，Strategy B 不采纳）与**命令面板**（O3DE 上游全仓核实无此设施）；**宿主侧欠账（能力在框架、接线在 CEE）** = `EditorRequests` 契约只实现 3/~40 个、ActionManager 零注册、ViewPane 注册表缺失——路线见 `editor_polish.md` §3。（初始方案承诺的"调整上一步"浮动面板**已取消**——未实现，Undo 栈已覆盖核心需求。）

## A2. 领域模型 → O3DE 设施映射（复用优先）
| 领域概念 | O3DE 现成设施 |
|---|---|
| 应用/生命周期 | `AzToolsFramework::ToolsApplication`（继承）|
| 文档/场景 | 关卡=根 Prefab（v1 框架占位壳；引擎原生场景=唯一真相，引擎场景 Prefab 化=v2）+ `EditorEntityContext` |
| 对象 | `AZ::Entity` + Component（引擎对象经镜像层映射）|
| 命令 / 撤销 | `UndoSystem` ✅ 已复用；`ActionManagerInterface` ⏳ 待接线（系统组件已激活、零注册，`editor_polish.md` §2） |
| 属性面板 / 大纲 / 资产 | `EntityPropertyEditor`+`EditContext` / `EntityOutlinerWidget` / `AssetBrowser` |
| Gizmo / 视口交互 | `Manipulators`+`DebugDisplayRequests` / `ViewportInteractionRequestBus` |
| 停靠 / 主题 | vendored Qt-Advanced-Docking-System(默认) · `AzQtComponents::FancyDocking`(fallback) / `StyleManager` |

我们只写"外壳**接线**（菜单/工具栏/右键/快捷键注册）+ 引擎后端边界 + 两个 UX 增强"（接线范围与路线见 `editor_polish.md`）。

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
- **C3 复用优先**（2026-08-25 澄清本意）：**本条的本意是"不重复造轮子"，不是"禁止扩展"**——O3DE 框架与控件库能满足设计需求时一律复用；**当框架/控件满足不了设计需求，且该需求不属于过度设计时，允许自行扩展框架功能与自定义控件**。三级优先序：**① 直接复用 stock** ＞ **② 扩展 stock**（转发已被消费的属性 / override 现成虚钩子 / 子类化——判例：材质面 S2 `m_colorSpace`、S5 `m_fileExtensions`、S6 `m_suffix`、`InspectorWidget::ShouldGroupAutoExpanded`）＞ **③ 自建控件**（前两级确实做不到时）。复用控件与其数据来源冲突时仍是**只替换数据源，不替换控件**（判例：AssetBrowser，`rbfx_migration.md` §2）。
  - **判定手段（保留牙齿）**：走到 ② 或 ③ 的，须在方案文档落两条**带证据**的记录——**(a) stock 为什么满足不了**（必须带 `文件:行号`，不得以"我记得没有"代替 grep）；**(b) 该需求为什么不是过度设计**（对标手段：目标引擎的原生编辑器是否如此呈现，或既有判例）。缺任一条即视为违例；临时脚手架另需删除计划（如 §B8 B3）。
  - **反面判例（2026-08-25 落账）**：材质方案初稿曾以"违 C3、需自建控件"为由砍掉单位后缀显示，实测 `AZ::Edit::Attributes::Suffix` 早已被 slider/spin/int/vector 四族 stock 控件消费，属 ① 级白送（`material_migration_final.md` §3.4b / §11.4-5）。**把"我没查"说成"规则不让"，是本条最需要防的误用**——(a) 项的 `文件:行号` 硬要求即为此设。
- **C4 后端契约 = 愿景核心资产**：引擎能力须先进契约再进后端；契约变更须在同一变更内更新全部后端。**核心方法 = 纯虚**（漏改后端在编译期爆炸）；**能力接缝 = 哨兵默认**（`FinishSync`/`SaveScene`/`GetWorldBounds`/`RaycastNode`），默认值只能是"不支持"语义、不得是真实行为——新增任何默认实现须 review 标注（E6）。契约禁止引擎专属语义（空间约定 O3DE Z-up RH 米，转换收口后端边缘）。**归一化适用范围（2026-08-25 澄清）**：仅适用于**编辑器自身要参与计算的量**（transform / bounds / 相机 / 拾取射线——gizmo 与视口都得拿它算，不归一就得在每个消费点分别处理引擎差异）；对**编辑器只显示与路由、从不参与计算的量**（材质属性值、单位、色彩空间）**不归一**，照引擎原生语义直出——归一化在这里没有消费者，只会平白复制 §B4 那条已知限制的代价（2026-08-20 落账：Inspector 显示 O3DE 契约值，与引擎原生编辑器数值不一致）。理由与三类差异归位见 `material_migration_final.md` §8.4③。**IPC 数据面红线**：数据面契约（`IEntityMirror`/`IAssetSource`）不得引入 Qt/窗口系统类型（保持 POD/可序列化——未来 Unity 托管桥/沙箱后端 = 传输层替换而非重设计）；渲染面（`ISceneRenderer` 的 `OnSurfaceCreated(void*)` 表面共享是进程内固有语义）显式豁免。`ISimulation` 契约补全见 §B12 E3。
- **C5 接入新引擎成本显式化**：接一个新引擎 = 实现 `IEngineBackend` + 三子契约 + 坐标转换收口调用。**渲染面**成本 = DebugDisplay 三段分解（63 方法 → 3 原语）；**编辑面**（`IEntityMirror` **19 方法** = 15 纯虚 + 4 哨兵默认——镜像/资源/剪贴板/拾取/创建销毁）才是接入成本大头，纯虚允许 stub（基线：Godot stub 10/15；v2 契约瘦身目标 ≤3，E2 方案见 `rbfx_migration.md` §4）；`IAssetSource` = 2 枚举纯虚。**材质面**（2026-08-25 新增）：`IMaterialSource` = **8 纯虚 + 2 哨兵**（`SetPreviewModel` / `AcquirePreviewImage`，预览走 RTT 按需回读不进 `ISceneRenderer`）+ 1 份引擎 schema（Godot 从 `get_property_list()` 反射零手写；rbfx/Filament 各一份策展 JSON **数据**，加参数不重编译）——约为编辑面的一半，**设计目标 stub 率 0**（终案 `material_migration_final.md` §4 契约 / §6.4 / §7.6 成本表）。踩坑沉淀模板见 §B12 E5。
- **C6 切换语义**：v1 切换 = 启动参数 `--backend`（重启切换）；**运行时热切换为非目标**——技术事实：rbfx/Godot 均在进程内持有 swapchain + native 窗口句柄 + 全局单例状态，热切需销毁重建整个视口表面与镜像层，且 `AZ::Interface<IEngineBackend>` 单实例注册模型不支持并存。
- **C7 不依赖 O3DE 内部（A0 扩展）**：从"公开头文件与反射"扩展到 CMake/构建层——不依赖 O3DE 内部私有属性名。现行违例：外部依赖运行时遍历器预缓存 hack（依赖 `O3DE_DEPENDENCIES_CACHED_*`，Diligent/rbfx/tracy 三处，见 B1），列为技术债（§B12 E4）。

---

# Part B — 当前落地方案（以仓库代码为准）

> 标注：**✅ 已落地** / **⏳ 待办或待实测** / **v2 后续**。

## B1. 总架构与契约 ✅

编辑器壳**面板**（Outliner/Inspector/Gizmo/Undo/Prefab/AssetBrowser/Console）复用 O3DE 框架 ✅；**命令层**（菜单/工具栏/右键/快捷键）待接入 ActionManager ⏳（`editor_polish.md` P0/P1）；Workspaces + 命令面板已自建 ✅。引擎以**进程内库**接入：

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
- **命令面板**：`Ctrl+P`（`Qt::ApplicationShortcut`）模糊搜索并执行。**当前数据源 = 遍历 menuBar 的手写动作（~20 个）**；ActionManager 接入后改枚举全部已注册动作（`editor_polish.md` P1-11）。

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
- **B2 快捷键上下文**（冒烟测试第一优先）：Create Entity / Delete / 模式键用 `Qt::WidgetWithChildrenShortcut`（对齐 O3DE `EditorAction`，靠主窗口 `ActionContextWidgetWatcher` 拦截冒泡的 `ShortcutOverride` 触发）；CommandPalette 用 `ApplicationShortcut`。**架构风险已裁决（2026-08-27 D13 R2，源码复核取代实机猜测）**：`ActionManager` 两级 watcher 只认 QWidget 的 `ShortcutOverride`（`ActionManager.cpp:31/96`），视口是裸 QWindow（`EngineViewportWindow.cpp:192-197` 只转发 KeyPress）→ **确认不到达**，桥接层不是兜底而是必做项——已排入 `editor_polish.md` P0-2「快捷键上行桥」（~15 行：合成 ShortcutOverride + sendEvent 容器 + `AssignWidgetToActionContext`），实机验收兜底。
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
3. **非可视 node 视口可见可点（2026-08-27 D13 改判）**：视口 billboard 图标在 CEE 撞 C1 结构性不可实现（`EditorViewportIconDisplayInterface` 全仓唯一实现者是 Atom Gem）→ 改走**线框 gizmo**（`editor_polish.md` P0-6：灯光/相机/空节点按 `m_className` 派发 WireSphere/WireCone/WireBox 等绘制，零契约改动，调用点现成 `EditorHelpers.cpp:312/316`）。
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
- 不做 hover 预高亮/逐帧脏标记轮询；不缓存 selection AABB（点击低频、按需算）。**框选改判（2026-08-27 D3，设计定稿）**：复用 `EditorBoxSelect`（①级 stock，Atom-free 输入状态机），列 `editor_polish.md` P2——原「不做框选」属 C3 反面判例同型（把「我没查」写成「规则不让」）。判定与语义吸收业界共识：屏幕投影 bounds-overlap（替代 stock 位置点测试，UE/Blender/Unity/Godot 一致）、普通=替换/Ctrl=并集/Ctrl+Shift=差集、松手结算单 undo 命令；marquee 经 QPainter 走现有 Qt overlay 绘制（`GenericDebugDisplay` 未实现 2D 图元，`DebugDisplayRequests` 路径在 CEE 画不出来）。lasso/遮挡剔除/视锥筛选不做。
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
- **E7 复用接线路线（2026-08-27 新增，D13 更新）**：`editor_polish.md` = 原生编辑器复用复审议程唯一终稿——EditorRequests 12 方法 + ActionManager 引导 + 快捷键上行桥 + 非可视线框 gizmo + ViewPane 薄注册表 + 菜单/右键/Undo/Redo/View/布局/保存安全，P0≈8.6 + P1≈7.1 + P2≈10.0 + P3≈0.8 ≈ **26.5 人日（约 5.3 人周）**；另 rbfx v1.5（undo 命令化+身份稳定化，3~5 人日）为 Undo 开门硬前置，挂 `rbfx_migration.md` §4。

> 关键杠杆：DebugDisplay 三段分解（63→3）、领域模型 100% 复用 AzToolsFramework、靠不依赖来删除。
> 定稿：初始方案的设计原则不变，落地细节全部以当前仓库代码为准（与实现不符的历史表述——GL FBO/QOpenGLWidget、AttachToWindow/RenderFrame 旧签名、Godot 先/rbfx 后、ImmediateMesh 逐顶点、`BoundsRequestBus` 拾取、`PumpSystemEventLoopUntilEmpty` 还原、PreciseTimer 定时器、子树 union bounds——均已修正或删除）。
> 2026-08-14：愿景定稿（序言）、落地规则 §A6、护栏与路线 §B12 并入；AssetBrowser 100% 复用方案外链 `AssetBrowersPlan.md`。
> 2026-08-25：材质编辑器跨引擎化终案 `material_migration_final.md` 落账（四方裁决合并稿）。同批三处**澄清**（划清适用范围，非放宽）：A0 离线工具豁免、C4 归一化适用范围、C5 材质面接入成本。同日两条**裁决**（见下两行）：形态 = dock 面板（非独立 exe）；预览 = RTT 按需回读（非多表面）。
> **护栏表述修正（2026-08-27 裁决）**：2026-08-25 的『第 1/2 层已精化 + 白名单』表述与脚本实际不符——`Scripts/check_no_atom.ps1` 仍是裸词 `-cmatch 'Atom'`（95 行内零命中 AtomToolsFramework/RPI/RHI）。C1 原文不动。**裁决：表述以现状为准（本行即修订后表述）；精化脚本不是 P0 前置杂项**——它归材质编辑器实施轮执行（`material_migration_final.md` §3.6 已含精化 spec：第 1 层禁 `Atom/RPI,RHI,Feature,…` + 白名单 `Gem::AtomToolsFramework.Core` + 第 3 层构建断言不变），触发点 = `Code/Source` 真实出现 `#include <AtomToolsFramework/…>` 的那一轮；触发前脚本保持裸词匹配（当前零命中，护栏有效，不预改）。
> **已裁决不做**（2026-08-25）：不给「本工具编 Atom 材质」开 target 口子——原版 MaterialEditor 已覆盖，收益≈0，而单底线一旦有例外护栏即打折。
> **C3 本意澄清（2026-08-25，用户亲自口径）**：C3 反对的是「重复造轮子」而非「扩展」——框架/控件满足不了设计需求、且需求不属于过度设计时，**允许扩展框架与自建控件**。规则改写为三级优先序 + 两条带证据的判定手段（见 §A6 C3），并把材质方案初稿的两次误用落为反面判例。**同日据此推翻一条当天早些时候的裁决**：Godot 组头勾选框由「维持降级」改为**采纳**——实测 `BaseMaterial3D` 用了 15 次 `PROPERTY_HINT_GROUP_ENABLE`（`material.cpp:3603-3734`，opus 复核 grep -c 订正），且落地形态是 ②级扩展（上游 3 行 `virtual CreateGroupHeader()` + CEE header 子类），并非 ③级自建。详见 `material_migration_final.md` §9.4 / §11.6。
> **材质编辑器形态裁决（2026-08-25）**：由「独立 exe」改判为 **CEE 内 dock 面板**。用户先纠正前提（业界常态是进程内编辑，O3DE 双进程是孤例），源码核实再推翻初稿两条承重理由（双 `AzQtApplication` 不承重——本就不继承 `AtomToolsApplication`；崩溃隔离不成立——引擎已在进程内）。dock 形态下 `CrossEngine::EditorFramework` 静态库抽取**取消**（§B2 路线不含该目标时，`material_migration_final.md` §6.2）。
> **预览渲染路径裁决（2026-08-25，用户拍板）**：**RTT + 按需回读**，不做第二 OS 表面。核实结论：多表面 = 同时 fork rbfx（RenderPipeline 主链硬编码 4-5 处）与 Godot（`display_server_windows.cpp:1855` parent 写死 null），且**两个引擎自己的编辑器都不这么做**（连分屏都不开第二窗口）；RTT 回读 = 零引擎改动、两引擎均裁决 (a) 现成可用、上游各两处同款先例。契约以第 2 个哨兵承接（`AcquirePreviewImage` + `PreviewResult` 三态），`ISceneRenderer`/`IViewportTick`/单节流主循环**零改动**；「按需渲一帧、绝不空闲回读」（两引擎停顿点同构：`RawTexture.cpp:1017` / `rendering_device.cpp:2729`）写入契约语义。详见 `material_migration_final.md` §6.4 / §9.9 / §11.6。
> **原生编辑器复用复审落账（2026-08-27，用户拍板）**：三稿合并唯一终稿 = `editor_polish.md`（deepseek/opus/kimi 过程稿已删）。
> - **两根因**：① `EditorRequests` ~40 虚方法只实现 3 个（`IsLevelDocumentOpen`→false → Outliner 右键永不弹，`EntityOutlinerWidget.cpp:582-586`）；② ActionManager 系统组件已激活但零注册、`TriggerRegistrationNotifications()` 从未调用（原生 `CryEdit.cpp:1676`）→ 无 Undo/Redo 入口（驱动 = `ToolsApplicationRequestBus::UndoPressed/RedoPressed`）。能力在 AzToolsFramework（Atom-free），接线在 EditorLib（绑 6 个 Atom target 撞 C1）→ CEE 复用了控件没写接线。
> - **裁决 D1–D12 全量再确认**：D1 自建 ADS 薄壳（不引 EditorCore）；D2 菜单 File/Edit/View/Tools/Help + Game 缓（sortkey 预留）+ Window 并入 View + 混合 ID 命名（`o3de.menu.editor.*`/`o3de.action.*`/`cee.*`）；D3 框选复议通过（复用 `EditorBoxSelect`，§B11 已同步改判）；D4 P0 八项；D5 ViewportUi 悬置；D6 ViewPane = P1 薄注册表 + Tools 菜单（业界最佳实践：Unity/UE/Blender 均为编辑器类型注册）；D7 不引 PrefabIntegrationManager；D8 状态栏 P1 精简四格；D9 Preferences = P2 对话框 + SettingsRegistry 持久化先行；D10 Python 面板不接；D11 §5 冲突清单本批已同步修订；D12 A1–A9 全采纳、A10 留 P2。
> - **工作量**：P0≈6.5 + P1≈9.5 + P2≈8.5 + P3≈1 ≈ 25.5 人日（5.1 人周）；P0+P1 零 CMake 改动、零新依赖。验收尺子 8 条见 `editor_polish.md` §7。
> - **第三方独立复审吸收（D13，2026-08-27 用户拍板）**：`review.md`（R1–R3/S1–S4/M1–M2/E1–E6）全部承重论据源码复核通过（100%，仅两处引用路径勘误），全项落账 `editor_polish.md` D13。要点：Undo/Redo 改**占位置灰**（RefreshFromEngine 重建镜像作废撤销历史，「宁可禁用不可半残」，rbfx v1.5 开门）；快捷键**上行桥**入 P0-2（§B8 B2 风险已源码裁决，桥是必做项）；非可视线框 gizmo 入 P0-6（§B10-3 改判）；保存安全（.bak+原子替换）入 P0-8；ViewportUi 改判视口 header bar（D5 关闭）；状态栏四格改两格（D8 修订）；动作 ID 全 `cee.action.*`（D2b 修订）；后端动作接缝（M1）入 P1；E1–E5 事实修正（HotKeyManager 无持久化/AzQtComponents 41 头且 Style 已生效/右键四处非三处/图标三路径）；**工作量更新：P0≈8.6 + P1≈7.1 + P2≈10.0 + P3≈0.8 ≈ 26.5 人日**；尺子重写为质量尺 9 条（`editor_polish.md` §7），不建自动化测试 target（人工点检入验收）。
