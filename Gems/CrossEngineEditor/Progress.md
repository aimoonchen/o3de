# CrossEngineEditor 进度存档（Progress）

> 本文 = 项目**背景与阶段总览**（Part A，交接 prompt 提炼）+ **当前实现明细**（Part B，反映当前仓库代码）。
> 权威方案见 `Plan.md`。所有内容已按当前代码整理，删除过时诊断过程与被后续修复取代的旧结论。

---

# Part A — 项目背景与阶段总览

## 目标（别跑偏）
把 O3DE 编辑器框架 + 完整编辑器功能，做成"跨引擎 all-in-one 游戏编辑器"。底层引擎可替换（自研/Godot/UE/Unity），
编辑器不依赖任何具体引擎运行时（不要 Atom/CryCommon/CrySystem/gEnv/LmbrCentral），引擎经"后端契约"接入。

## 关键决策（不要推翻）
- **Strategy B**："靠不依赖来删除，而非剥离依赖"。不 fork 遗留 Editor.exe，新建薄外壳直接构建在 `AzToolsFramework` 之上（自带 EditorEntityContext + Prefab + Undo + 反射）。
- 只做现代 EC/Prefab 编辑工作流，不复刻遗留 Editor.exe 全部工具。
- 第一方新代码 **C++23**。KISS、不过度设计。能用原版逻辑/语义就以原版为准，不臆测自造。
- gizmo 两套风格 Blender/Unreal，视觉+操作 100% 对齐官方，几何/配色只取官方源码逐值真值。

## 阶段进度
| 阶段 | 状态 |
|---|---|
| 0 骨架 / 1 面板+NullBackend / 2 GenericDebugDisplay+相机+网格 | 完成（M1/M2）|
| 3 Prefab/关卡工作流 | 代码完成 |
| 4 对象模型+资产（SQLite 升级留后续）| 代码完成 |
| 4.5 gizmo 自绘子系统（双风格+交互辅助+Local/World+Snap）| 代码完成，gizmo 视觉待肉眼验收 |
| 5 首个真引擎后端：**rbfx 先**（代码完成，编译链接通过）、**Godot 随**（已落地主体）| 端到端肉眼验收待做 |
| 5.x 点选 + 帧循环单循环重构 | 已落地 |
| 6 打磨/多引擎/Play-in-Editor | 打磨方案定稿（`editor_polish.md`，2026-08-27，P0–P3 ≈ 26.5 人日，零 CMake 改动）；多引擎/PIE 后续 |

## 启动/析构顺序（务必保持）
```
StartCommon → 连 EditorRequestBus → 注册后端 → 建 EditorMainWindow → 连 EditorWindowRequestBus
  → SetHandler(装 CrossEngineViewportSelection，非 SetDefaultHandler) → CreateNewLevel() → show()
Destroy: 断总线 + reset m_mainWindow → Unregister + reset backend → ToolsApplication::Destroy
```

## 编译坑（保留，防重犯）
- **Unity 分组的传递 include 陷阱（2026-08-31）**：新增源文件会改变 unity 批分组——`CommandPalette.cpp` 曾依赖批内前序文件传递 include `AzCore/PlatformDef.h` 才能用 `AZ_PUSH_DISABLE_WARNING`，新文件使其成为批首文件后暴雷。规则：**每个用 AZ_PUSH/POP_WARNING 的 .cpp 必须自 include `AzCore/PlatformDef.h`**。
- **应用类成员指针 C2247**：`CrossEngineEditorApplication` 从 `ToolsApplication` **私有**继承 EBus handler，`&AzToolsFramework::ToolsApplicationRequests::Xxx` 成员指针形成会被拒——直调公共 override（如 `AreAnyEntitiesSelected()`）或把广播留在非派生类（EditorMainWindow 等）里。
- `AZ::Color` 四浮点构造为模板且非 constexpr——命名空间级配色常量用 `const`。
- `GenericDebugDisplay` C4266：override 基类重载对（DrawArc/DrawPolyLine/DrawQuad/DrawWireSphere）会隐藏另一个 → 已 `using AzFramework::DebugDisplayRequests::Xxx;`。
- FancyDocking/ADS 会重建原生表面：surface 生命周期信号必须 `OnSurfaceAboutToBeDestroyed` 同步释放 swapchain 再返回，否则崩。
- `EditorGrid`：中心整格吸附到相机在 z=0 的投影，网格锁死世界坐标不随相机滑动。
- Unity build：跨 .cpp 匿名 namespace 同名符号冲突（已消除）；`QFileIconProvider/QFileInfo` 拉 winsock 与 WinSock2 冲突 → 入 `SKIP_UNITY_BUILD_INCLUSION_FILES`。
- MSVC：`fopen`→`fopen_s`（C4996）；`AZStd::make_unique` 在 `<smart_ptr/unique_ptr.h>`，`make_shared` 在 `<smart_ptr/make_shared.h>`。
- 所有 .cpp/.h 注释必须纯 ASCII（C4819 warning-as-error）；数学符号用 `~=`/`->`/`deg`。
- O3DE Transform 只支持 uniform scale（`GetLocalScale` 已 deprecated）→ 单轴 scale 走 `SetLocalUniformScale` 是框架限制非 bug。

---

# Part B — 当前实现明细（以仓库代码为准）

## 铁律
- 绝不臆想 API（rbfx/Godot/O3DE 接口须 grep 真实头核实）；KISS，能用原版语义就用原版。
- BackendAPI 边界恒 O3DE 约定（Z-up 右手 米），各后端在自身边缘转换。
- 路径：repo `G:\o3de`；rbfx `I:\rbfx`；godot `I:\godot`（Godot 4.8，libgodot）；Blender `I:\blender`；Unreal `I:\UnrealEngine`。

## 编译 / 运行
- 编译：`cmake --build build/windows --config profile --target CrossEngineEditor`（产物 `build\windows\bin\profile\CrossEngineEditor.exe`）
- 运行：`CrossEngineEditor.exe --backend rbfx|godot|diligent|null --project <工程> --scene <相对资源根路径>`
  - rbfx 验证：`--backend rbfx --project I:\rbfx\bin --scene Scenes/RenderingShowcase_0.xml`
  - **场景路径必须正斜杠且相对资源根**（`Scenes/...` 不是 `Data\Scenes\...`），否则镜像 0 节点；正确时镜像 141 节点。
- **profile 直接运行 `AZ_Assert` 只警告不中断；Debug 挂调试器才 `DebugBreak`**（排查启动/点击崩溃优先 Debug + 调试器抓栈）。

## 已实现能力（均 profile 编译通过）

| 子系统 | 状态 |
|---|---|
| 后端工厂 | `--backend rbfx\|godot\|diligent\|null`；rbfx 与 Diligent 编译期互斥（默认 rbfx）；Godot DLL 运行时 `LoadLibrary`（构建期不链接）|
| rbfx 渲染 | `EP_EXTERNAL_WINDOW` 直挂 Qt HWND（无子窗口/无 reparent）；`RunFrame` 在 `EndOverlayFrame`；根节点 `DebugRenderer` 3 原语；相机节点同步 + `SetViewport` |
| Godot 渲染 | libgodot + `--wid` + `SetParent` reparent（`WS_CHILD\|WS_VISIBLE\|WS_DISABLED` + `WS_EX_NOACTIVATE`）；vsync 关；`iteration()` 在 `Tick`；ArrayMesh 批量 overlay（顶点色材质，material_override 保活）|
| 对象镜像 | `SyncFromEngine` 一次性全量（`AddRequiredComponents→AddEditorEntity→FinalizeEditorEntity→FinishSync SetParent`）；`EngineNodeComponent`（className / PropertyBag / nodeHandle 反射 Version 2）|
| 属性反射 | PropertyBag 10 子类 + `DynamicEditDataProvider`（零自定义 handler）；rbfx `GetAttributes` / Godot `get_property_list`；属性改动整包回推（变更总线只报 componentId）+ `m_syncing` 抑制位 |
| 双向回写 | Gizmo 拖拽 → `TransformBus` → `OnEditorTransformChanged`；属性 → `OnEditorPropertyChanged`；节点解析统一 `ResolveNode`（handle 路径，`EntityMirrorBridge` 挂总线驱动）|
| 点选 | `EditorComponentSelectionRequestsBus`（4 方法，实现 3）；`FindVisibleEntities` handler；`GetWorldBounds` 仅本节点（排除 Skybox/Zone/Light）；rbfx 三角精拾（`RaycastNode`）；bounds transform 失效缓存；选中框橙框 |
| Gizmo | 双风格 Blender/Unreal（`GizmoTheme/GizmoViews/GizmoManager/CrossEngineViewportSelection`）；`GizmoControlRequestBus`；`GizmoMode::Select`(默认) + Q/W/E/R/T + 1/2/3/4；World/Local；Snap 开关 |
| 相机/网格 | 自建 `EditorViewportCameraController`（orbit/pan/dolly/fly）；`EditorGrid` 世界 overlay |
| 帧循环 | 单循环 `OnIdle`（1ms/100ms 自旋）+ 60fps 节流门；无手写 Win32 泵；鼠标 move 每帧合并；一帧一次 present |
| Prefab/关卡 | New/Open/Save Level（`.prefab`）；`CreateNewLevel` 启动即调；Ctrl+P 命令面板；Save/Restore Workspace（停靠 saveState + QSettings）|
| 停靠 | vendored **Qt-Advanced-Docking-System 唯一路径**（v5.1.1 submodule pin，缺失即 configure FATAL——`dock_style_review.md` 3-① 裁决后 FancyDocking fallback 分支已删）；面板经 ViewPaneRegistry 注册（editor_polish P1-14） |
| 命令层 | **ActionManager 全量生成**（editor_polish P1-12）：五顶层菜单+三 context menu+Create/Recent 子菜单，动作 `cee.action.*`，后端命令经 GetActionRegistrationPatterns 表注册（M1）；快捷键上行桥使视口聚焦时全部生效 |
| 持久化 | `SaveScene` 已实现并接菜单：rbfx `SaveXML` / Godot `PackedScene`+`ResourceSaver`；镜像不写 prefab；**.tmp+.bak 原子替换**（P0-8）；脏标记/Recent/布局会话槽/Preferences 均落盘 |
| 资产 | 双引擎目录枚举 + 扩展名图标 |
| Tracy | `CEE_ENABLE_TRACY`（采样默认 OFF）；FrameMark 在节流门内；统一头 `Profiling/CrossEngineProfiler.h` |

## 关键修复记录（压缩）

| # | 问题 | 修复 |
|---|---|---|
| 1 | 点击不选中（visibleCache=0）| viewport 补 `EditorEntityViewportInteractionRequestBus::FindVisibleEntities`（返回全部 `EngineNodeComponent` 实体；不做视锥剔除、不依赖 octree）|
| 2 | Debug 启动断言（重复反射）| 移除 `Reflect()` 手动调用，只走 `RegisterComponentDescriptor` |
| 3 | 点击一次崩溃（空指针）| `CrossEngineViewportSelection` 补构造 `ActivateMode(Default)` / 析构 `DeactivateMode`（替换 `EditorDefaultSelection` 必须）|
| 4 | 点不中小物体（子树巨盒抢选）| 后端 `GetWorldBounds` 改**仅本节点**（SelfDerived / 仅 GeometryInstance3D）|
| 5 | 双轨节点解析矛盾 | 统一 `ResolveNode(entityId)` handle 路径 + miss `AZ_Warning`；删正向 map 解析源 |
| 6 | 注释/文档漂移 | `BoundsRequestBus` 注释去"拾取必需"错误论断；`GetWorldBounds` 注释勘误为"仅本节点" |
| 7 | 小项 | Godot `get_aabb` 补 `is_class` 守卫；删死成员 `m_hoveredEntityId`；Snap toggle 只翻 enabled |
| 8 | 非可视 node 隐形小盒抢选 | bounds 语义拆分：selection/pick 用原始 bounds（非可视=null 不可拾取）；visibility 路径保留 pivot 小盒 |
| 9 | 天空盒/Zone/灯光抢选 | rbfx `IsInstanceOf("Skybox"\|"Zone"\|"Light")` 排除（Godot 天然免疫）|
| 10 | 旋转大 mesh 膨胀 AABB 抢选 | rbfx 三角精拾：`RaycastNode` 三态接口 + `Drawable::ProcessRayQuery(RAY_TRIANGLE)`（AABB 粗剔后三角 hit/miss 权威）；Godot 走默认（AABB 级）|
| 11 | Godot vsync 阻塞 present | 一次性 `window_set_vsync_mode(VSYNC_DISABLED)`，宿主节流门限帧 |
| 12 | Godot 空转 ~500fps + 与 present 拍频尖刺 | 并入单循环 60fps 节流门（`backend->Tick` 只在门内调）|
| 13 | 选中后 orbit 卡顿（300-490ms）| 根因 = Win32 鼠标消息洪流（700+ 条/帧）。修复：**删除 OnIdle 手写泵（交还 Qt）+ 输入层每帧合并（`ApplyPendingMouseMove`）** |

**已证伪的旧结论（勿据此回改）**：①"GodotApi::Call 封送是卡顿核心"——每帧 GDExtension Call 稳定 9 次、不随 orbit 放大，真正根因是消息洪流；②"还原 `PumpSystemEventLoopUntilEmpty`"被否决——原生 O3DE 编辑器 OnIdle 零消息泵，正确做法是**删除**手写泵；③"Qt 有默认鼠标移动压缩"——Qt 6.11 对普通鼠标 WM_MOUSEMOVE 无逐帧压缩，兜底靠业务层合并。§13 的 overlay 批量 / StringName 缓存 / bounds 缓存本身是正当优化，保留。

## 架构备忘：两后端视口嵌入为何不同（写死）

权威最佳实践 = 引擎 RHI swapchain 直接 present 到 Qt 提供的 native surface（Qt 拥有窗口、引擎往里画）。
**rbfx 100% 采用**（`EP_EXTERNAL_WINDOW`=Qt HWND，无独立窗口/无 reparent）；**Godot 架构上无法采用**——libgodot 只能自建窗口（`--wid` = `CreateWindowExW` hWndParent + `set_embedded_in_editor(true)`，非"渲染到外部表面"API），故取其自建窗口 HWND 再 `SetParent` reparent 进视口（把 owned-popup 转真子窗口），输入靠 `WS_DISABLED` 子窗口路由回 Qt（Godot 官方"编辑器内嵌游戏"是跨进程浮窗，不适用于同进程 libgodot）。**两后端不统一、也无需统一。**

## 待办 / 验收清单

**2026-08-31 实施轮（editor_polish.md P0+P1+P2+P3 全量落地，全部记录见该文 §12）**：
- **P0**：Console 补 tab / 快捷键上行桥（KeyPress 副本 sendEvent 主窗口，Qt 自动合成 ShortcutOverride）/ EditorRequests 12 方法 / ActionManager 引导（`Window/CeeActionsHandler.{h,cpp}`）/ 三个 context menu + 视口右键 / 非可视节点线框 gizmo（rbfx RaycastNode 灯光修复）/ Undo/Redo 占位置灰 / 保存 .bak+原子替换（rbfx+Godot）/ View=面板开关 / 布局会话槽。
- **P1**：手写菜单栏整体退役（MenuManager 生成五顶层菜单，全动作 cee.action.*，热键走 HotKeyManager，选择敏感装 enabled 回调+updater）/ 后端动作接缝 M1（IEngineBackend::GetActionRegistrationPatterns 表驱动，rbfx Export Prefab 首个表项）/ ViewPane 薄注册表（`Window/ViewPaneRegistry.{h,cpp}`，五面板工厂化，Tools 菜单+View 开关注册表驱动）/ 命令面板=菜单栏∪context menu 枚举 / 主工具栏（注册 QAction+addMainToolBarStyle）/ 状态栏两格 / 视口 header bar（gizmo 控件迁入+SegmentControl）。
- **P2**：框选（stock 状态机+bounds-overlap 判定+QPainter marquee）/ Preferences（`Window/CeePreferences{,Dialog}.{h,cpp}`，SettingsRegistry 持久化+ReflectedPropertyEditor+Card）/ 快捷键重绑定（SetActionHotKey+QSettings 自管）/ Recent / 脏标记+标题+诚实关闭框 / 自动保存+Toast / Outliner+AssetBrowser 展开态（TreeViewState，**rbfx §3.6 v2 待办清账**）/ Help 外链。
- **P3**：主题化 Save/Export 对话框；**ProgressShield 缓办**（v1 无分块长任务，LegacyShowAndWait 包不住阻塞同步——带理由落 editor_polish §12.4）。
- **回归**：profile 0 error ✅ / check_no_atom 三层 PASS ✅ / NullBackend 冒烟（新工具 `Scripts/cee_smoke_null.ps1`，15 秒存活）✅。**修复两个先于本轮的启动崩溃**：① Console 无父构造踩 BaseLogPanel pParent->layout()（LogPanel_Panel.cpp:105，提交版本即如此——Console 落地后从未启动验证过）；② ComponentModeCollectionInterface 无人注册（EditorDefaultSelection 被替换后其注册义务未继承，LmbrCentral 注册钩子解引用 null）——CrossEngineViewportSelection 现持有空 ComponentModeCollection 镜像 stock。
- **实施后第三方审查吸收（2026-08-31，`review_editor_polish_deepseek.md`，全记录=editor_polish.md §12.6）**：1🔴+4🟠+9🟡 已修复——🔴 Preferences 持久化锚点错配（dump 无根键前缀+merge 默认锚根，改传 `/CEE/Preferences` 锚）；🟠 命令面板 QSet 去重/脏回调析构清空/AssetBrowser 回退死路删除/快捷键清除改直清 QAction；🟡 Godot tmp 残留/rbfx 回滚诊断/线框分类改精确名（**修正审查方论据：rbfx 是单一 Light 类非三类**）/Create 首开即时刷新/浮动面板语义对齐/SaveLevel 静默分支/调试打印/RegisterPane 单次查找/落盘警告。维持：header bar 裸动作不入面板（知情边界）。
- **新编译坑**：QStringLiteral 只吃字面量；EnumAttribute 必须真枚举；CreateTreeViewState 返回 unique_ptr 用赋值。
- **Dock/Style 专项复查吸收（2026-09-01，`dock_style_review.md`，全记录=editor_polish.md §12.7）**：三项建议全采纳——① **fallback 双路径收敛已执行**（ADS 缺失 WARNING+降级 → FATAL_ERROR；CEE_HAVE_ADS 宏删除；EditorMainWindow 13 处 #else 分支+FancyDocking 成员删除；理由：后端降级是 C2 可拔插设计、dock 是 UI 基础设施，fallback 是从未编译过的半残路径）；② 两条缝隙落账（**ADS 面板不吃 O3DE 主题=固有代价接受并存不做**；InputDialog 无消费者缓办）；③ 样式侧零调整（零自写 QSS 确认）。回归复验：编译 0 error + check_no_atom PASS + 冒烟 ALIVE。
- **待人工实机点检**：editor_polish.md §7 尺子 1–9 + **新增：改偏好→完全退出→重启→值保留（专测持久化锚点修复）**。

**2026-08-27 文档裁决轮（只出文档）**：编辑器打磨与原生对齐唯一终稿 = `editor_polish.md`（原 review_editor.md 改名，三份过程稿 + 第三方 review.md 已删；D1–D13 裁决全记录）。质量尺 9 条见该文 §7；Plan/rbfx/材质三文档同步已执行。

1. **点选全链路实测**（rbfx + Godot）：点 mesh 精确选中、橙框、Outliner/Inspector 联动、gizmo 拖拽框跟随、Ctrl 多选、Select 模式纯净点选（天空盒/灯光/旋转大盒不再抢选，含 "Geometry 100" 茶壶本体命中而旁边空白不误选）。**最优先证伪：拖 gizmo / 改属性 / 删除 → 引擎侧物体真实响应**（修复5 后应 OK；Godot 删除后 Outliner 不得残留——同步 free 修复）。若仍有 Drawable 抢选，按同法加入 rbfx 排除名单。
2. ~~非可视 node 的 icon 拾取~~ → **改判（2026-08-27 D13）**：视口 billboard 图标在 CEE 撞 C1 结构性不可实现（`EditorViewportIconDisplayInterface` 唯一实现者是 Atom Gem）→ 改**线框 gizmo**（`editor_polish.md` P0-6：灯光/相机/空节点按 `m_className` 派发 WireSphere/WireCone 等，零契约改动）。
3. **Tracy 复测** pick-then-orbit：`ApplyPendingMove` 每帧 1 次、Dispatch 总耗时骤降、300-490ms 卡顿帧消失（建议选中/未选中各 30s 对比段）。
4. **冒烟测试**（qt6 review 遗留，从未实机运行）：🔴 B2 视口聚焦时快捷键（Ctrl+Shift+N / Delete / Q-W-E-R-T）——**已源码裁决（2026-08-27 D13 R2）**：`ShortcutOverride` 确认穿不过 window-container 边界（视口是裸 QWindow，只转发 KeyPress）→ 桥接层必做项 = `editor_polish.md` P0-2 快捷键上行桥（~15 行），实机验收兜底；🔴 B3 gizmo 拖拽文字读数可见；🟠 dock 浮动/恢复不黑屏、resize 无闪烁、最小化恢复无 Resize(0,0)、关闭无 validation error、HiDPI 清晰。
   基础工作流（阶段 2/3，代码完成未逐项验收）：New/Open/Save Level（`.prefab`）、Ctrl+P 命令面板、**Workspace Save→打乱布局→Restore 复原**。
   > 变通：无 GPU 的 CI 可先 `CEE_ENABLE_DILIGENT=OFF`（NullBackend）跑逻辑，B2 亦可测。
5. **Gizmo 双风格 100% 对齐肉眼验收**（对照 Blender 5.x / UE）：1/2/3(或 Q/W/E/R/T) 切模式、Blender/Unreal 即时切换、World/Local、Snap 跳格、回写可 Undo；Blender 细线轴+8段锥+菱形平面柄+旋转细环半环+白 view ring+视角淡出+拖拽灰 ghost+area-header 数值；Unreal 3D 圆柱轴+锥头+实体方管臂+旋转厚填充环带朝相机象限+屏幕厚环+中心球/方+hover 变黄+黄 snap 刻度+白字黑底块 HUD；线宽可辨、UE 蓝 Z 不被 remap、Undo/Outliner 拖拽后 gizmo 归位。
6. **P3 打磨**：Gizmo N4-N7、F.7 backlog（见 `Plan.md` §B7）；Qt6 B4/B5/B7/B8（§B8）。
7. **v1.5**：undo 命令化 + 镜像身份稳定化（`rbfx_migration.md` §4，3~5 人日）——同时是 CEE 壳 Undo/Redo 开门的**硬前置**（`editor_polish.md` P0-7 先置灰占位，v1.5 落地后开门）。
8. **v2**：引擎侧增量同步（重同步先清旧镜像）；Godot 三角精拾（同一 `RaycastNode` 接缝）；dirty 渲染；独立渲染线程（与 InputPacket POD 捆绑）；Play-in-Editor；Godot 导入管线进程内；IAssetSource 升 SQLite。

## 已核实关键 API（速查）

- **rbfx**：`EP_EXTERNAL_WINDOW`；`Engine::RunFrame`；`Scene::GetNode(unsigned)` / `Scene::SaveXML`；`Node::FindComponents<Drawable>(SelfDerived)` + `Drawable::GetWorldBoundingBox()`；`Drawable::ProcessRayQuery(RayOctreeQuery(results, Ray, RAY_TRIANGLE))`（逐 drawable，不需 octree；`RayQueryResult::distance_` 世界米，`Ray(origin,dir)` dir 需归一化）；`Object::IsInstanceOf(StringHash)`（含子类，`StringHash("Name")`==类型 id）；`Serializable::GetAttributes/GetAttribute/SetAttribute`；`Renderer::SetViewport`。target `Urho3D`，EASTL=`ea::` 不外泄。
- **Godot**：`libgodot_create_godot_instance`（--wid）；`window_get_native_handle(WINDOW_HANDLE)`；`window_set_vsync_mode(VSYNC_DISABLED=0)`；`Main::iteration`；`ObjectFromId`；`get_property_list/get/set`；`ClassDB::instantiate`+`add_child`；`Node3D::set_global_transform`(Transform3D 12 floats) / `get_global_transform`；`GeometryInstance3D::get_aabb`+`is_class`；`ArrayMesh::add_surface_from_arrays`（官方 gizmo 路径）；`Camera3D::make_current`；`PackedScene::pack`+`ResourceSaver::save`；`GodotApi::AsAabb/AsTransform3D/BuildSurfaceArrays`。坐标 O3DE[x,y,z]→Godot[x,z,−y]。
- **O3DE**：`EditorComponentSelectionRequestsBus`（4 方法，实现 3）；`EditorEntityViewportInteractionRequestBus::FindVisibleEntities`（拾取 cache 唯一来源）；`ViewportEditorModeTrackerInterface::ActivateMode/DeactivateMode`（替换 `EditorDefaultSelection` 必须）；`GetLooseEditorEntities`；`AabbIntersectRay`；`CalculateEditorEntitySelectionBounds`；`FindEntityIdUnderCursor`；入编辑器 `AddRequiredComponents→AddEditorEntity→FinalizeEditorEntity`；`TransformBus::SetParent`（Outliner 建树）；`EditorTransformChangeNotificationBus` / `PropertyEditorEntityChangeNotificationBus`（回写监听）；变长属性范式 `DataElement + SetDynamicEditDataProvider`（ScriptEditorComponent 先例）；gizmo 自绘：`ManipulatorView`（可继承，虚 `Draw` + protected `RefreshBoundInternal`/`ManipulatorViewScaleMultiplier`）、`Linear/Planar/AngularManipulator`（`MakeShared/SetViews/SetAxis`）。

## 关键文件

- 方案：`Plan.md`（rbfx 后端终稿 `rbfx_migration.md`；Godot 后端终稿 `godot_migration.md`；编辑器打磨·原生对齐终稿 `editor_polish.md`）
- 代码：`Framework/EngineNodeComponent.{h,cpp}` / `EngineProperty.{h,cpp}` / `EngineTransformConverter.h`；
  `Backends/{NullBackend,DiligentBackend,RbfxBackend,GodotBackend}.{h,cpp}` / `GodotApi.h`；
  `Viewport/EngineViewport.{h,cpp}` / `EngineViewportWindow.{h,cpp}` / `EditorViewportWidget.{h,cpp}` /
  `GenericDebugDisplay.{h,cpp}` / `EditorViewportCameraController.{h,cpp}` / `EditorGrid.{h,cpp}` /
  `CrossEngineViewportSelection.{h,cpp}` / `GizmoTheme.{h,cpp}` / `GizmoViews.{h,cpp}` / `GizmoManager.{h,cpp}`；
  `Application/CrossEngineEditorApplication.cpp` / `EntityMirrorBridge.{h,cpp}`；
  `Window/EditorMainWindow.cpp` / `CommandPalette.{h,cpp}` / `CeeAssetBrowserPanel.{h,cpp}`；
  `BackendAPI/IEngineBackend.h` / `ISceneRenderer.h` / `IEntityMirror.h` / `IViewportTick.h` / `BackendTypes.h`
- 构建：`Code/CMakeLists.txt`（C++23）；`External/CMakeLists.txt`（后端开关/互斥/依赖遍历器 hack）；`Profiling/CrossEngineProfiler.h`；`External/tracy`（v0.14.0）；`External/Qt-Advanced-Docking-System`（vendored 停靠）
