# editor_polish —— CEE 编辑器打磨·原生对齐终稿（2026-08-27）

> **本文 = 唯一终稿**。吸收三份过程稿（`review_editor_deepseek.md` / `review_editor_opus.md` / `review_editor_kimi.md`）的全部有效结论，完成 2026-08-27 用户**完整再确认**（D1–D12，§7），并吸收**第三方独立复审 `review.md`**（R1–R3 / S1–S4 / M1–M2 / E1–E6 全项落账为 D13）。
> **触发问题（用户）**：CEE 没有原生 O3DE Editor 的通用菜单/工具栏/子编辑入口/其他 UI 面板，整个编辑器看上去非常简陋。原则：保留原版编辑器框架与控件库，移除 Atom 与编辑器无关模块。
> **裁决口径**：`Plan.md` §A6——C1 单底线（Atom 渲染模块零依赖）、C3 三级优先序（① 复用 stock ＞ ② 扩展 stock ＞ ③ 自建，走 ②/③ 须带 `文件:行号` 证据）。
> **执行状态**：本轮**只出文档与决策记录，不动任何代码**；最终方案落定（A1–A9 采纳 + A10 保留 P2）后，`Plan.md` 已同步修订（§A1/§A2/§B1/§B5/§B11/§B12 E7/文末裁决落账），`rbfx_migration.md` 三处小修订已执行（§4 ActionManager 推翻 / §3.3 palette 缺口 / §3.6 展开态恢复降级 P2）。**第三方复审 `review.md` 全部承重论据源码复核通过**（仅两处引用路径勘误），D13 落账 + Plan/rbfx 二次同步已执行。

---

## 0. 结论摘要（TL;DR）

1. **用户的判断成立，且比表述的更严重。** 但根因不是"没复用控件"——控件层复用度其实不低（Outliner / Inspector / AssetBrowser 全栈 / TracePrintFLogPanel / Manipulators / ToolsApplication 全套系统组件）。真正的根因是**命令层（ActionManager）从未通电**，叠加**宿主契约（EditorRequests）大面积空实现**。
2. **O3DE 把"能力"放在 AzToolsFramework（全树 Atom-free），把"接线"放在 EditorLib（绑 6 个 Atom target，`Code/Editor/CMakeLists.txt:111-116`，撞 C1 不可用）。** CEE 复用了能力、没写接线 → 拿到一堆各自失能的官方控件。**「简陋」不是缺控件，是缺接线。**
3. **两条根因（均已核实，必须都修）**：
   - **根因 #1 `EditorRequests` 空实现**：~40 个虚方法只实现 3 个。`IsLevelDocumentOpen` 返回 false → `EntityOutlinerWidget.cpp:582-586` 直接 return → **Outliner 右键菜单从来不弹**；图标方法空 → 实体/组件图标全空。
   - **根因 #2 ActionManager 零注册**：`ActionManagerSystemComponent` 是 `ToolsApplication` 必需系统组件（`ToolsApplication.cpp:210`），**设施在 CEE 进程里活着**，但 `TriggerRegistrationNotifications()` 从未被调用（public static，`ActionManagerSystemComponent.h:39`；原生调用点 `CryEdit.cpp:1676`），四个 manager 零注册。**CEE 的 Edit 菜单里根本没有 Undo/Redo**（撤销栈活着但无入口）。
4. **「简陋」的另一半在视口里（第三方复审纠偏）**：CEE 打开一个含灯光/相机的场景，视口里**除了 mesh 什么都看不见、点不中**——非可视节点无任何表示。且视口 billboard 图标在 CEE **结构性不可实现**（`EditorViewportIconDisplayInterface` 全仓唯一实现者是 Atom Gem，`AtomViewportDisplayIconsSystemComponent.h:31`，撞 C1）。修法 = 线框 gizmo（P0-6，S2），不是补菜单。
5. **快捷键必须专门架桥（R2）**：ActionManager 两级 watcher 只认 `QEvent::ShortcutOverride`，而 CEE 视口是裸 `QWindow`（`EngineViewportWindow.cpp:192-197` 只转发 KeyPress）→ 视口聚焦时（用户 99% 时间）所有注册快捷键不触发。P0-2 新增「快捷键上行桥」（~15 行）为交付物，实机验收兜底。
6. **落地成本**：P0+P1 ≈ 15.7 人日、合计 ≈ 26.5（约 5.3 人周），**CMake 一行不用改**——全部能力都在已链接的 `AzToolsFramework` + `AzQtComponents` 里躺着。另：rbfx v1.5（undo 命令化+身份稳定化，3~5 人日）显式挂路线图，为 Undo 开门的硬前置（R1）。

### 事实修正（相对旧稿与既有认知）

| # | 旧认知 | 事实（已核实） |
|---|---|---|
| F1 | 旧稿 §0.6「CEE **有**日志窗口」 | **面板在，但是空的**：`EditorMainWindow.cpp:356` `new TracePrintFLogPanel()` 后既未 `SetStorageID` 也未 `AddLogTab`；`BaseLogPanel::LoadState()` 无 storageID 直接 `return false`（`LogPanel_Panel.cpp:206-235`）→ **0 个 tab**。**用户原判断「没有日志窗口」效果上成立**，旧稿修正撤回。修复 3 行（官方范式 `AssetProcessor/native/ui/MainWindow.cpp:518,1851-1852`）。 |
| F2 | 旧稿「CEE 有 **6 个**顶层菜单」 | **5 个**：File(`:104`) / Edit(`:117`) / View(`:179`) / Window(`:182`) / Help(`:196`)，其中 View/Help 是空壳。原生是 6 个（File/Edit/Game/Tools/View/Help，`EditorActionsHandler.cpp:1807-1812`，sortkey 100–600）。 |
| F3 | 两份外稿引 `HotKeyManagerInterface::AssignWidgetToActionContext` | **API 真实存在**（`HotKeyManagerInterface.h:34`），另附 dock 属性机制（`ActionManager.cpp:48`），引用无误。 |
| F4 | 「快捷键：HotKeyManager 集中管理 + **SettingsRegistry 持久化**」 | **持久化与重绑定 UI 均不存在**：`HotKeyManagerInterface.h` 仅 3 虚方法（Assign/Remove/SetActionHotKey），无 Load/Save；热键全部硬编码（`EditorActionsHandler.cpp:264,311,414,629…`）。重绑定 = CEE 自建（`SetActionHotKey` 公开，~40 行），排 P2 随 D9 同批，**不是接 ActionManager 白送** |
| F5 | 「AzQtComponents 88 个控件**用了 0 个**」 | **41 个头**（88 是把 `Components/*.h` 一并计入），且绝大多数是 `Style` 友元适配器（非可实例化控件，如 `ComboBox.h:42-59`/`StatusBar.h:23-47`）；CEE 已 `StyleManager::initialize`（`CrossEngineEditorApplication.cpp:85`）→ **样式对全部标准控件已自动生效**，实际复用率远高于 0。可实例化控件仅 Card/BreadCrumbs/SegmentControl/ColorPicker 等少数几个 |
| F6 | 「实体/组件图标全空」是一条问题 | **三条独立路径**（E4）：Inspector 组件头图标（`GetComponentEditorIcon`→`ComponentEditor.cpp`，✅ P0 可修）；palette 图标（无消费者，随 palette 推迟）；**视口 billboard 图标（结构性不可实现，R3）**；Outliner 行图标**本来就有**（`EntityOutlinerListModel.cpp:298/324` 走 `EditorEntityUiHandler` 回落 `:/Entity/entity.svg`，不走 EditorRequests） |
| F7 | 右键菜单是 3 个 | **4 个**：`EditorMenuIdentifiers.h:43`（viewport）+ `:53`（Outliner）+ `:54`（Inspector 组件）+ `:55`（Inspector 属性行，Reset to default 等，反射网格里有真实用途） |

---

## 1. 模块复用裁决（对用户原问的直答）

| 模块 | 判定 | 证据 |
|---|---|---|
| **AzToolsFramework** | ✅ **100% 可复用**，当前复用得远不够 | 全树 Atom-free（`CMakeLists.txt:56-64`，PUBLIC 仅 AzCore/AzFramework/AzQtComponents/Qt）；CEE 已链接并复用面板/Manipulators/Undo 等，但 ActionManager 族零调用 |
| **AzQtComponents** | ✅ **100% 可复用**，41 个控件头（F5）**当前仅基础设施在用** | CEE 已 `StyleManager::initialize` → **样式对全部标准控件自动生效**；41 个头里绝大多数是 `Style` 友元适配器（非可实例化控件），真正可实例化的仅 Card/BreadCrumbs/SegmentControl/ColorPicker 等少数几个（升级排 P2） |
| **EditorCore** | ❌ **不引入**（D1 维持） | `PRIVATE Legacy::CryCommon`（`Code/Editor/CMakeLists.txt:35`）会把 CryCommon 拉进依赖图；`QtViewPaneManager` 布局序列化走 `XmlNodeRef`，与 ADS `saveState` 二源 |
| **EditorLib** | ❌ **不可用**（撞 C1 底线） | 6 个 Atom target（`Code/Editor/CMakeLists.txt:111-116`）+ CryCommon + LmbrCentral。**陷阱**：`Core/EditorActionsHandler.cpp` 在 `Core/` 目录但**编进 EditorLib**（`editor_lib_files.cmake:246-248`），别按目录误判 |
| **Editor** (exe) | ❌ 不适用 | CEE 自己就是 exe |
| Atom / AzNetworking / AzGameFramework | ✅ 确认不需要 | AzNetworking/AzGameFramework 本来就不在任何 Editor target 依赖里，不构成缺口 |

**对用户前提的回答**：`AzToolsFramework` + `AzQtComponents` = 保留并深挖；`EditorLib`/`Editor` = 不是"编辑器框架"，是"绑死 Atom+Cry 的 O3DE 具体实现"；正确路径 = 保留干净框架层 + 按原生 `EditorActionsHandler` 的**范式**重建 CEE 自己的薄 shell（Strategy B 的原判仍成立，只是执行不彻底——复用了控件，没复用 shell 的组织范式）。

---

## 2. 两条根因

### 根因 #1 —— `EditorRequests` 契约大面积空实现

`EditorRequests`（`ToolsApplicationAPI.h:737`，`HandlerPolicy = Single`）是框架面板**反向调用宿主应用**的唯一通道。CEE 挂了总线（`CrossEngineEditorApplication.cpp:134`）却只重写 3 个方法。

| 未实现的方法 | 行号 | 被谁消费 | 实际后果（已核实） |
|---|---|---|---|
| `IsLevelDocumentOpen` → `false` | `:833` | `EntityOutlinerWidget.cpp:582-586` | 🔴 **Outliner 右键菜单永远不弹**（第一行就 `return`） |
| `GetEntityContextId` → null | `:813` | `EntityOutlinerWidget.cpp:605` | 拾取/聚焦模式拿不到实体上下文 |
| `GetDefaultEntityIcon` / `GetComponentEditorIcon` / `GetComponentTypeEditorIcon` → `""` | `:845/:849/:853` | `EntityPropertyEditor.cpp:975`、`ComponentEditor.cpp:704`、`ComponentPaletteUtil.cpp:93`、`EditorEntityIconComponent.cpp:217` | 🔴 **实体/组件图标全空**——最直接的"简陋"观感 |
| `CreateNewEntity` / `CloneSelection` / `DeleteSelectedEntities` → 无效 | `:802/:796/:799` | `EntityOutlinerWidget.cpp:638,650,658,667` | Outliner 新建/复制/删除无效 |
| `GoToSelectedEntitiesInViewports` / `CanGoTo…` | `:887/:890` | `EntityOutlinerWidget.cpp:773,853`、`EntityIdQLabel.cpp:81,85` | 没有"聚焦选中物"（业界通用 F 键/属性面板实体跳转） |
| `OpenPinnedInspector` / `ClosePinnedInspector` | `:876/:878` | `EntityPropertyEditor.cpp:605,5246,5252,5269`（pin 按钮） | Inspector"钉住"按钮无效 |

**范围裁决（A4，按消费方证据精确化）**：P0-2 只实现上表 **12 个有活跃消费方的方法**（`IsLevelDocumentOpen`/`GetEntityContextId`/`CreateNewEntity`/`CloneSelection`/`DeleteSelectedEntities`/`GoToSelectedEntitiesInViewports`/`CanGoToSelectedEntitiesInViewports`/图标×2[组件头+组件类型]/钉住×2），实现体委托给 CEE 现有 Edit 菜单动作的同一后端路径（单一真相源）。**第 13 个方法 `GetComponentIconPath`（E3）**：唯一活跃消费方是 `ComponentPaletteWidget.cpp:1059`（palette 随 P2 推迟），其调用链终点是 Atom-only 视口图标显示 → **与 palette 同批推迟**。以下方法**当前无活跃消费方，推迟到消费者出现**：`RegisterViewPane` 等 7 个（随 P1 注册表实现，EBus 层仅在出现框架侧注册方时补——框架侧现有注册方 PaintBrush/Python 终端 CEE 均不实例化）、`CreateNewEntityAsChild/AtPosition`（消费方 PrefabIntegrationManager 不实例化，D7）、`GetWorldPositionAtViewportCenter`（无消费方）、`ClearRedoStack`（全框架零消费方）、`GetLevelName`（随 P2 窗口标题功能）。

**性质**：不是"缺功能"，是**已复用的官方控件被喂了空契约**。修复属 C3 ①级（实现框架既有接口，零新控件），投入产出比最高。

### 根因 #2 —— ActionManager 已激活但零注册

原生编辑器的菜单栏**完全由 ActionManager 生成**（`MainWindow.cpp` 全文 `new QMenu`/`addMenu(` 计数 = 0）。CEE 手写 QMenuBar + ~20 裸 QAction。

| 项 | 原生 | CEE 现状 |
|---|---|---|
| 注册动作数 | 150+（EditorActionsHandler 45 动作 + 53 菜单绑定 + 13 热键 + 各 Gem） | 0 |
| **Undo / Redo** | Edit 菜单首两项（`o3de.action.edit.undo/redo`） | 🔴 **完全没有**——`ScopedUndoBatch` 已用 **5 处且全在 GizmoManager.cpp**（gizmo 拖拽），撤销栈是活的，但无任何 UI/快捷键；且 **RefreshFromEngine 整批销毁重建镜像实体（`EntityMirrorBridge.cpp:91-114`）→ 撤销历史悬空**（R1）。驱动入口现成：`ToolsApplicationRequestBus::UndoPressed/RedoPressed`（`ToolsApplicationAPI.h:286,291`）。**裁决 R1-A：P0 注册 Undo/Redo 但置灰（诚实禁用），rbfx v1.5（undo 命令化+身份稳定化，3~5 人日）落地后开门**——「宁可禁用，不可半残」 |
| 上下文菜单 | Outliner / Viewport / Inspector 组件 / Inspector 属性行**四个**注册 menu（`EditorMenuIdentifiers.h:43/53/54/55`，F7） | 0 注册 + 视口触发点丢失（见下） |
| 快捷键 | 原生也无持久化（F4）——`HotKeyManagerInterface` 仅 3 虚方法，热键全部硬编码 | 硬编码进 QAction；重绑定需 CEE 自建 UI（P2，`SetActionHotKey` 公开） |
| 动作 enable/disable | 10 个 Action Updater（`EditorActionsHandler.cpp:223-236`） | 无——Delete/Cut/Copy 在零选中时仍可点击 |
| 命令面板数据源 | ActionManager 注册表 | `CollectCommands()` 扫 menuBar（`EditorMainWindow.cpp:788-815`），只见手写 ~20 个 |

**框架自带的注册处理器随广播激活**（`ActionManagerRegistrationNotificationBus` 13 个 hook）：`EditorTransformComponentSelection.cpp:1064`（19 个 transform 动作）、`ComponentModeActionHandler.cpp:29`、`GlobalPaintBrushSettingsSystemComponent.cpp:34`、`HotKeyWidgetRegistrationHelper.cpp:23`、`PrefabIntegrationManager.cpp:147`。其中前两个因 route-B 替换与 D7（不引 prefab）在 CEE 不会实例化（**属预期**）；实际实例化的处理器以打点验收为准。

**route-B 的连带损失（显式记账）**：CEE 用 `CrossEngineViewportSelection` 替换了 `EditorDefaultSelection`（`CrossEngineEditorApplication.cpp:185-193`）→ 唯一调用 `EditorContextMenuUpdate` 的触发点没了（`EditorTransformComponentSelection.cpp:1896`）→ **视口右键无反应**。补法 = 在 `CrossEngineViewportSelection` 里调同一个 `EditorContextMenuUpdate`（1 行 + 成员）。

---

## 3. 缺口 → 落地路线

> 估算 = 人日（1 人日 ≈ 6 有效小时，含实机验收）。**P0+P1 全程零 CMake 改动、零新依赖。**

### P0 ——「让编辑器不再看起来是半成品」（≈ 8.6 人日）

| # | 事项 | 级别 | 估算 | 验收 |
|---|---|---|---|---|
| 1 | **Console 补 tab**（F1）：`SetStorageID(AZ_CRC_CE("CEE::Console"))` + `LoadState()` 失败则 `AddLogTab` 两个 tab（All output / Warnings+Errors，范式 `AssetProcessor/native/ui/MainWindow.cpp:1851-1852`） | ① | 0.3 | 启动即见 tab，后端 `AZ_Warning` 实时进 tab |
| 2 | **快捷键上行桥（R2，新增）**：视口是裸 `QWindow`，不产生 `ShortcutOverride` → ActionManager 两级 watcher 全部失聪。在 `EngineViewportWindow::event()` KeyPress 分支合成 `QEvent::ShortcutOverride` → `sendEvent` 给 `CrossEngineEditorMainWindow` 容器 + `AssignWidgetToActionContext(mainWindow, "o3de.context.editor.mainwindow")`（`HotKeyManagerInterface.h:34`）。~15 行，闭掉 Plan §B8 B2 悬案 | ② | 0.3 | 视口聚焦时 Ctrl+Z / Ctrl+S / Delete 等**全部已注册快捷键生效**（实机验收兜底） |
| 3 | **补齐 `EditorRequests`**（A4 精确集合 12 个有消费方方法 + `GetComponentIconPath` 推迟至 palette 批，§2 表；实现体委托 CEE 现有后端路径，单一真相源） | ① | 1 | Outliner 右键菜单弹出；**Inspector 组件头**图标出现；F 聚焦与属性面板实体跳转可用；pin 按钮可用 |
| 4 | **ActionManager 引导**：新增 `Window/CeeActionsHandler.{h,cpp}`（对标 `EditorActionsHandler` 范式），挂注册通知总线；`StartCommon` 尾调 `TriggerRegistrationNotifications()`（位置对齐原版 `CryEdit.cpp:1669→1676`：主窗口建好、布局恢复后、show 前）。**Trigger 只调一次**；后续组件（材质面板等）经 ActionManager 接口直接注册，无需二次触发（A7） | ① | 1 | 打点确认已实例化的框架注册处理器被触发 |
| 5 | **四个 context menu 注册 + 视口触发点**（F7）：Outliner / Inspector 组件 / Inspector 属性行 / 视口（`EditorContextMenuUpdate` 是公开 `AZTF_API`，`EditorContextMenu.h:32`，直接调用），菜单内容 = CEE 引擎语义动作（Create/Delete/Duplicate/Rename/Focus 等，不含 prefab，D7） | ① | 1.5 | 四处右键各 ≥5 可用条目 |
| 6 | **非可视节点线框 gizmo（S2，新增）**：灯光/相机/空节点在视口里看得见、点得中。`EngineNodeComponent` 挂 `EntityDebugDisplayEventBus` + 按 `m_className` 派发绘制（零契约改动，调用点现成 `EditorHelpers.cpp:312/316`，图元现成 `GenericDebugDisplay` 的 WireSphere/WireCone/WireBox/Arrow）——视口 billboard 图标撞 C1 结构性不可实现（R3），线框是唯一正解（Blender 范式） | ② | 1.5 | 打开含灯/相机场景：三者在视口有可辨线框表示、可被点选 |
| 7 | **Undo / Redo 占位置灰（R1-A）**：Edit 菜单首两项 + Ctrl+Z / Ctrl+Shift+Z 注册并驱动 `ToolsApplicationRequests::UndoPressed/RedoPressed`（`ToolsApplicationAPI.h:286,291`），但 `InstallEnabledStateCallback` → false + tooltip 说明「引擎后端 undo 未命令化」。**rbfx v1.5（undo 命令化+身份稳定化，3~5 人日）落地后开门** | ① | 0.2 | 菜单项可见、诚实置灰、tooltip 说明原因；v1.5 后转可用 |
| 8 | **保存安全（M2，新增）**：`RbfxBackend::SaveScene` 直写 XML → 崩溃/断电即毁场景。改 `.bak` + 原子替换（QSaveFile 或 write-tmp-then-rename，复用范式 `Qt::QSaveFile`） | ② | 0.3 | 保存后 `scene.xml.bak` 存在；保存中杀进程场景文件不损坏 |
| 9 | **View 菜单 = 面板开关**：数据源 = ADS `CDockManager::dockWidgetsMap()`（`DockManager.h:505`）**永久直取**（A1——面板无论来自硬编码还是注册表都进 ADS，ADS 即唯一真相源），checkable 双向同步 | ② | 0.5 | 每个 dock 一条 checkable，勾选与 ADS 显隐双向同步 |
| 10 | **布局自动保存/恢复**：复用现有 Workspaces 基建，ctor 尾 restore + closeEvent 自动 save；**自动存档写入独立"会话槽"，不覆盖用户命名 Workspaces**（A5） | ② | 0.5 | 打乱布局 → 重启 → 复原 |
| 11 | 回归：`check_no_atom.ps1` + NullBackend 单后端冒烟 + **人工点检菜单/快捷键清单**（§7 尺子 1–9 逐条走查；不建自动化测试 target，KISS） | — | 1.5 | 三层断言 PASS，尺子逐条人工走查记录在案 |

### P1 ——「补齐原版观感与工具入口」（≈ 7.1 人日）

| # | 事项 | 级别 | 估算 |
|---|---|---|---|
| 12 | 手写 QAction → ActionManager 全量迁移（语义不变，New/Open/Save 仍走 CEE 的 `OnNewLevel` 等；热键走 HotKeyManager；Delete 等装 enabled 回调 + Updater）。**ID 命名（S4）：菜单结构 ID 复用 `o3de.menu.*`（框架硬编码 `EditorContextMenu.cpp:52`/`EntityOutlinerWidget.cpp:598`），动作 ID 全部 `cee.action.*`**（CEE 零 Gem，无 `o3de.action.*` 冲突风险；框架级上下文无硬编码时换）；**数据驱动菜单例外条款（S3）**：引擎类型表驱动的 Create 菜单**不预注册空槽**（原生 Recent Files 的 10 槽是妥协不是范式，`EditorActionsHandler.cpp:58,319`），在 `Create` 域手动建子菜单，落代码注释说明例外原因 | ① | 2 |
| 13 | **后端动作接缝（M1，新增）**：`IRbfxBackend::GetActionRegistrationPatterns` 以静态动作表形式返回（`{ID, 文本, 热键, 处理器}`），rbfx/Godot 各挂自己的 `ActionManagerRegistrationNotificationBus` handler，从 CEE 动作注册循环**解耦**——新引擎接入不再动 CEE 壳代码。契约零增长（纯虚已存在，加一个静态方法即可） | ①协议+② | 0.3 |
| 14 | **ViewPane 薄注册表 + Tools 菜单**（D6）：`{名字, 类别, 工厂, dock}`（~150 行），5 个现有面板迁入（消灭 `BuildDockPanels` 硬编码），Tools 菜单列出子编辑器。**注册表的真实正当性 = ADS `restoreState` 按 dock 名字查找（`DockManager.cpp:427`）→ 布局恢复与延迟创建都需要稳定名字**。View 菜单 = 注册表（目录）∪ ADS（实例状态），数据源维持 ADS 直取不切换（A1） | ①协议+③薄壳 | 2 |
| 15 | 命令面板数据源改 `ActionManagerInterface` 枚举（覆盖面 20 → 150+） | ② | 0.5 |
| 16 | 工具栏样式 + 图标：`AzQtComponents::ToolBar::addMainToolBarStyle()`（`ToolBar.h:61`）一行 + icon 资源 | ① | 1 |
| 17 | **状态栏（D8 修订）**：**两格**——项目路径 / 后端名+版本，`AzQtComponents::StatusBar` 样式，~60 行。**砍 FPS/内存两格**（引擎无现成统计接口；硬上 = 引擎轮询注入，违 KISS）。不复刻源码管理/AssetProcessor 两格 | ③ | 0.3 |
| 18 | **视口 header bar（S1，新增，取代 ViewportUi，D5 关闭）**：`ViewportUiDisplay` 是透明 QWidget 覆盖层（`ViewportUiDisplay.h:59,64`），与 §B8 B3 native 表面约束冲突且与引擎输入竞抢。改 **QToolBar 兄弟件**（Blender/Godot/UE 范式）：transform/space/snap 按钮 + gizmo 开关，零覆盖层、零 §B8 B3 风险 | ② | 0.5 |
| 19 | 实机验收（菜单/工具栏/状态栏/header bar 全项走查 + Plan §B10 B2 快捷键项——由上行桥统一派发，实机验证兜底） | — | 0.5 |

### P2 ——「完整性」（≈ 10.0 人日）

框选 `EditorBoxSelect`（D3 已复议通过，0.5）· **Preferences 对话框**（D9：SettingsRegistry 持久化先行，再挂 QTreeWidget + `ReflectedPropertyEditor` 对话框，数据源换 SettingsRegistry，2）· **快捷键重绑定页**（E1：`SetActionHotKey` 公开，~40 行，挂进 Preferences 同批，+0.5）· Recent Files（QSettings，1）· **窗口标题+脏标记+`SaveChangesDialog` 同批**（A6：脏标记先行，避免"永远询问"换皮，1.5+0.5）· **自动保存/崩溃恢复**（M2 后续：与脏标记同批，1）· Toast 宿主（`ToastNotificationsView`，0.5）· ViewportSettings 接入（helpers/icons 显隐、manipulator 缩放，0.5）· `QTreeViewStateSaver` 补 Outliner/AssetBrowser 展开态（顺带清掉 `rbfx_migration.md` §3.6 v2 待办，0.5）· AzQtComponents 控件升级（Card/SegmentControl，1）· Help 菜单外链（0.5）。

> **rbfx v1.5（undo 命令化 + 镜像实体身份稳定化）3~5 人日**：不属 CEE 壳批，挂 `rbfx_migration.md` §4 路线图；是 P0-7 置灰 Undo 开门的**硬前置**（R1）。
> 框选设计与 stock 语义的差异（2026-08-27 调研定稿）：复用 `EditorBoxSelect` 状态机（点击 vs 拖拽判定/修饰键跟踪/4 个回调点），**判定与语义吸收业界共识**——屏幕投影包围盒重叠测试（UE/Blender/Unity/Godot 一致，替代 stock 的"位置点命中"漏选）、普通 = 替换选择 / Ctrl = 并集 / Ctrl+Shift = 差集、松手才结算（单次 undo 命令）；marquee 用 QPainter 走现有 Qt overlay 路径绘制（`GenericDebugDisplay` 无 2D 图元）。lasso/遮挡剔除/视锥筛选不做（§4）。

> A2/A3 已删两项死代码/重复项：ComponentMode switcher（CEE 零 ComponentMode 实现，grep 0 命中）；Pinned Inspector（P0-2 实现 EditorRequests 钉住两方法即落地，pin 按钮 `EntityPropertyEditor.cpp:605`）。

### P3 ——「打磨」（≈ 0.8 人日）

ProgressShield（0.3）· 主题化对话框（`FileDialog`/`MessageBox`/`InputDialog`，0.5）· ~~`StyledTracePrintFLogPanel` 可选换用~~（**删除**：Console 已用官方 `TracePrintFLogPanel` 范式，换皮无收益）。

### 工作量合计

| 批次 | 人日 |
|---|---|
| P0 | ≈ 8.6 |
| P1 | ≈ 7.1 |
| P2 | ≈ 10.0 |
| P3 | ≈ 0.8 |
| **合计** | **≈ 26.5（约 5.3 人周）** |

> P0 完成后的一次实机启动即可见：右键四处有菜单、Inspector 组件头图标出现、视口里灯/相机可见可点、Undo/Redo 诚实置灰、Console 有日志、View 菜单可开关面板、布局自动恢复、保存有 `.bak` 兜底。**P0 仅新增线框 gizmo 一处自建绘制（② 级，§2 S2），其余纯填框架契约。**

---

## 4. 防过度设计清单（明确不做）

- ❌ 不引 `EditorLib` / `EditorCore` / `Editor`（§1 裁定）。
- ❌ **不引 `PrefabIntegrationManager`**（D7，2026-08-27 再确认）——CEE 真相源是引擎原生场景，prefab 保存/实例化动作大多无意义甚至污染引擎场景。
- ❌ 不复刻 `QtViewPaneManager`（1500 行 + Cry Resource.h ID 段 + `XmlNodeRef` 序列化）——薄注册表只做 `名字→工厂→dock`，布局序列化仍走 ADS `saveState`（单一真相源）。
- ❌ 不复刻 `ConsoleSCB`（绑 Cry `IConsole`/`ICVar`）——`TracePrintFLogPanel` 已覆盖日志需求。
- ❌ 状态栏不复制源码管理/AssetProcessor 两格（CEE 无这两个子系统）。
- ❌ 不做 Python 面板（D10）——`EditorPythonBindings` Gem 虽无 Atom，但 CEE 无任何消费者，新增 Gem 依赖违 KISS；出现真实自动化需求再评估。
- ❌ 不做 Track View / UI Editor / Script Canvas / EMotionFX / PhysX Config 等 Gem 面板（全绑 O3DE 运行时）。
- ❌ 不做 Layouts 多布局管理（原版 View▸Layouts）——CEE Workspaces 已是超集。
- ❌ 不做多视口布局（Top/Front/Side/Persp 四视图，A8）——依赖引擎侧多相机渲染，v2 视需要。
- ❌ 不做 ComponentMode switcher（A2）——CEE 零 ComponentMode 实现，空 UI 无消费者。
- ❌ 不建 Game 菜单空壳（D2）——等 `ISimulation`（Plan §B12 E3 草案）落地再按原生 sortkey 300 插入。
- ❌ 不为 ActionManager 迁移引入任何新 CMake 依赖（验收尺子 9）。
- ❌ **不做视口 billboard 图标（R3）**——`EditorViewportIconDisplayInterface` 全仓唯一实现者是 Atom Gem，撞 C1 结构性不可实现；由线框 gizmo（P0-6）承担可视性。
- ❌ **不建自动化测试 target（KISS）**——P0-11 回归改为人工点检清单（§7 尺子 1–9 逐条走查记录）；测试价值不够覆盖新 target 的 CMake/CI 维护成本。
- ❌ 框选不做 lasso / 遮挡剔除 / 视锥筛选——与业界主流收敛点（bounds-overlap + 修饰键 + 松手结算）无关的增强全部不做。
- ❌ 状态栏不做 FPS/内存格（D8 修订）——引擎无现成统计接口，硬上 = 轮询注入。
- ❌ 数据驱动菜单**不预注册空槽**（S3）——原生 Recent Files 的 10 槽是妥协不是范式；Create 域菜单手动建子菜单并落代码注释说明例外。

---

## 5. 与既有文档的冲突点（✅ 2026-08-27 已同步修订，清单留档）

| 文档 | 原文 | 冲突（已核实） | 待修订 |
|---|---|---|---|
| `Plan.md` §A2 | 「命令/撤销 → ActionManagerInterface/UndoSystem」标"100% 复用，零重写" | ActionManager 复用度 = 0；撤销栈复用但无 UI 入口 | 改"⏳ 待接入，见本文 §3 P0" |
| `Plan.md` §A1 | 「只补两样框架层缺失 UX：Workspaces 与命令面板」 | 低估：EditorRequests 实现、ActionManager 注册、ViewPane 注册表同缺（宿主侧欠账） | 重述边界 |
| `Plan.md` §B1 | 编辑器壳「工具栏…全部复用 O3DE 框架」 | 工具栏实为手写裸 QToolBar + 文字按钮 | 改"待接入 ActionManager + addMainToolBarStyle" |
| `Plan.md` §B5 | 命令面板「模糊搜索全部已注册动作」 | 实为遍历 `menuBar()` ~20 项 | P1-11 后修正措辞 |
| `Plan.md` §B11 | 「不做框选」 | `EditorBoxSelect` 是独立、回调驱动、Atom-free 的 ① 级 stock（经 `DebugDisplayRequests` 绘制，CEE `GenericDebugDisplay` 已实现），D3 复议通过 | 改写为「框选 = 复用 EditorBoxSelect（①级）」；记为 C3 反面判例**第三例** |
| `Plan.md` 文末 | `check_no_atom.ps1` 已精化为"禁 Atom 渲染模块 + 白名单 AtomToolsFramework.Core" | 脚本未改（仍 `-cmatch 'Atom'` 裸词，95 行内零命中 AtomToolsFramework/RPI/RHI） | ✅ **改表述已执行**（Plan 文末）；精化脚本归材质实施轮（`material_migration_final.md` §3.6 spec 已有，触发点 = Code/Source 真实出现 AtomToolsFramework include） |
| `rbfx_migration.md` §4 | 「ActionManager 全量接入暂不做」 | 当时论证只把 ActionManager 当"快捷键方案"，漏掉它同时是三个 context menu 的唯一数据源 | 推翻：P0/P1 全量接入 |
| `rbfx_migration.md` §3.3 | 「palette 头部可见但惰性」 | 真正缺的是 `ComponentPaletteWidget` 未接 | P2 |
| `rbfx_migration.md` §3.6 | v2 待办「Refresh 展开态恢复」 | `QTreeViewStateSaver` 现成 | 降级为 P2 |
| `Plan.md` §B10 验收项 3 | 「非可视节点靠 editor icon 点选（镜像实体需带 `EditorEntityIconComponent`）」 | 视口 billboard 图标撞 C1 结构性不可实现（R3） | 改判为线框 gizmo（P0-6），§B10-3 重写 |
| `Plan.md` §B8 B2 | ShortcutOverride 风险「由框架自带机制解决」 | 两级 watcher 只认 QWidget 的 ShortcutOverride；视口是裸 QWindow → 失效（R2） | 处置改为 P0-2 快捷键上行桥（~15 行），实机验收兜底 |
| `Plan.md` §B11 | 框选「经 `DebugDisplayRequests` 绘制」 | `GenericDebugDisplay` 未实现任何 2D 图元（无 DrawWireQuad2d） | marquee 改走 QPainter 现有 Qt overlay 路径（P2 框选注） |

---

## 6. 决策记录（2026-08-27 用户完整再确认）

| # | 议题 | 裁决 |
|---|---|---|
| **D1** | ViewPane 落地方式 | ✅ **自建 ADS 薄壳，不引入 EditorCore**（维持）。理由：(a) CryCommon 进依赖图与 Plan §A4/§B11 冲突；(b) 原生 `XmlNodeRef` 序列化与 ADS `saveState` 二源。协议 ① 级复用（`EditorRequests` 7 个 ViewPane 虚方法），实现 ③ 级薄壳 ~150 行 |
| **D2** | 菜单结构 | ✅ **顶层 = File / Edit / View / Tools / Help 五个**（维持）；`Game` 缓到 `ISimulation` 落地（sortkey 300 已预留）；原 Window 菜单内容（Workspaces/命令面板）并入 View 菜单。**修正旧稿"6 个"** |
| **D2b** | Action ID 命名 | ✅ **混合命名（D13 修订 S4）**：菜单结构 ID 复用 `o3de.menu.editor.*`（框架硬编码 `EditorContextMenu.cpp:52`/`EntityOutlinerWidget.cpp:598`，Gem 动作自动落位）；**动作 ID 全部 `cee.action.*`**（CEE 零 Gem 装载，无 `o3de.action.*` 冲突风险；语义等价动作经后端接缝 M1 由各引擎自注册为 `cee.action.<engine>.*`）；CEE 特有（gizmo 风格/Workspace/命令面板）用 `cee.*` |
| **D3** | §B11「不做框选」复议 | ✅ **复议通过（维持）**：`EditorBoxSelect` ① 级白送，P2 上；C3 反面判例第三例。**设计定稿（2026-08-27）**：状态机全复用；判定与语义吸收业界共识（bounds-overlap 替换 stock 位置点测试、替换选择/Ctrl 并集/Ctrl+Shift 差集、松手结算单 undo 命令）；marquee 走 QPainter Qt overlay（`GenericDebugDisplay` 无 2D 图元）；lasso/遮挡/视锥不做 |
| **D4** | P0 范围 | ✅ **扩大为本文 §3 P0 十一项**（≈8.6 人日，全 ①/② 级）：在原八项基础上吸收第三方复审 R2（快捷键上行桥）、S2（非可视线框 gizmo）、M2（保存安全），Undo/Redo 由 R1-A 改为占位置灰（0.2），回归改人工点检（1.5）；撤销原 P0-4（Prefab，见 D7） |
| **D5** | ViewportUi 时机 | ✅ **关闭（D13，S1）**：`ViewportUiDisplay` 是透明 QWidget 覆盖层，与 §B8 B3 native 表面约束冲突。**改判为视口 header bar（QToolBar 兄弟件，P1-18）**——transform/space/snap 进 toolbar 而非覆盖层，零 §B8 B3 风险、零引擎输入竞抢，Blender/Godot/UE 均为此范式 |
| **D6** | ViewPane 注册表 + Tools 菜单时点 | ✅ **按业界最佳实践裁决：P1 建**（采纳 opus A5，推翻 kimi「不建」与本人旧稿「缓」）。依据：面板注册表是成熟编辑器标准构件（Unity EditorWindow 注册 / UE Slate tab / Blender editor type / O3DE QtViewPaneManager），消费者 = Tools 菜单 + 程序化注册 + 稳定 ID + 材质 dock 面板接入点（已在路线图）。护栏：只做 `名字→工厂→dock`，序列化走 ADS。**A1 修订：View 菜单数据源 = ADS `dockWidgetsMap()` 永久直取，注册表不与 ADS 双源** |
| **D7** | PrefabIntegrationManager | ✅ **不引入**（2026-08-27 再确认）：CEE 真相源 = 引擎原生场景；右键菜单由 CEE 自注册引擎语义动作 |
| **D8** | 状态栏 | ✅ **P1 建两格版（D13 修订）**（项目路径 / 后端名+版本，~60 行，`AzQtComponents::StatusBar` 样式）；**砍 FPS/内存两格**——引擎无现成统计接口，硬上 = 轮询注入，违 KISS；不复制源码管理/AP 两格 |
| **D9** | Preferences | ✅ **P2 建对话框**（按业界最佳实践：Unity/UE/Blender/Godot/O3DE 均有图形化设置界面，工业级交付无对话框=换一处"简陋"）。落地顺序：**SettingsRegistry 持久化先行**（承接 Plan §B7 F.7），再挂 QTreeWidget + `ReflectedPropertyEditor` 对话框（骨架可抄，数据源换 SettingsRegistry，② 级）；不造自建设置框架 |
| **D10** | Python 面板 | ✅ **不接**（默认裁决）：无消费者、新增 Gem 依赖违 KISS；有自动化需求再评估 |
| **D11** | Plan.md/rbfx 修订时机 | ✅ **已执行**：深度复审（A1–A9 采纳 + A10 保留 P2）落定后，本轮同步修订 `Plan.md`（§5 冲突表 6 处 + §B12 E7 + 文末裁决落账）；rbfx 三处小修订已执行（§4 ActionManager 推翻、§3.3 palette 缺口、§3.6 展开态恢复降级 P2） |
| **D12** | 深度复审修正 | ✅ **A1–A9 全采纳（2026-08-27 用户拍板），A10 保留 P2**：A1 View 菜单永久直取 ADS（删返工）/ A2 删 ComponentMode switcher 死 UI / A3 Pinned Inspector 并入 P0 / A4 EditorRequests 精确化 12 方法（死代码推迟）/ A5 布局自动存档走会话槽 / A6 脏标记+SaveChangesDialog 同批 / A7 Trigger 一次性语义 / A8 多视口列入不做 / A9 免测措辞软化；A10 Card/SegmentControl 留 P2 |
| **D13** | 第三方独立复审 `review.md` 吸收（2026-08-27） | ✅ **全部承重论据源码复核通过（100%，仅两处引用路径勘误），用户拍板全项落账**：**R1** Undo 栈只盖 gizmo + RefreshFromEngine 重建镜像作废历史 → 采纳 R1-A：P0-7 Undo/Redo **占位置灰**（诚实禁用 + tooltip），rbfx v1.5（undo 命令化+身份稳定化，3~5 人日）挂 `rbfx_migration.md` §4 为开门硬前置；**R2** 快捷键上行桥 → P0-2（~15 行），闭 Plan §B8 B2；**R3** 视口 billboard 图标撞 C1 结构性不可实现 → 列入不做清单，由 S2 线框 gizmo 承担；**S1** header bar 取代 ViewportUi → P1-18，**D5 关闭**；**S2** 非可视节点线框 gizmo → P0-6（1.5 人日，零契约改动）；**S3** 数据驱动菜单例外条款 → P1-12 动作表 + §4 禁止空槽绕法；**S4** 动作 ID 全 `cee.action.*` → D2b 修订；**M1** 后端动作接缝 → P1-13（0.3，契约零增长）；**M2** 保存安全（.bak+原子替换）→ P0-8，自动保存/崩溃恢复随脏标记排 P2；**E1** HotKeyManager 无持久化 → F4 修正 + 重绑定 P2 自建（+0.5）；**E2** AzQtComponents 41 头非 88、Style 已生效 → F5 修正 §1；**E3** 图标三路径 → F6 修正，`GetComponentIconPath` 与 palette 同批推迟；**E4** 量化叙事（150+ vs 0）→ TL;DR 重写（§0-4/5）；**E5** 右键四处非三处 → F7 修正，P0-5 改四处；**E6** 三份过程稿已删；**7.1** 计划后置项前置为 P0（上行桥/gizmo/保存安全）；**7.2** ViewPane 注册表正当性 = ADS restoreState 按名字查找（`DockManager.cpp:427`）→ P1-14 理由替换；**7.3** 尺子重写为质量尺（§7）**7.4** 不建自动化测试 target，人工点检入验收（§4） |

---

## 7. 验收尺子（2026-08-27 重写为质量尺，砍虚荣数字）

> **验收方式**：P0-11 回归 = 人工点检清单，逐条走查记录在案；**不建自动化测试 target**（KISS，测试价值 < 新 target 的 CMake/CI 维护成本）。

1. **零死条目**：菜单/工具栏/右键四处（含 Inspector 属性行）的每一个条目点击后都有可观察效果——没有"点了没反应"的僵尸项。
2. **零悬空绑定**：ActionManager 注册动作的 enable 状态与实际可执行性一致——零选中时 Delete/Cut/Copy 置灰，选中后恢复。
3. **enable 语义诚实**：Undo/Redo 在 v1.5 前**置灰**且 tooltip 说明原因；v1.5 落地后转可用——不存在"亮着但点了没反应"。
4. **视口聚焦快捷键**：焦点在视口（用户 99% 时间）时 Ctrl+Z/S/C/V/Delete/F 等全部已注册快捷键生效（R2 上行桥）。
5. **右键四处全部可执行**：Outliner / Inspector 组件 / Inspector 属性行 / 视口，各 ≥5 个可用条目（D7 护栏：不含 prefab 动作）。
6. **Console + 布局 + View 三连**：启动 10 秒内 All output 出现后端日志；打乱布局 → 重启 → 复原；View 菜单每个 dock 一条 checkable 双向同步。
7. **视口不空（S2）**：打开含灯/相机/空节点的场景，三者在视口有可辨线框表示、可被点选——不依赖 Inspector/Outliner 盲选。
8. **Undo 诚实性**：v1.5 落地后 `rbfx_migration.md` §4「10 次 Ctrl+Z 回到初始状态」尺子可执行。
9. **护栏不退化**：`check_no_atom.ps1` 三层 PASS；**零新增 CMake 依赖**（P0+P1 全部落在已链接的 AzToolsFramework + AzQtComponents 内）。

---

## 附录 A — 核心证据索引

| 论点 | 证据 |
|---|---|
| EditorLib 绑 6 个 Atom target | `Code/Editor/CMakeLists.txt:111-116` |
| AzToolsFramework 全树 Atom-free | `Code/Framework/AzToolsFramework/CMakeLists.txt:56-64` |
| 原生菜单全走 ActionManager | `Code/Editor/MainWindow.cpp` `new QMenu`/`addMenu(` 计数 0；建栏方 `EditorMenuBar.cpp:54,62` |
| 6 顶层菜单与 sortkey | `EditorActionsHandler.cpp:1807-1812` |
| ActionManager 已在 CEE 进程中 | `ToolsApplication.cpp:210`；触发入口 `ActionManagerSystemComponent.h:39`；原生调用点 `CryEdit.cpp:1676` |
| 5 个框架注册处理器 | `PrefabIntegrationManager.cpp:147`、`EditorTransformComponentSelection.cpp:1064`、`ComponentModeActionHandler.cpp:29`、`GlobalPaintBrushSettingsSystemComponent.cpp:34`、`HotKeyWidgetRegistrationHelper.cpp:23` |
| Outliner 右键被 `IsLevelDocumentOpen` 掐断 | `EntityOutlinerWidget.cpp:582-586` |
| Undo 驱动入口 | `ToolsApplicationAPI.h:286,291`（`UndoPressed/RedoPressed`） |
| ShortcutOverride 机制 | `ActionManager.cpp:92-135`（watcher）；`HotKeyManagerInterface.h:34`（`AssignWidgetToActionContext`）；ADS 非 StyledDockWidget → 靠 Qt 冒泡（A9） |
| Console 0-tab 机理 | `LogPanel_Panel.cpp:206-235`；官方范式 `AssetProcessor/native/ui/MainWindow.cpp:518,1851-1852` |
| 视口右键唯一触发点 | `EditorTransformComponentSelection.cpp:1896`；补触发点 API 公开：`EditorContextMenu.h:32`（`AZTF_API EditorContextMenuUpdate`） |
| ADS 面板列表 API（A1） | `External/Qt-Advanced-Docking-System/src/DockManager.h:505`（`dockWidgetsMap()`） |
| Pinned Inspector 消费链（A3） | `EntityPropertyEditor.cpp:605`（pin 按钮）→ `:5246,5252,5269`；基类默认空 `ToolsApplicationAPI.h:876` |
| GoTo 消费方（A4） | `EntityOutlinerWidget.cpp:773,853`；`EntityIdQLabel.cpp:81,85` |
| ComponentMode 零实现（A2） | CEE `Code/Source` grep `ComponentMode` = 0 命中 |
| `GetEntityContextId` 消费方（A4） | `EntityOutlinerWidget.cpp:605`；默认空 `ToolsApplicationAPI.h:813` |
| EditorActionsHandler 编进 EditorLib | `editor_lib_files.cmake:246-248` |
| ViewBookmark 系统组件已激活无 UI | `ToolsApplication.cpp:220` |
| 工具栏样式一行 | `AzQtComponents/Components/Widgets/ToolBar.h:61` |
| `EditorBoxSelect` 独立可复用 | `EditorBoxSelect.h:31-60`；`EditorBoxSelect.cpp:11-15` |
| `IEditor` 135 纯虚硬阻塞 | `Code/Editor/IEditor.h:342-556` |
| 护栏脚本未精化 | `Scripts/check_no_atom.ps1`（95 行裸词 `-cmatch 'Atom'`） |
| CEE 菜单手写、View/Help 空壳 | `EditorMainWindow.cpp:100-197`（`:179`、`:196`）；Console 未初始化 `:356`；命令面板扫菜单 `:788-815`；Workspace 手动存档 `:822-865` |
| R1：ScopedUndoBatch 仅 5 处、全在 GizmoManager | `Gems/CrossEngineEditor/Code/Source/**/GizmoManager.cpp`（grep 5 命中）；镜像重建作废历史 `EntityMirrorBridge.cpp:91-114` |
| R2：watcher 只认 QWidget ShortcutOverride | `ActionManager.cpp:31/96`；视口裸 QWindow 只转发 KeyPress `EngineViewportWindow.cpp:192-197` |
| R3：视口图标唯一实现者是 Atom | `AtomViewportDisplayIconsSystemComponent.h:31`；`EditorHelpers.cpp:324-328` null → return |
| S1：ViewportUiDisplay 是透明覆盖层 | `ViewportUiDisplay.h:59,64`（ctor `(QWidget* parent, QWidget* renderOverlay)`） |
| S2：线框 gizmo 调用点与图元均现成 | `EditorHelpers.cpp:312/316`；`GenericDebugDisplay.h:132-157`（WireSphere/WireCone/WireBox/Arrow）；`EngineNodeComponent` 已挂 4 条总线、有 `m_className` |
| S3：Recent Files 预注册 10 槽是妥协 | `EditorActionsHandler.cpp:58,319` |
| S4：框架硬编码 o3de.menu.* 菜单 ID | `EditorContextMenu.cpp:52`；`EntityOutlinerWidget.cpp:598` |
| M1：后端接缝契约零增长 | `IRbfxBackend` 纯虚已存在，加静态动作表方法即可 |
| M2：保存直写无兜底 | `RbfxBackend.cpp:1001` `SaveXML(file)` 直接覆盖 |
| E1：HotKeyManager 无持久化 | `HotKeyManagerInterface.h` 仅 3 虚方法（Assign/Remove/SetActionHotKey）；热键硬编码 `EditorActionsHandler.cpp:264,311,414,629…` |
| E3：GetComponentIconPath 唯一消费方随 palette | `ComponentPaletteWidget.cpp:1059` |
| E5：四个 context menu ID | `EditorMenuIdentifiers.h:43/53/54/55` |
| 7.2：ADS restoreState 按名字找 dock | `External/Qt-Advanced-Docking-System/src/DockManager.cpp:427`（`findDockWidget`） |

---

*终稿 2026-08-27。三份过程稿（deepseek/opus/kimi）与第三方独立复审 `review.md`（R1–R3/S1–S4/M1–M2/E1–E6）均已被本文吸收并**删除**，D13 为唯一吸收记录。本文为唯一裁决依据。本轮无任何代码改动。*
*待办：① ✅ `Progress.md` 补记已执行（2026-08-27）；② 开 P0 实施轮。*
