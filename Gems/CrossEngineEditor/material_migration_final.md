# 跨引擎材质编辑器 —— 终稿设计方案（四方裁决合并稿）

> 状态：2026-08-25 定稿。本文 = CEE 材质编辑器跨引擎化的**唯一当前方案**。
>
> 成稿过程（按用户指定的两阶段流程）：
> **阶段一** —— 在**未阅读**其余三份方案的前提下，基于对 O3DE·Atom / rbfx / Filament / Godot / Blender 五套材质系统的
> 一手源码调研独立完成初稿。
> **阶段二** —— 阅读 `material_migration.md`（deepseek，下称 A 稿）、`material_migration_opus.md`（opus，B 稿）、
> `material_migration_kimi.md`（kimi，C 稿），逐条吸收/驳回并升级为本文。**四方裁决记录 = §11**。
> 三稿 + opus review 已按用户指示彻底删除（工作区与 git 历史均不留档）；
> §11 为自含裁决记录，可独立追溯。
>
> 权威关系：愿景硬规则与顶层边界 = `Plan.md` §A0 / §A6（C1–C7）/ §B12；总路线 = `rbfx_migration.md` §4；
> Godot 后端 = `godot_migration.md`。本文任何内容与 `Plan.md` 冲突，以 `Plan.md` 为准。
> 铁律：不臆想 API（Progress.md:51）——本文所有承重声称均带 `文件:行号`，且**阶段二吸收的每一条承重事实都经本人独立 grep 复核**。
> §11.4 = 复核结果：3 处三稿偏差 + 1 处四方全漏的缺口（S5）+ **3 处本文初稿自身的错误**（S6 / S7 / R1 幻影 API，根因同一：拿规则当省掉验证的借口）。
> §11.6 = 2026-08-25 规则裁决（C1 检验手段精化 / Atom 后端不开口子 / C3 澄清后组头 toggle **改判采纳 S7** / Plan.md 三处澄清）。

---

## §0 一页纸结论（TL;DR）

**用户的判断成立，而且比"PBR 都差不多"这个直觉更硬。** 论据是三条源码级事实：

1. **O3DE 的材质编辑器框架本来就跟 Atom 无关。**
   `DynamicPropertyConfig` / `DynamicPropertyGroup`（`DynamicProperty.h:21-48`、`DynamicPropertyGroup.h:18-32`）只依赖 AzCore。
   `Include/AtomToolsFramework/{Document,DynamicProperty,Inspector,Window}` 四个目录**头文件 Atom 引用数 = 0**
   （实测 Document 14/14、DynamicProperty 2/2、Inspector 6/6、Window 3/3 全净）。Atom 只在 **4 处实现细节**里漏进来（§3.2）。

2. **"材质 = 一堆带元数据的属性"是五个引擎的共识，而且四个引擎自己就能吐出这份 schema。**
   Atom 从 `.materialtype` 数据驱动；Godot 从 `_get_property_list()` + `_validate_property()` 反射；
   Filament 从 `.filamat` 反射出参数名/类型/精度；rbfx 没有反射，但**它自己的编辑器已经把表手写好了**
   （`MaterialInspectorWidget.cpp:85-193`）。所以"后端提供 schema"不是我们发明的抽象，是四个引擎的既有能力。

3. **PBR 已经收敛到可实证的程度。** 最硬的一条：介电质反射率——最容易分歧的参数——**五家默认值全部 0.5**
   （Atom `specularF0.factor` / rbfx `DielectricReflectance` `Material.cpp:1007` / Godot `set_specular(0.5)` /
   Filament `reflectance` / Blender `Specular IOR Level`）。
   而"值 + 贴图"两字段的表达方式是 4/5 的事实标准；唯一的例外 Blender 用节点图，但它导出 glTF 时
   **又必须把节点图折叠回 factor+texture**（`search_node_tree.py` 用 1169 行做图模式识别且经常失败）。
   这反过来证明：属性网格式材质编辑器的正确模型就是 factor+texture，节点图是另一个工具（对应 MaterialCanvas，不在本方案）。

**方案一句话**：把 `AtomToolsFramework` 按既有的天然缝切成 `.Core`（Atom-free：Document/DynamicProperty/Inspector/Window）
与 `.Static`（Atom 侧：视口/预览/Graph），CEE 侧以 **dock 面板**形态挂在 `CrossEngineEditor.exe` 内（§6，2026-08-25 改判：
独立 exe 的 5 条理由经源码核实全部不成立），
**100% 复用 Core 的文档系统 + 属性面板 + undo 栈**，只把 `MaterialDocument` 那层"Atom 材质模型"换成
一个 **8 纯虚 + 2 哨兵**的后端契约 `IMaterialSource`。
**不发明中立材质格式，不发明语义标签，不跨引擎搬运内容——跨的是编辑器，不是内容（§A0）。**

**三个关键杠杆**：
① **值不出引擎**（编辑器只持展示快照）→ 往返无损是**结构性**保证，不靠字段覆盖率；
② **schema 由引擎产** → Godot 近乎零成本，rbfx/Filament 各一份 ~80 行 JSON **数据**；
③ **Atom 泄漏只有 4 处调用** → "100% 复用 MaterialEditor 框架"与"Atom 零依赖"可以同时成立。

**代价核算**：上游改动 = 一次纯构建期切分 + 4 处接缝（S1–S4）+ 3 个纯增字段（S2/S5/S6）+ 2 个纯增/纯改名（S8/S9）+ 1 个虚工厂（S7，随 P3），
**上游 MaterialEditor/MaterialCanvas/ShaderManagementConsole 行为零变化**；CEE 侧新增约 12 个文件；工作量 ≈ **8~9 人周**（2026-08-25 opus review 复核重估，§11.7）。
**③ 级自建 = 1**（`CeeMaterialPreviewPanel`，判据 §9.9）；**② 级扩展 = 6**（S2/S5/S6 属转发、S7 属子类化、S8 纯增函数、S9 纯改名）。
**两引擎零 fork**：预览走 RTT 按需回读（§6.4），`ISceneRenderer`/`IViewportTick`/主循环零改动。

---

## §1 需求与边界

### 1.1 目标
- 编辑器框架 **100% 复用** O3DE MaterialEditor 的那套东西：文档系统（开/关/存/另存为副本/撤销重做/外部变更重载）、
  属性检视器（分组折叠、滑杆、枚举下拉、修改指示器）、主窗口菜单。
- 后端渲染引擎**可插拔**：rbfx / Godot / Filament / 自研，与 CEE 场景编辑器同一套 `--backend` 语义。
- 材质文件**存为目标引擎的原生格式**。

### 1.2 硬性边界（继承既有铁律，不重新讨论）

| 规则 | 出处 | 对本方案的约束 |
|---|---|---|
| **C1 单底线**：Atom 渲染模块一律不进依赖图 | Plan.md §A6 | 材质编辑器不得链接任何 `Atom_*`；**Atom 本身因此不能作为后端**（§1.3） |
| **C3 复用优先：① 直接复用 ＞ ② 扩展 ＞ ③ 自建**（须落 `文件:行号` + 非过度设计两条证据） | Plan.md §A6 | 贴图路径 = 官方 `StringFilePath` handler（①）；默认折叠 = 官方虚函数钩子、组头勾选框 = 3 行上游虚工厂 + 子类 paintEvent（②）；预览面板（§6.4）是唯一自建面板（③，判据见 §9.9） |
| **C4 契约纯虚化 + 哨兵默认只能是"不支持"语义 + 数据面禁 Qt 类型** | Plan.md §A6 | `IMaterialSource` 全 POD 值类型；纯虚计数 CI 断言 == 8（哨兵不计数） |
| **§A0：CEE 是跨引擎的编辑器前端，不是跨引擎的内容** | Plan.md §A0 | **直接否决中立材质格式 `.cematerial` 与 `m_semantic` 字段**（§9.1/§9.2） |
| C6：后端切换 = 启动参数，非运行时热切换 | Plan.md §A6 | `--backend` 仍是启动参数（§6.3 挂载点不变）；dock 形态下不再有"材质编辑器单独跑另一个后端"的歧义 |
| 绝不臆想 API | Progress.md:51 | 全文带证据；阶段二吸收项全部独立复核（§11.4） |
| 代码注释纯 ASCII / 文档中文 | Progress.md:44 | 新代码注释英文 |

### 1.3 一个必须说清楚的后果：Atom 不是本工具的后端

C1 禁止链接 Atom，因此 **CrossEngineMaterialEditor 不能编辑 `.material`/`.materialtype`**。
这不是缺陷而是分工：O3DE 原版 MaterialEditor 继续负责 Atom 材质，本工具负责 rbfx/Godot/Filament/自研。
两者共用同一套框架代码（`AtomToolsFramework.Core`），UI 与操作手感一致。

**已裁决不开口子**（2026-08-25，`Plan.md` 变更日志同批落账）：技术上可以单独出一个链 Atom 的
`CrossEngineMaterialEditor.Atom.exe` target 让同一套 UI 编五个引擎的材质，**但不做**——
原版 MaterialEditor 已经把 Atom 材质编得很好，再造一个的用户收益 ≈ 0；
而"单底线"一旦有一个例外，护栏对**其余所有 target** 的约束力就开始打折。

> 三份参考方案均未点出这一点。它直接影响验收预期——不要期待本工具能打开 `StandardPBR.material`。

---

## §2 核心设计：三层模型

```
┌─────────────────────────────────────────────────────────────────────┐
│ L1  编辑器框架层（100% 复用 AtomToolsFramework.Core，零改动）          │
│     AtomToolsDocumentSystem / AtomToolsDocument(undo 栈) /            │
│     DocumentSystemRequestBus（标签页组件不存在 → 单文档 combo，§6.5）       │
│     AtomToolsDocumentInspector / InspectorWidget /                    │
│     DynamicPropertyGroup / DynamicProperty                            │
│     ── 只认 AZStd::any 与 DynamicPropertyConfig，永远不知道有引擎 ──   │
└───────────────────────────────▲─────────────────────────────────────┘
                                │  DocumentObjectInfo / AZStd::any
┌───────────────────────────────┴─────────────────────────────────────┐
│ L2  适配层（CEE 新增，唯一新逻辑，约 120 行核心 + 壳）                 │
│     CeeMaterialDocument : AtomToolsDocument                          │
│       · MaterialTypeDesc + 值 map ──► DynamicPropertyGroup 树         │
│       · MaterialPropertyValue   ◄──► AZStd::any                      │
│       · BeginEdit/EndEdit 快照 diff ──► undo/redo 闭包                │
└───────────────────────────────▲─────────────────────────────────────┘
                                │  IMaterialSource（8 纯虚 + 2 哨兵）
┌───────────────────────────────┴─────────────────────────────────────┐
│ L3  引擎后端（每引擎一份，住在 Backends/）                             │
│     RbfxMaterialSource / GodotMaterialSource / NullMaterialSource     │
│       · 持有**活材质实例** = 值的唯一真相                              │
│       · 原生文件读写、预览场景、MaterialPropertyValue → 引擎值         │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.1 为什么接缝正好在这里（不是拍脑袋，是源码里现成的）

上游 `MaterialDocument` 的数据流是（`MaterialEditor/Code/Source/Document/MaterialDocument.cpp:598-723`）：

```
MaterialTypeSourceData (Atom schema)
  └─ EnumeratePropertyGroups
       └─ ConvertToPropertyConfig(cfg, propertyDefinition)          // :674  schema → 通用 config
       └─ cfg.m_originalValue = ConvertToEditableType(assetValue)   // :699  Atom 值 → AZStd::any
       └─ cfg.m_parentValue   = ConvertToEditableType(parentValue)  // :701
       └─ cfg.m_dataChangeCallback = [] { SetPropertyValue(...) }   // :706-711
  └─ DynamicPropertyGroup ──► DocumentObjectInfo ──► InspectorWidget
```

**Atom 的痕迹全部止步于 `ConvertToPropertyConfig` / `ConvertToEditableType` 这两个函数**
（声明 `AtomToolsFramework/Util/MaterialPropertyUtil.h:19-28`，实现 `Source/Util/MaterialPropertyUtil.cpp:29-68`）。
它们上面的一切（DynamicProperty、Inspector、Document、undo/redo）都是纯 AzCore/Qt。

所以本方案做的事就一句话：**把这两个函数换成 `MaterialTypeDesc + 值 map → DynamicPropertyGroup` 的转换，其余原样复用。**
`MaterialPropertyUtil` 本身留在 Atom 侧不动（上游 MaterialEditor 还要用），CEE 侧的新转换**以它为蓝本**。

### 2.2 领域模型：两层——引擎产 schema + 引擎侧活实例（值不出引擎）

```
[引擎侧]  活材质实例 (rbfx Material* / Godot StandardMaterial3D* / Filament MaterialInstance*)
             │  值的唯一真相；引擎专属字段全程留在这里，编辑器看不见也碰不到
             ├── GetMaterialState ──► MaterialTypeDesc(schema，可见性已按当前值求值) + 当前值 map
             └── SetPropertyValue ◄── 单属性回写（立即生效 → 预览球下一帧即变）
[编辑器侧] MaterialTypeDesc ──► DynamicPropertyGroup ──► AtomToolsDocumentInspector ──► 属性网格
```

三条推论，每条都是设计的承重墙：

- **往返无损是结构性的，不靠字段覆盖率**：保存 = 后端序列化**它自己的活实例**，编辑器从不重新组装材质内容
  → 引擎专属字段（rbfx `<parameteranimation>`/`<depthbias>`/`<renderorder>`、Godot `next_pass`/`stencil_*`/`proximity_fade`）
  即使 schema 未暴露也原样保留。这恰是 O3DE 自己的模型（`MaterialTypeSourceData` + 稀疏覆盖），不是新发明。
- **属性 id = 引擎原生名，无翻译层**：编辑器显示 rbfx 的 `MatDiffColor`、Godot 的 `albedo_color`。
  **id 是扁平的引擎原生名，不做分组点分拼接**——Godot 的 `ADD_GROUP` hint_string 本就是属性名前缀
  （`class_db.cpp:1384-1408`），再点分一次会破坏 `set(name, …)` 的可路由性。分组只是显示层。
- **每个打开的文档 = 一个后端活实例**（不只是预览中的那个）：Godot 的 `_validate_property` 必须在 live 实例上求值
  （`material.cpp:2547-2726`），schema 拉取不能只在激活 tab 上有实例。`SetPreviewMaterial` 只决定预览球绑哪一个。

### 2.3 值模型：编辑器侧 `AZStd::any`，契约侧封闭 variant，引擎值不出后端

- **L1 层**天然用 `AZStd::any`（`DynamicProperty.h:131`），这是既成事实，不动。
- **L2↔L3 契约**用封闭的 10 元 variant（§4.1 `MaterialPropertyValue`）。理由：C4 要求数据面 POD/可序列化；
  封闭集合能编译期穷举转换，漏一种类型编译不过（未来把后端推到子进程/托管桥只换传输层）。
- **引擎原生值类型（`Urho3D::Variant` / `godot::Variant` / `filament::MaterialInstance` 句柄）绝不出现在契约里**，
  一律在后端边缘转换 —— 与 CEE 既有的"空间约定收口后端边缘"（Plan §A6 C4）同构。

**贴图引用 = 路径字符串**，不是 AssetId。依据：rbfx 用相对资源根的资源名（`Material.cpp:366-392`，
`name="Textures/PBR/Lead/Albedo.jpg"`）；Godot 用 `res://` 路径（`resource_format_text.cpp:1882-1889`）；
Filament 从图片文件建 Texture。三者都是路径语义，AssetId 对它们无意义。
**路径规范化归后端**：官方文件拾取控件返回的是 O3DE 别名路径（`PropertyStringBrowseEditCtrl.cpp:263` `GetPathWithAlias`），
后端在 `SetPropertyValue` 里转成自己的资源名——同"空间约定收口后端边缘"。

---

## §3 L1：AtomToolsFramework 的 Core/Atom 切分

### 3.1 实测依据

对 `Gems/Atom/Tools/AtomToolsFramework/Code` 全量 grep `#include <Atom/`，按目录统计（头文件 / 全部文件）：

| 目录 | 头文件 Atom 引用 | 归属 |
|---|---|---|
| `Document/`（AtomToolsDocument / DocumentSystem / TypeInfo / Inspector / MainWindow / CreateDocumentDialog + 4 条总线） | **0 / 14** | **Core**（多 tab / undo 栈 / dirty / 最近文件 / 另存为 / 关闭确认 / 热重载队列） |
| `DynamicProperty/` | **0 / 2** | **Core** |
| `Inspector/`（含 `PropertyWidgets/`） | **0 / 6**（.cpp 也是 0） | **Core** |
| `Window/AtomToolsMainWindow*` | **0 / 3** | **Core** |
| `SettingsDialog/` `Communication/`(LocalServer/Socket) `Debug/TraceRecorder` | **0** | **Core** |
| `AssetBrowser/` `AssetSelection/` | 0 头 / 1 cpp（未使用 include） | **Core** |
| `PerformanceMonitor/` | 0 头（Bus 头干净）/ 2 cpp | Bus 头 → Core；SystemComponent 实现 → Atom |
| `Util/Util.*` | 1 头 / 2 调用点 | Core（去 2 处调用，见 S4） |
| `Document/AtomToolsAnyDocument.*` | 1 头 / 3 调用 | **Atom**（见 §3.3 判定） |
| `Document/AtomToolsDocumentApplication.*` `Application/` | 0 头 / 各 1 cpp | **Atom**（RPI 初始化 + AP 连接） |
| `Util/MaterialPropertyUtil.*` | 3 | **Atom** |
| `Viewport/` | 3 / 4 | **Atom** |
| `EntityPreviewViewport/` | 5 / 10 | **Atom** |
| `PreviewRenderer/` | 1 / 5 | **Atom** |
| `Graph/`（MaterialCanvas 用，框架本身引擎无关，v2 再评） | 0 头 / 1 cpp | **Atom** |

### 3.2 全部改动 —— 4 处 Atom 接缝 + 3 个纯增字段 + 2 个纯增/纯改名（S8/S9）

| # | 位置 | 现状 | Core 侧做法 |
|---|---|---|---|
| **S1** | `Source/Document/AtomToolsDocument.cpp:368` | `AZ::RPI::AssetUtils::ResolvePathReference(scanFolder, relativePath)` | 位于 `SourceFileChanged`（AssetProcessor 通知路径，CEE 无 AP = 死分支）。抽 `AtomToolsFramework::ResolvePathReference`：`AZ::IO::Path` + `AzFramework::StringFunc::Path::Normalize` + 存在性检查，语义等价 |
| **S2** | `Source/DynamicProperty/DynamicProperty.cpp:9,167` | 硬编码 `AZ::RPI::ColorUtils::GetLinearRgbEditorConfig()` | **新增 `DynamicPropertyConfig::m_colorSpace`**（Linear/Srgb），把 `GetLinearRgbEditorConfig()` 与 `GetRgbEditorConfig()` **两个**一起下沉 Core。该类型本就是 AzToolsFramework 的（`ColorUtils.h:12` 只 include `AzToolsFramework/UI/PropertyEditor/PropertyColorCtrl.hxx`），零新依赖 |
| **S3** | `Source/Window/AtomToolsMainWindow.cpp:9,613-615` | `AZ::RHI::Factory::IsReady()` / `Get().GetName()` 取渲染器名做状态栏 | 提为虚函数 `virtual AZStd::string GetRendererDisplayName() const { return {}; }`，Atom 侧 override 返回 RHI 名，CEE 侧返回后端名（"rbfx"/"godot"）——顺带成为一个功能点 |
| **S4** | `Source/Util/Util.cpp:9-12, 60-62, 614` | `ImageProcessingAtom::LoadImagePreview`（缩略图）+ `AZ::RPI::AssetUtils::GetSourcePathByAssetId` | 两个函数整体移到 Atom 侧的 `AtomUtil.cpp`；Core 版 `Util` 保留路径/设置/`LaunchTool` 等全部纯逻辑（CEE 用扩展名图标，AssetBrowser 判例已定，`rbfx_migration.md` §2.3） |
| **S5** | `Source/DynamicProperty/DynamicProperty.cpp:130-181` | `UpdateEditData()` **不转发** `Extensions` / `Title` 属性 | **新增 `DynamicPropertyConfig::m_fileExtensions`**，在 `UpdateEditData` 加两行 `AddEditDataAttribute(AZ_CRC_CE("Extensions"), …)` / `("Title", …)`。**这不是 Atom 泄漏，是功能缺口**——详见 §3.4，四份方案全漏 |
| **S6** | 同上 | `UpdateEditData()` 也**不转发** `Suffix` | **新增 `DynamicPropertyConfig::m_suffix`** + 一行 `AddEditDataAttribute(AZ::Edit::Attributes::Suffix, …)`。与 S5 同型同批。详见 §3.4b —— **本文初稿曾错误地把"单位显示"归为 C3 违例（要自建控件），实为白送** |
| **S8** | `AtomToolsFrameworkSystemComponent.cpp:34-48` | 5 个 Core 类的 `Reflect`（`AtomToolsDocument`/`AtomToolsDocumentSystem`/`DynamicProperty`/`DynamicPropertyGroup`/`InspectorWidget`）**唯一调用点**在该系统组件里，而它同时反射 `DynamicNode`/`GraphCompiler`/`GraphDocument` 等 Graph 系，**必须归 `.Static`** | **Core 侧加自由函数 `AtomToolsFramework::ReflectCoreTypes(AZ::ReflectContext*)`**（把 5 个 `Reflect` 调用搬进去，~10 行），`AtomToolsFrameworkSystemComponent::Reflect` 改为调用它（上游行为零变化）；CEE 在 `CrossEngineEditorApplication::Reflect`（`CrossEngineEditorApplication.cpp:107-114`）调一次。**没有它，EditContext 不注册、属性网格一行都渲染不出来**（2026-08-25 opus review 实锤） |
| **S9** | `Document/AtomToolsDocument.h:16-19,95` | 头文件里声明 `namespace AZ::RPI::MaterialUtils { using ImportedJsonFiles = … }` 并用作成员 `m_sourceDependencies`——**不是 include**（§3.1"头文件引用 = 0"的统计没错），但每个 include 该头的 TU 都命中精化后第 1 层禁词 `AZ::RPI` | **切分时把别名改名为 `AtomToolsFramework::SourceDependencySet`**（同一类型），Atom 侧写入点（`AtomToolsAnyDocument`/`MaterialDocument` 少数几处）同批改。**护栏拦到自己时改代码、不改护栏**——第 1 层永远只是代理指标，第 3 层构建断言才是真保证（§3.6） |

另有 4 个文件（`AtomToolsDocumentMainWindow.cpp:9-10`、`CreateDocumentDialog.cpp:9-10`、
`AtomToolsAssetBrowserInteractions.cpp:9`、`AtomToolsDocumentApplication.cpp:9-10`）只是 **include 了 AssetUtils 但没有调用点**
（全量 grep `AssetUtils::` 只命中 S1/S4）—— 直接删 include。

S2/S5/S6 三个新字段都是**带默认值的纯增字段**，上游 MaterialEditor 不填 = 行为完全不变。
三者是**同一类改动**（把 stock 控件已经能消费、而 `DynamicProperty` 恰好没转发的属性补上），建议同一个 PR 提交。

**另有 S7，排期不同**：`InspectorWidget` 加 `virtual InspectorGroupHeaderWidget* CreateGroupHeader()`
（默认返回原类，3 行，上游行为零变化），解锁 CEE 侧的组头勾选框。
它**不属于 P0 那批**——唯一消费者是 Godot 后端，随 **P3** 一起提交（论证与选项对比见 §9.4）。

### 3.3 与三份方案的差异：为什么是 4 处而不是 7 处

三份方案一致给出 **7 处泄漏点**，多出的 3 处全部来自 `Document/AtomToolsAnyDocument.cpp`
（`:282/:337` 的 `RPI::JsonReportingHelper`、`:11` 的 `AnyAsset.h` include），处置方案是"换 `AZ::JsonSerializationResult`"。

**本文判定：`AtomToolsAnyDocument` 整体归 Atom 侧，那 3 处不需要处理。** 理由：
- 该类的存在意义就是"编辑存放在 `AZ::RPI::AnyAsset` 里的任意反射类型"，消费者是 ShaderManagementConsole 与 PassCanvas，
  **CEE 的材质文档不经过它**（`CeeMaterialDocument` 直接继承 `AtomToolsDocument`）；
- `AtomToolsDocumentSystem` 按 `DocumentTypeInfo` 泛型注册文档类型，不硬依赖 AnyDocument；
- 文件级归属本来就是逐文件分配到两份 `_files.cmake`，同目录拆分零成本。

**净效果：少改 3 处上游代码，风险更低。** 若将来 CEE 确需通用反射文档，A/B/C 三稿已把那 3 处的改法写好，直接取用即可（退路已备）。

### 3.4 四方全漏的缺口：贴图拾取器的扩展名过滤（S5）

三份方案都写了"纹理槽 = `AZStd::string` + 官方 `StringFilePath` handler + `m_fileExtensions` 过滤，新增控件数 = 0"。
**前半句成立，后半句不成立**——独立复核结果：

- 控件确实存在且完全够用：`PropertyStringFilePathCtrl : PropertyStringBrowseEditCtrl`
  （`Source/Inspector/PropertyWidgets/PropertyStringBrowseEditCtrl.h:85-98`），handler 名 `AZ_CRC_CE("StringFilePath")`（`:109`），
  `EditValue()` 直接弹文件对话框（`.cpp:259-266`），`ConsumeAttribute` 认 `AZ_CRC_CE("Extensions")`/`("Extension")`/`("Title")`
  （`.cpp:212-257`，扩展名接受 `string` / `vector<string>` / `vector<pair<string,string>>` 三种形态）。**且该文件零 Atom include。**
- **但 `DynamicProperty::UpdateEditData()`（`DynamicProperty.cpp:130-181`）从不转发 `Extensions` 或 `Title`。**
  它转发的是 NameLabelOverride / DescriptionTextOverride / ReadOnly / ChangeNotify / **AssetPickerTitle** /
  ShowProductAssetFileName / Thumbnail / SupportedAssetTypes / Handler / 范围 / 向量标签 / ColorEditorConfiguration / EnumValues。
  注意 `AZ::Edit::Attributes::AssetPickerTitle == AZ_CRC_CE("AssetPickerTitle")`（`EditContextConstants.inl:122`），
  **与控件要的 `AZ_CRC_CE("Title")` 不是同一个 CRC**。
- 结论：不补 S5，`m_fileExtensions` 是**写了不生效的死字段**，贴图对话框会列出所有文件。
  补 S5 = `DynamicPropertyConfig` 加一个 `AZStd::vector<AZStd::string> m_fileExtensions` + `UpdateEditData` 加两行。

**这条正是"不臆想 API"铁律的价值所在**：三份方案都在同一句话上凭合理推断跳过了一次验证。

### 3.4b 同一个缺口的第二处：单位后缀（S6）—— **本文初稿自己的错误**

初稿 §9.8 曾写"单位/subtype 控件 → O3DE 属性网格无 stock 控件，做了要写新 handler → 违 C3"。**这是错的**，
而且错法与 §3.4 批评三稿的完全一样：**没 grep 就下结论，然后拿规则当挡箭牌**。核实结果：

- `AZ::Edit::Attributes::Suffix` 存在（`EditContextConstants.inl:154`）；
- 且被 stock 数值控件**全家消费**：`PropertyDoubleSliderCtrl.cpp:246`、`PropertyDoubleSpinCtrl.cpp:262`、
  `PropertyIntCtrlCommon.h:218`（int 一族模板）、`PropertyVectorCtrl.cpp:40`（向量一族）；
- 缺的**只是转发**——`DynamicPropertyConfig` 无 `m_suffix`，`UpdateEditData()` 不转发它。

即：**单位显示是白送的，零自建控件，完全不触碰 C3**，代价是与 S5 同型的一行转发（S6）。
而消费者现成：Godot 的 `hint_range` 本就带 `suffix:m` / `suffix:°`（`property_info.h:41` 的格式定义里
`or_greater`/`suffix:` 是同一串后缀选项），rbfx 的偏移/偏置类参数也有天然单位。

> **落账为教训**：C3 的本意是"不重复造轮子"而非"禁止扩展"（`Plan.md` §A6 C3）；往 stock 控件转发一个**已被消费**的属性
> 连"扩展"都算不上（S2 / S5 / S6 同理）。**"把没查说成规则不让"——规则被当成了省掉验证的借口**，是本方案三处初稿错误
> （§11.4-5/6/7）的共同根因；C3 判定手段 (a) 项"必须带 `文件:行号`"的硬要求即为此设。

### 3.5 CMake 形态（上游零破坏）

```cmake
# Gems/Atom/Tools/AtomToolsFramework/Code/CMakeLists.txt
ly_add_target(NAME ${gem_name}.Core STATIC NAMESPACE Gem
    AUTOMOC AUTOUIC AUTORCC
    FILES_CMAKE atomtoolsframework_core_files.cmake      # 新增清单
    INCLUDE_DIRECTORIES PUBLIC Include PRIVATE Source
    BUILD_DEPENDENCIES PUBLIC
        3rdParty::Qt::Core 3rdParty::Qt::Gui 3rdParty::Qt::Network 3rdParty::Qt::Widgets
        AZ::AzCore AZ::AzFramework AZ::AzQtComponents AZ::AzToolsFramework)   # 无任何 Atom_*

ly_add_target(NAME ${gem_name}.Static STATIC NAMESPACE Gem ...
    BUILD_DEPENDENCIES PUBLIC
        Gem::${gem_name}.Core                            # ← 新增一行
        Gem::Atom_RPI.Edit Gem::Atom_RPI.Public ...      # 其余原样（CMakeLists.txt:30-51）
)
```

上游 MaterialEditor / MaterialCanvas / ShaderManagementConsole / PassCanvas 继续
`#include <AtomToolsFramework/...>` 并链 `AtomToolsFramework.Static` —— **命名空间不改、头文件路径不改、一行代码不改**。

**`3rdParty::Python` 不进 Core**：现状在 `.Static` 依赖表里（`CMakeLists.txt:30-51`），源码中只有
`AzToolsFramework` 的 `EditorPythonConsoleBus` 引用（`AtomToolsApplication.cpp:24-25` 等），无直接 Python C API 调用——
由 AzToolsFramework 传递满足，直接依赖是历史遗留，留在 `.Static` 即可。

**否决 vendoring**（A 稿初版曾提）：`AtomToolsFramework` 是**同仓第一方**代码，vendoring = 仓内造永久分叉（上游修 bug 收不到）。
Qt-ADS / Diligent / tracy 三个 vendoring 判例都是**第三方**库，判例不适用。
CMake 切分是**同一份源码文件**的复用，是 C3 的最强形式 —— **落账为 C3 判例 3**。
**退路（R1）**：若上游切分受阻，退回 vendoring 同一子集（清单即 §3.1 表），代价 = 永久分叉，故为退路非首选。

### 3.6 `check_no_atom.ps1` 同步更新（唯一需要放宽的护栏）

现行脚本第 1 层对 `Code/Source` 去注释后**区分大小写 grep 裸词 `Atom`**。CEE 材质编辑器会出现
`#include <AtomToolsFramework/Document/AtomToolsDocument.h>`，会误判为违例。

**这不是放宽 C1，是修一个与规则不一致的代理指标。** C1 规则原文（`Plan.md:68`）说的是
"O3DE Atom **渲染模块**完全剔除（**渲染类模块**一律不进依赖图）；编辑器框架所需的**基础模块依赖可以引入**
（…**以框架实际需要为准，不列白名单**）"——`AtomToolsFramework.Core` 是 Atom-free 的**编辑器框架模块**，
按规则原文本就允许。挡住它的是检验手段里的**字面匹配**（`Atom*` 通配 + grep 裸词 `Atom`），
**它拦的是名字，不是依赖**，比规则本身严且严错了对象。
**2026-08-25 裁决：只精化脚本，C1 原文不动**（`Plan.md` 变更日志已落账）。

**更新原则：把"禁裸词 Atom"精化为"禁 Atom 渲染模块"，第 3 层构建断言不变（那才是真正的保证）。**

```powershell
# 第 1 层改为：禁止渲染类命名空间/头文件
$forbidden = @(
    'Atom/RPI', 'Atom/RHI', 'Atom/Feature', 'Atom/Bootstrap',
    'Atom/ImageProcessing', 'Atom/Component', 'AtomLyIntegration',
    'AZ::RPI', 'AZ::RHI', 'AZ::Render'
)
# 白名单：AtomToolsFramework（Atom-free 的 .Core target）
# 第 2 层：CMakeLists 允许 Gem::AtomToolsFramework.Core，仍禁 Atom_RPI/Atom_RHI/Atom_Feature 等
# 第 3 层不变：vcxproj 的 AdditionalDependencies 不得含 Atom_*.lib（真正的硬保证）
# 同批新增：契约不增长断言 —— IMaterialSource 纯虚计数 == 8（grep '= 0;'；哨兵不计数）
```

C1 的实质是"渲染类模块不进依赖图"，第 3 层断言精确覆盖了它。放宽第 1 层是把代理指标换成真实指标，
按 Plan §A6 C1 "BUILD_DEPENDENCIES 变更须 review 说明"走一次评审即可。

---

## §4 L3：后端契约 `IMaterialSource`

### 4.1 头文件（完整草案）

放在 `Gems/CrossEngineEditor/Code/Source/BackendAPI/IMaterialSource.h`，风格对齐 `IAssetSource.h`。

```cpp
/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Material data source feeding the reused AtomToolsFramework document + inspector stack
//! (material_migration_final.md SS2, SS4). Fourth sub-contract on IEngineBackend.
//!
//! Two-layer model: the backend owns the LIVE material instance (the only source of truth
//! for values) and hands the editor a schema plus a value map. The editor turns that into
//! DynamicPropertyGroup / DynamicProperty and lets the stock ReflectedPropertyEditor render
//! it. Edited values come back one at a time; the backend applies them to its live material
//! so the preview updates, and serializes that same instance on save. Round-trip fidelity is
//! therefore structural: engine-specific fields the schema never exposes are never touched.
//!
//! Contract rules (Plan SSA6 C4): every value type below is POD or AZStd, no Qt types, no
//! engine types. Engine-native values (Urho3D::Variant, godot::Variant,
//! filament::MaterialInstance) stay inside the backend. Pure virtual count is asserted at 8
//! by check_no_atom.ps1 (sentinels are not counted) - new capabilities go through a sentinel
//! default.

#include <AzCore/Math/Color.h>
#include <AzCore/Math/Vector2.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/Math/Vector4.h>
#include <AzCore/base.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/containers/variant.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/optional.h>
#include <AzCore/std/string/string.h>
#include <AzCore/std/string/string_view.h>

namespace CrossEngineEditor
{
    //! Backend-allocated opaque handle for one live material instance. 0 = invalid.
    //! Same shape as IEntityMirror::CreateObject returning an AZ::EntityId (IEntityMirror.h:80).
    using MaterialHandle = AZ::u64;

    //! Closed value set shared by every engine's material parameters. Texture and resource
    //! references travel as engine-relative path strings (rbfx resource name, Godot res://
    //! path, Filament image path) - an O3DE AssetId would be meaningless to those engines.
    using MaterialPropertyValue = AZStd::variant<
        AZStd::monostate,   //!< unset / invalid
        bool,
        AZ::s32,
        AZ::u32,
        float,
        AZ::Vector2,
        AZ::Vector3,
        AZ::Vector4,
        AZ::Color,
        AZStd::string>;

    using MaterialPropertyValueMap = AZStd::unordered_map<AZStd::string, MaterialPropertyValue>;

    //! Declared type of a property. Drives both the value alternative and the stock editor:
    //! Enum -> u32 index + combo box, Texture -> string + file browse control.
    enum class MaterialPropertyType : AZ::u8
    { Bool, Int, UInt, Float, Vec2, Vec3, Vec4, Color, Enum, Texture, String };

    //! Color space the stored value lives in, so the picker shows the artist the right swatch.
    //! Getting this wrong is the classic cross-engine color bug: rbfx stores MatDiffColor and
    //! MatEmissiveColor in GAMMA space (its own glTF importer calls LinearToGamma explicitly,
    //! GLTFImporter.cpp:2522) while Atom, Filament and Godot store linear.
    enum class MaterialColorSpace : AZ::u8 { Linear, Srgb };

    //! Outcome of one property edit. Tri-state so the editor knows whether the schema and the
    //! values have to be re-read (mirrors the tri-state IEntityMirror::RaycastNode convention).
    enum class SetResult : AZ::u8
    {
        Failed,               //!< unknown property or value rejected; editor reverts
        Applied,              //!< value applied, nothing else changed - refresh the group only
        AppliedSchemaChanged, //!< visibility set and/or OTHER property values may have changed;
                              //!< the editor calls GetMaterialState again and rebuilds the group
    };

    //! Preview mesh. Deliberately three shapes: every engine's own material inspector offers
    //! exactly sphere / box / plane (Godot material_editor_plugin.cpp:264-448).
    enum class PreviewModel : AZ::u8 { Sphere, Cube, Plane };

    //! Outcome of AcquirePreviewImage. The panel keeps its existing image on Unchanged, so a
    //! backend that has not rendered the requested frame yet costs one extra 60fps gate, never
    //! a readback. Unsupported is the sentinel default: backends without preview capability
    //! return it and the panel shows a placeholder.
    enum class PreviewResult : AZ::u8 { Unsupported, Unchanged, Updated };

    //! One editable property. Pure metadata plus a default - the CURRENT value travels
    //! separately in MaterialPropertyValueMap, so this struct is also exactly what a curated
    //! schema JSON file deserializes into (rbfx / Filament, SS7).
    struct MaterialPropertyDesc
    {
        AZStd::string m_id;           //!< engine-native property name, flat and globally
                                      //!< unique inside the material. The editor only routes
                                      //!< it back verbatim; it never parses or prefixes it.
        AZStd::string m_displayName;  //!< falls back to m_id when empty
        AZStd::string m_description;  //!< tooltip. Treat as required (Blender precedent);
                                      //!< the schema validator warns on empty.

        MaterialPropertyType m_type = MaterialPropertyType::Float;
        MaterialPropertyValue m_defaultValue;   //!< drives the "modified" indicator

        //! Numeric range hints. Hard range clamps on write; soft range only bounds the slider
        //! (Godot "or_greater", Blender rna_def_property_ui_range). Applied to Int / UInt /
        //! Float / Vec2-4 (per component).
        AZStd::optional<double> m_min, m_max, m_softMin, m_softMax, m_step;

        //! Unit shown after the number ("m", "deg", "nits"). Free at the widget level - every
        //! stock numeric control already consumes AZ::Edit::Attributes::Suffix. Engine-native
        //! unit, never normalized across engines (Plan SSA6 C4 normalization scope).
        AZStd::string m_suffix;

        AZStd::vector<AZStd::string> m_enumValues;     //!< Enum: display names, index = value
        AZStd::vector<AZStd::string> m_vectorLabels;   //!< {"X","Y","Z"} / {"U","V"}
        AZStd::vector<AZStd::string> m_fileExtensions; //!< Texture: picker filter, e.g. {"png","dds"}

        MaterialColorSpace m_colorSpace = MaterialColorSpace::Linear;  //!< Color only

        bool m_visible = true;    //!< backend already evaluated it against the current values
        bool m_readOnly = false;
    };

    //! A group of properties, one level of nesting (matches Godot GROUP / SUBGROUP).
    struct MaterialPropertyGroupDesc
    {
        AZStd::string m_id;
        AZStd::string m_displayName;
        AZStd::string m_description;
        bool m_defaultCollapsed = false;   //!< advanced groups start folded (Blender precedent)

        //! Optional: id of a Bool property inside this group that gates the whole feature.
        //! When set, the editor hoists that checkbox into the group header and hides its
        //! ordinary row (SS5.4 / S7). Empty means an ordinary group. Godot fills this for
        //! free from PROPERTY_HINT_GROUP_ENABLE - BaseMaterial3D uses it 15 times
        //! (material.cpp:3603-3734); rbfx and Filament leave it empty.
        AZStd::string m_toggleProperty;

        AZStd::vector<MaterialPropertyDesc> m_properties;
        AZStd::vector<MaterialPropertyGroupDesc> m_groups;
    };

    //! The schema of one open material: pure metadata, no values.
    struct MaterialTypeDesc
    {
        AZStd::string m_id;           //!< matches a MaterialTypeInfo::m_id
        AZStd::string m_displayName;
        AZStd::string m_description;
        AZStd::vector<MaterialPropertyGroupDesc> m_groups;
    };

    //! A material template the engine can instantiate: an rbfx technique + defines combo, a
    //! Godot material class, a Filament .filamat package.
    struct MaterialTypeInfo
    {
        AZStd::string m_id;           //!< engine-native identifier, round-tripped verbatim
        AZStd::string m_displayName;
        AZStd::string m_description;
        AZStd::vector<AZStd::string> m_fileExtensions;  //!< feeds DocumentTypeInfo's
                                                        //!< m_supportedExtensionsToCreate/Open/Save
    };

    class IMaterialSource
    {
    public:
        virtual ~IMaterialSource() = default;

        // ---- discovery -------------------------------------------------------------
        //! Templates offered in the New Material dialog; the union of m_fileExtensions also
        //! defines the file filters the document type registers with.
        virtual void EnumerateMaterialTypes(AZStd::vector<MaterialTypeInfo>& out) = 0;

        // ---- live instance lifetime ------------------------------------------------
        //! One instance per open document, not just the previewed one: Godot's
        //! _validate_property only evaluates on a live instance (material.cpp:2547-2726).
        [[nodiscard]] virtual MaterialHandle CreateMaterial(AZStd::string_view typeId) = 0;
        //! Load into the ENGINE'S SHARED CACHE and return the cached instance (rbfx:
        //! ResourceCache::GetResource(type, name, false) - CEE precedent RbfxBackend.cpp:1341;
        //! Godot: ResourceLoader::load(..., CACHE_MODE_REUSE)) - NOT a private copy. Only the
        //! shared instance keeps "edits visible in the main viewport" true (6.1 positive
        //! reason 4). DestroyMaterial releases the editor's reference only; it never rolls
        //! back values already pushed to the live instance (Revert = v1.5 via re-Load).
        [[nodiscard]] virtual MaterialHandle LoadMaterial(AZStd::string_view absolutePath) = 0;
        virtual void DestroyMaterial(MaterialHandle handle) = 0;

        // ---- read / edit -----------------------------------------------------------
        //! Schema (with m_visible / m_readOnly already evaluated against the current values)
        //! plus the current value map. Called on open and again after AppliedSchemaChanged.
        [[nodiscard]] virtual bool GetMaterialState(
            MaterialHandle handle, MaterialTypeDesc& outSchema, MaterialPropertyValueMap& outValues) = 0;

        //! Apply one edited value to the live material. propertyId is a MaterialPropertyDesc
        //! m_id. Must be visible in the preview within one frame; the backend may defer the
        //! actual GPU work to its own tick. Applied means the value took effect AS-IS: the
        //! backend must NOT silently clamp (hard ranges belong in the schema m_min/m_max),
        //! and any backend-side rewrite of OTHER values must return AppliedSchemaChanged.
        [[nodiscard]] virtual SetResult SetPropertyValue(
            MaterialHandle handle, AZStd::string_view propertyId, const MaterialPropertyValue& value) = 0;

        // ---- persist ---------------------------------------------------------------
        //! Serialize the live instance to the engine's native format. absolutePath is absolute.
        [[nodiscard]] virtual bool SaveMaterial(MaterialHandle handle, AZStd::string_view absolutePath) = 0;

        // ---- preview ---------------------------------------------------------------
        //! Bind this material to the preview object. The backend creates its preview scene
        //! (mesh + light + environment) lazily on the first call. 0 clears it.
        virtual void SetPreviewMaterial(MaterialHandle handle) = 0;

        //! Sentinel default - "not supported" only (Plan §A6 C4). Default: fixed sphere.
        virtual bool SetPreviewModel(PreviewModel model) { (void)model; return false; }

        //! Sentinel default - "not supported" only. Copy the preview image as tightly
        //! packed RGBA8 (length = width*height*4, no row padding) into outPixels.
        //!
        //! Contract (see §6.4.4): AcquirePreviewImage is only called while the panel is
        //! dirty, i.e. after SetPropertyValue / SetPreviewMaterial / SetPreviewModel. The
        //! backend marks its preview render target dirty on those three calls and renders
        //! it on demand (rbfx: SURFACE_MANUALUPDATE + QueueUpdate, RenderSurface.cpp:79-82;
        //! Godot: VIEWPORT_UPDATE_ONCE, renderer_viewport.cpp:955-957). It returns Unchanged
        //! until that frame is ready; in steady state the panel makes no calls at all, so
        //! no readback and no GPU stall ever happens unattended.
        [[nodiscard]] virtual PreviewResult AcquirePreviewImage(
            uint32_t width, uint32_t height, AZStd::vector<AZ::u8>& outPixels)
        {
            (void)width; (void)height; (void)outPixels;
            return PreviewResult::Unsupported;
        }
    };
} // namespace CrossEngineEditor
```

**规模：8 纯虚 + 2 哨兵。** 对比既有契约：`ISceneRenderer` 9 纯虚、`IEntityMirror` 15 纯虚 + 4 哨兵、`IAssetSource` 2 纯虚。
位于中间，且没有一个方法是"为了将来"预留的。两个哨兵（`SetPreviewModel` / `AcquirePreviewImage`）都是"无此能力的后端什么都不用做"，
而真正渲染的后端把两者当成完整的预览契约——**预览整条链路不进 `ISceneRenderer`，主视口契约一行不动**（§6.4.3）。

### 4.2 挂载点：`IEngineBackend` 加一个 getter

```cpp
// BackendAPI/IEngineBackend.h
virtual IMaterialSource& GetMaterialSource() = 0;   // 与 GetSceneRenderer/GetEntityMirror/GetAssetSource 同构
```

按 C4，同一变更内补齐全部 4 个后端：rbfx 真实现；Godot v1.5 真实现；Diligent/Null 返回共享的 `NullMaterialSource`
（`EnumerateMaterialTypes` 返回内置 "Demo PBR"、`SetPropertyValue` 写内存 map、`SaveMaterial` 落 JSON）。
Diligent 已有"内嵌 NullBackend 委托"的先例（`DiligentBackend.h:60-62`），照抄即可。

### 4.3 为什么每个方法都必要（逐条辩护）

| 方法 | 不能省的理由 |
|---|---|
| `EnumerateMaterialTypes` | New Material 对话框 + 文档类型的扩展名过滤都要它；且这是唯一一次性调用 |
| `CreateMaterial` | 新建材质必须由引擎给出该类型的默认值集（Godot 构造函数 `material.cpp:3908-3979` 给了 40+ 个默认值，编辑器无从得知） |
| `LoadMaterial` / `DestroyMaterial` | 多标签页文档 × 每文档一活实例，必须有生命周期 |
| `GetMaterialState` | schema + 值的唯一读取口。合并成一次调用是因为它们总是一起用；schema 与值分离是为了让 schema 能直接从 JSON 反序列化（§7.1） |
| `SetPropertyValue` | 实时预览 + 撤销重做的执行原语（undo 就是反向调它） |
| `SaveMaterial` | 存原生格式，各引擎写法完全不同（rbfx `Material::Save(XMLElement)`；Godot `ResourceSaver::save`） |
| `SetPreviewMaterial` | 预览面板要知道渲染哪个材质；多标签页切换时必须换 |
| `SetPreviewModel`（哨兵） | 球/立方/平面切换是材质编辑刚需，但不是每个后端 v1 就有 —— 正是哨兵默认的用途 |
| `AcquirePreviewImage`（哨兵） | 预览像素的唯一取回口（RTT + 按需回读，§6.4）。进契约而不进 `ISceneRenderer`：预览是**材质面**的能力（脏位只由材质面的三个入口置起），且不需要表面/相机/overlay 那套场景语义。哨兵默认让无预览后端零负担 |

### 4.4 明确**不进**契约的东西

- ❌ `Undo/Redo` —— 编辑器侧用值快照 + 闭包实现（`AtomToolsDocument.h:103-120`），后端只需支持重复 `SetPropertyValue`。
- ❌ `IsModified` —— 编辑器比对当前值与打开时的快照。
- ❌ 父材质 / 继承链 / `SaveAsChild` —— Atom 专属（`MaterialSourceData.h:62`），rbfx/Godot/Filament 都没有；
  保存时必须扁平化 → 不可 round-trip。**连哨兵都不留**（哨兵也是死代码）。
  `MaterialPropertyDesc::m_defaultValue` 用**材质类型默认值**充当"父值"，修改指示器语义变成"与类型默认值不同"，
  三个引擎都成立且对用户更直观。
  *（注意区分：`CeeMaterialDocument` 仍要 override 基类的 `SaveAsChild`/`CanSaveAsChild` 返回 false，那是框架虚函数，不是契约方法。）*
- ❌ 批量 `SetPropertyValues` —— 属性网格一次只改一个；undo 恢复多个值时循环调用即可，没有性能问题（§4.6）。
- ❌ 语义标签 `m_semantic` / 中立格式 —— §9.1/§9.2。
- ❌ 声明式可见性谓词 DSL / Lua functor —— §9.3。
- ❌ ~~组头勾选框 `m_toggleProperty`~~ —— **2026-08-25 已采纳（S7）**，字段进契约、控件实现排 P3，见 §9.4。
- ❌ 采样器复合类型 / 矩阵类型 —— §9.6。
- ❌ 贴图通道选择器特化 —— Godot 的 `*_texture_channel` 就是普通 Enum 反射属性，schema 已能表达。

### 4.5 一个 tri-state 统吃四套动态元数据机制

`SetResult::AppliedSchemaChanged` 的两类触发源：

1. **可见性变化**：Godot 里就是引擎自己调 `notify_property_list_changed()` 的那些 setter
   （`set_feature` / `set_flag` / `set_texture` / `transparency`，`material.cpp:2468-2519`）；
   Atom 里就是 EditorContext functor（`MaterialFunctor.h:211-262`）；
   rbfx 里是改 technique / vs·ps defines（会 `CloneWithDefines`，`Material.cpp:1100-1113`）。
2. **后端规范化改了别的属性的值**：Godot 官方 UX 在赋 metallic 贴图时自动把 factor 置 1.0
   （否则 metallic=0 × 贴图 = 全黑，`material_editor_plugin.cpp:473-506`）。
   **规范化后的值必须回显**，否则面板与引擎不一致 —— 这正是 `GetMaterialState` 同时返回 schema **与值**的原因。

**否决谓词 DSL**（`m_visibleWhen = {controlProperty, op, value}` 之类）：重建通道存在后谓词是冗余的第二套机制，
且 Godot 的真实规则（`material.cpp:2547-2726` 包含"按属性名前缀批量隐藏"、"ORM 模式下动态改 hint_string"）谓词表达不了，
最终仍要 fallback 到重建。**一套机制、后端算、前端渲染。**

无死循环风险：重取只发生在 `SetPropertyValue` 返回后，不再触发新的 Set；undo 快照已在 `BeginEdit` 存好，
回放时会重现同一次规范化。

### 4.6 性能：为什么 `GetMaterialState` 全量返回不是问题

Godot 的 `BaseMaterial3D` 约 90 个属性，一次 `MaterialTypeDesc` + 值 map 拷贝约 30KB。
但**只有 `AppliedSchemaChanged` 才触发重读**：拖动滑杆改 `roughness` 返回 `Applied`，全程不重读；
只有勾选 `emission_enabled` 这类会改可见性的操作才返回 `AppliedSchemaChanged`。
后者是人手点击级的低频交互，30KB 拷贝完全无感。

---

## §5 L2：`CeeMaterialDocument` 适配层

### 5.1 类骨架

```cpp
// Source/MaterialEditor/CeeMaterialDocument.h
class CeeMaterialDocument final : public AtomToolsFramework::AtomToolsDocument
{
public:
    AZ_RTTI(CeeMaterialDocument, "{...}", AtomToolsFramework::AtomToolsDocument);

    static AtomToolsFramework::DocumentTypeInfo BuildDocumentTypeInfo();   // 扩展名来自后端

    // AtomToolsDocument overrides
    AtomToolsFramework::DocumentObjectInfoVector GetObjectInfo() const override;
    bool Open(const AZStd::string& loadPath) override;
    bool Save() override;
    bool SaveAsCopy(const AZStd::string& savePath) override;
    bool SaveAsChild(const AZStd::string&) override { return false; }   // no inheritance (SS4.4)
    bool CanSaveAsChild() const override { return false; }
    bool IsModified() const override;
    bool BeginEdit() override;
    bool EndEdit() override;
    void Clear() override;

private:
    void RebuildFromBackend();                       // GetMaterialState -> m_groups
    void ApplyEdit(const AZ::Name& id, const AZStd::any& value);

    IMaterialSource* m_source = nullptr;             // borrowed from IEngineBackend
    MaterialHandle m_handle = 0;                     // this document's live instance
    AZStd::vector<AZStd::shared_ptr<AtomToolsFramework::DynamicPropertyGroup>> m_groups;
    MaterialPropertyValueMap m_valuesAtOpen;         // IsModified baseline
    MaterialPropertyValueMap m_valuesBeforeEdit;     // undo baseline
    AZStd::any m_invalidValue;
};
```

与上游 `MaterialDocument`（`MaterialDocument.h:26-146`）逐条对应，只是把
`m_materialAsset` / `m_materialInstance` / `m_materialTypeSourceData` / `m_materialSourceData` / `m_editorFunctors`
五个 Atom 成员换成了 `m_source` + `m_handle`。**其余成员语义一致。**
量级参考：原版 `MaterialDocument.cpp` 1065 行，砍掉全部 Atom 资产构建后约 450 行，其中核心转换约 120 行。

### 5.2 `MaterialPropertyDesc → DynamicPropertyConfig` 映射表（这是全部的"转换代码"）

| MaterialPropertyDesc | DynamicPropertyConfig | 备注 |
|---|---|---|
| `m_id` | `m_id` **且** `m_name` | 引擎原生名，原样，不拼接组路径（§2.2） |
| `m_displayName` | `m_displayName` | `NameLabelOverride`（`DynamicProperty.cpp:139`） |
| `m_description` | `m_description` | `DescriptionTextOverride`（`:140`）。追加 "(Script Name = 'id')" 与修改指示器说明，照抄上游 `MaterialDocument.cpp:681-683` |
| 值 map 里的当前值 | `DynamicProperty::SetValue()` | variant → any，一次 `AZStd::visit` |
| `m_defaultValue` | `m_defaultValue` **且** `m_parentValue` | 同一值填两处：`m_parentValue` 驱动修改指示器（上游用父材质值，我们用类型默认值） |
| `m_min/m_max/m_softMin/m_softMax/m_step` | 同名字段（`AZStd::any`） | **double → 值类型的广播**：Float→`any(float)`、Int→`any(AZ::s32)`、Vec2/3/4→`any(float)`（框架对向量按分量套 `ApplyRangeEditDataAttributes<float>`，`DynamicProperty.cpp:162`）。min+max 齐备自动升级为滑杆（`:388-395`）；类型不符会被 `CheckRangeMetaDataValues()`（`:133`）拦掉 |
| `m_suffix` | `m_suffix`（S6 新增） | 单位后缀，slider/spin/int/vector 四族 stock 控件全部现成消费（`PropertyDoubleSliderCtrl.cpp:246` 等）。**照引擎原生单位直出，不跨引擎归一**（`Plan.md` §A6 C4 归一化适用范围） |
| `m_enumValues` + Enum | `m_enumValues` | 值为 u32 索引 → ComboBox（`:170-174`） |
| `m_vectorLabels` | `m_vectorLabels` | 向量轴标签（`:159-163`） |
| `m_fileExtensions` + Texture | `m_customHandler = AZ_CRC_CE("StringFilePath")` + `m_fileExtensions`（S5 新增） | 复用现成控件，见 §5.4 |
| `m_colorSpace`（S2 新增） | `m_colorSpace` | Linear→`GetLinearRgbEditorConfig()`，Srgb→`GetRgbEditorConfig()` |
| `m_visible` / `m_readOnly` | 同名 | 隐藏 / 灰化 |
| — | `m_dataChangeCallback` | 编辑器构造，指向 `ApplyEdit` |
| `MaterialPropertyGroupDesc::m_defaultCollapsed` | （不进 config） | 走 `ShouldGroupAutoExpanded` 虚函数，见 §5.4 |

变体转 `AZStd::any` 用一次 `AZStd::visit`，约 20 行；反向同理。**这就是全部的类型胶水。**

### 5.3 编辑与撤销流程（undo 零新代码）

```
RPE 控件改值
  └─ DynamicProperty::OnDataChanged            (DynamicProperty.cpp:241-248)
       └─ m_dataChangeCallback → CeeMaterialDocument::ApplyEdit
            ├─ any → MaterialPropertyValue
            ├─ m_source->SetPropertyValue(handle, id, value)
            │    ├─ Applied              → OnDocumentObjectInfoChanged(该组, rebuilt=false) → RefreshGroup
            │    ├─ AppliedSchemaChanged → RebuildFromBackend()（schema+值一起重取）→ RebuildGroup
            │    └─ Failed               → 回滚 DynamicProperty 值 + AZ_Warning
            └─ OnDocumentModified

BeginEdit (RPE BeforePropertyModified → AtomToolsDocumentInspector.cpp:93-119 已现成映射)
  └─ m_valuesBeforeEdit = 全量值快照
EndEdit   (RPE SetPropertyEditingComplete)
  └─ diff(m_valuesBeforeEdit, 当前值) → AddUndoRedoHistory(
        undo = [旧值集] { 逐条 SetPropertyValue },
        redo = [新值集] { 逐条 SetPropertyValue })
```

**两条关键点：**
- **`AtomToolsDocumentInspector` 的 `IPropertyEditorNotify` 已经把 RPE 的编辑生命周期自动映射到 `BeginEdit/EndEdit`**
  （`AtomToolsDocumentInspector.cpp:93-119`）→ **拖一次滑条 = 一步 undo，零新代码**，与原版行为一致。
- **EndEdit 要 diff 全量而不只记被拖的那一个**：因为后端可能派生改值（Godot 赋贴图自动 factor=1.0）。
  全量 diff 天然把派生变更收进同一个 undo 条目 —— 这正是上游 `MaterialDocument::EndEdit`
  （`MaterialDocument.cpp:328-355`）的做法，直接照抄语义。

**undo 回放的重建抑制**（编辑器侧实现细节，非契约）：一个 undo 条目恢复 N 个值 = N 次 `SetPropertyValue`，
若其中多次返回 `AppliedSchemaChanged` 就会重建 N 次面板。做法：回放期间置一个抑制位，**只累计"是否需要重建"，
循环结束后统一重建一次**。这与 `IEntityMirror` 的 transform 抑制位同构（`Plan.md` §B4 判例），
**零契约成本** —— 所以契约里不需要批量写接口（§4.4）。

### 5.4 三处控件复用的落地细节（C3 三级优先序）

> C3（2026-08-25 澄清）：① 直接复用 stock ＞ ② 扩展 stock ＞ ③ 自建控件。
> 本节前两项是 ①，第三项是 ②（并按 C3 判定手段落了 (a)(b) 两条证据，见 §9.4）。**全案无 ③ 级**。

**贴图路径编辑**：`m_type == Texture` 的属性设 `m_customHandler = AZ_CRC_CE("StringFilePath")`。
控件 `PropertyStringFilePathCtrl`（`PropertyStringBrowseEditCtrl.h:85-98`，**零 Atom include**）自带
BrowseEdit + 浏览按钮 + 清除按钮，`EditValue()` 直接弹文件对话框并按 `m_extensions` 过滤（`.cpp:259-266`）。
**需要 S5 把 `m_fileExtensions` 转发进去**（§3.4）。
AssetBrowser 拖放赋值复用既有 D&D 链路（`rbfx_migration.md` §3.4），v1.5。

**分组默认折叠**：`InspectorWidget::ShouldGroupAutoExpanded(groupName)` 是**虚函数**（`InspectorWidget.h:86`），
默认实现查持久化的折叠集合（`InspectorWidget.cpp:260-264`）。
在 `CeeMaterialDocumentInspector` 里 override 约 5 行：**用户没有显式记录过该组 → 用 schema 的 `m_defaultCollapsed`；
记录过 → 尊重用户**。**零上游改动、零新控件。**

> 三份方案都写"官方 `InspectorGroupWidget` 原生支持 `m_defaultCollapsed`"——原生支持的是**虚函数钩子**，不是数据字段。
> 落地路径不同，结论相同（可做、免费）。

**组头启用勾选框（S7，②级，P3 排期）**：`m_toggleProperty` 非空的组，把那个 bool 提升到组标题栏，
并隐藏它在属性列表里的普通行。上游加一个 3 行的 `virtual CreateGroupHeader()`，CEE 侧子类 override `paintEvent`
画 `QStyle::PE_IndicatorCheckBox`，点击分发走现成的 `InspectorWidget::OnHeaderClicked`（`InspectorWidget.h:89`）。
完整论证、选项对比与排期见 **§9.4**。

**缩略图**（`DynamicPropertyConfig::m_showThumbnail`）v1 不启用 —— **理由是排期不是规则**：
P0–P4 已 8~9 人周（2026-08-25 opus review 重估），且它不阻塞任何一把验收尺子，先把六把尺子跑通。
（新口径下另有一条更便宜的路：不走 Thumbnailer，在 `PropertyStringFilePathCtrl` 子类里直接 `QPixmap` 读图片文件画小预览——
属 ② 级扩展。留作 v1.5 的备选实现，届时按 C3 判定手段补 (a)(b) 两条记录。）

---

## §6 应用形态与预览

### 6.1 CEE 内的 dock 面板，不是独立 exe（2026-08-25 改判）

产物 = `CrossEngineEditor.exe` 内的一组 dock 面板。**无第二个可执行文件、无 `LaunchTool`、无进程间握手、无 `QFileSystemWatcher` 桥。**

**本文初稿判"独立 exe"，是错的。** 初稿列了 5 条理由，逐条清算：

| 初稿理由 | 复核结论 |
|---|---|
| ① 双 `AzQtApplication` 不可同进程 | **不承重**。§6.3 早已决定不继承 `AtomToolsApplication`；内嵌形态下只有一个 `CrossEngineEditorApplication`，"双 Application"从来不存在。这条是拿一个被自己否决的前提当论据 |
| ② `ISceneRenderer` 单表面 → 内嵌须 RTT 回读，"与 §B2 无 QImage 拷贝冲突" | **事实对，推论错**。见 6.4：RTT 回读正是两个引擎**自己的编辑器**渲染材质预览的方式；§B2 那条反例针对的是**主视口**每帧全屏拷贝，不是按需刷新的预览球 |
| ③ O3DE 是 Editor + MaterialEditor 双进程先例 | **O3DE 是行业孤例，不是常态**。Unreal / Unity / Godot / rbfx / Blender 全部在进程内编辑材质（Unreal/Blender 为 2026-08-25 源码核实补齐）。Godot：`MaterialEditorPlugin : EditorPlugin`，`add_custom_control(editor)` 挂进检视器（`editor/scene/material_editor_plugin.cpp:468-470`，`material_editor_plugin.h:134`）；rbfx：`SystemUI/MaterialInspectorWidget.cpp` 是主窗口内的 ImGui 面板；Unreal：MaterialEditor 是 `FAssetEditorToolkit` 的 **MajorTab 停靠面板**（`MaterialEditor.cpp:726`、`AssetEditorToolkit.cpp:149-165,222`），默认宿主虽是独立 SWindow，但**同进程、可 dock 回主窗口**（`MainFrameModule.cpp:546` "DockedToolkit" 槽）——不是独立 exe；Blender：**没有材质编辑器窗口**——节点编辑器是主窗口内 `SPACE_NODE` SpaceType（`DNA_space_enums.h:1173`、`space_node.cc:1721`），Material Preview 只是 3D 视口的一个着色模式（`DNA_object_enums.h:41`） |
| ④ 崩溃隔离 | **不成立**。后端引擎已在 CEE 进程内运行；材质预览把引擎搞崩，主编辑器一样死 |
| ⑤ 材质编辑器可跑不同 `--backend` | **是反特性**。C6 的切换语义是整个编辑器一次性切换；同进程两个后端反而破坏"值不出引擎"（§2.2）的单一真相源 |

**改判后的正面理由**：

1. **业界一致**（见上表③的源码证据）。材质编辑本质是"改属性 → 看小球"，与场景编辑共享同一份资产浏览器、同一个 undo 栈、同一个后端实例，拆进程是把这些全部切断再用 IPC 缝回来。
2. **dock 是免费的**。CEE 已在用 ADS（`EditorMainWindow.cpp:86`），且 `:410` 已有通用建 dock 助手（Qt dock area → ADS area 映射就位）。新增面板 = 调它。
3. **消掉了整个 §6.2**（见下）：不再需要抽 `CrossEngine::EditorFramework` 静态库。
4. **实时同步天然成立**：同进程、同一个 `IMaterialSource` 实例，材质改了主视口里用到该材质的物体立即变。初稿那条"v1 接受两进程不同步 + v1.5 补 40 行文件监视桥"的限制**整条消失**。

### 6.2 CMake：前置改动已取消（纯减法）

初稿要求先把共享源码抽成 `CrossEngine::EditorFramework` STATIC 库，理由是"CEE 只有一个
`ly_add_target(NAME CrossEngineEditor APPLICATION)`（`Code/CMakeLists.txt:27-28`），加第二个 app 会把共享源码编译两遍"。

**改判为 dock 之后没有第二个 app，这条前置改动整个消失。** 材质编辑器的源码（`MaterialEditor/` 目录）
和各后端材质面 `XxxMaterialSource.cpp` 直接进现有 target，与既有 `XxxBackend.cpp` 并列，
沿用同一套 `CEE_ENABLE_*`/`CEE_HAVE_*` 条件编译与 `SKIP_UNITY_BUILD` 规则。CMake 改动 = **只加源文件**。

### 6.3 文档系统挂进现有 app，且不碰 `AtomToolsApplication`

**不继承 `AtomToolsApplication`** —— 这条初稿结论**继续成立**，而且改判成 dock 后理由更强（连"要一个 Application 基类"这个需求本身都没有了）：

`AtomToolsApplication`（`Application/AtomToolsApplication.h:33-38`）= `AzQtApplication + AzFramework::Application`
+ AssetProcessor 连接 + 关键资产同步编译（`GetCriticalAssetFilters`）+ AssetDatabase sqlite 路径。
这些全部服务于 **O3DE 资产管线**——而 rbfx/Godot 的资产根本不进 AssetProcessor（CEE 的 `IAssetSource`
就是为此存在的，"预烹饪枚举"语义）。强行复用会把死代码和潜在挂起带进来。

我们要的从来只是**文档系统**，而它不依赖 `AtomToolsApplication`：`AtomToolsDocumentSystem`
按 `Crc32 toolId` 挂总线、可独立实例化。所以挂载点是现有的
`CrossEngineEditorApplication::StartCommon`，在后端注册之后追加三行：

```
CreateBackendFromCommandLine(--backend)        // 现有，CrossEngineEditorApplication.cpp:240-266
  → AZ::Interface<IEngineBackend>::Register → backend->Initialize(...)
  ── 以下为新增 ──────────────────────────────
  → new AtomToolsDocumentSystem(k_ceeMaterialToolId)
  → RegisterDocumentType(CeeMaterialDocument::BuildDocumentTypeInfo())   // 扩展名来自 MaterialTypeInfo
```

主窗口侧**不引入** `AtomToolsDocumentMainWindow`（它是个 `DockMainWindow`，而 CEE 已有自己的
`EditorMainWindow` + ADS dock manager，再套一个主窗口是重复造轮子）。改为：
把 `AtomToolsDocumentInspector`（Core，纯 `QWidget`）作为 ADS dock 面板挂进
`EditorMainWindow`，File/Edit 菜单项直接调 `AtomToolsDocumentSystemRequestBus`。
**复用的是真正有价值的那部分（文档/undo/检视器），丢掉的是不适用的那部分（Application、主窗口外壳）。**

**标签页层裁定（R1，2026-08-25 opus review 实锤）**：终稿曾写"文档标签栏 = `DocumentTabWidget` 直用"——该组件**不存在**（全仓零命中）；标签栏是 `AtomToolsDocumentMainWindow` 的私有成员 `AzQtComponents::TabWidget*`（`.h:142`），围绕它的簿记 ~420 行（`.cpp:289-708`），且该窗口构造器无条件建 FancyDocking + WindowDecorationWrapper + **第二个 AssetBrowser** + Python 终端 + log 面板 + metrics 定时器（`AtomToolsMainWindow.cpp:36-37,54-67`）——嵌进 CEE = 两套停靠系统嵌套 + 资产浏览器重影 + 多余 QTimer（撞 §B3）。**v1 形态 = 单文档 dock（用户拍板选项 C）**：面板顶部 `QComboBox` 列出已打开文档 + Save/Close 按钮 + 三个 `OnDocumentOpened/Modified/Closed` 通知处理，约 80~120 行；`AtomToolsDocumentSystem` 的多文档能力原样保留，只是不给 tab UI。**业界一致**：Godot / rbfx / Unity / Blender 的 dock 形态材质编辑没有一家用标签页（rbfx `MaterialInspectorWidget` 跟随选中材质，`materials_.size()==1` 才出预览；Godot 面板结构无任何 tab）——标签页是独立工具窗口的产物。v1.5 若真需要 tab，再自建薄 tab 栏，文档系统那侧一行不改。

**dock 三处主窗口冲突（同批处理）**：① `Ctrl+S` 已被 "Save Level" 占用（`EditorMainWindow.cpp:111-113` 绑 `QKeySequence::Save`）→ 材质保存走**面板局部动作**（`Qt::WidgetWithChildrenShortcut`，§B8 B2 口径）或显式按钮；② **undo 双栈**——`AtomToolsDocument` 自带闭包 undo 栈（`.h:103-120`）vs 场景侧 `AzToolsFramework::ScopedUndoBatch`（`GizmoManager.cpp:427` 等），v1 只在材质面板内提供 Undo/Redo 按钮 + 面板局部快捷键，不碰全局 Ctrl+Z（CEE 目前无全局 Undo 动作，无历史包袱）；③ **退出确认**——`EditorMainWindow::closeEvent`（`:618-634`）目前只问关卡，需先问 `AtomToolsDocumentSystemRequestBus::CloseAllDocuments()`，返回 false 则 `event->ignore()`。

### 6.4 预览：离屏 RTT + 按需回读（= 两个引擎自己的做法）

**预览面板不占用 OS 表面。** 后端把预览场景渲进一张离屏纹理，编辑器按需取回 RGBA8 像素画进 Qt widget。

#### 6.4.1 为什么不是"第二个 OS 表面"（2026-08-25 两引擎核实；2026-08-26 五引擎复核重写横评段）

| | rbfx | Godot |
|---|---|---|
| 第二 swapchain 绑外部 HWND | 设备层**支持**（`RenderDevice::CreateSecondarySwapChain`，`RenderAPI/RenderDevice.h:82-84`），但场景管线**只认主链**：`RenderTargetView.cpp:71,73,119,121,137` 无条件取 `GetSwapChain()`，`RenderBuffer.cpp:185-189,220-232` 的 backbuffer 回退亦然 → 改 4-5 处引擎内部 | 多窗口多 swapchain 架构**完备**（每窗口 surface `rendering_context_driver.h:46`、每窗口 swapchain `rendering_device.h:1310`、帧末批量 present `rendering_device.cpp:8177-8246`），但 `create_sub_window` 的 parent **硬编码 `nullptr`**（`display_server_windows.cpp:1855`），只有主窗口能认 `--wid` 外部 owner → 加公开入口 + 改 `DisplayServerWindows`，~100-200 行 |
| 代价 | **fork rbfx** | **fork Godot**（scons 独立 out-of-tree 构建，补丁更难维护） |
| 上游自己这么做吗 | **没有**。`SceneWidget`（**主**场景视图）与 `MaterialInspectorWidget` 都走 `SceneRendererToTexture`（`SceneWidget.cpp:133-140`、`MaterialInspectorWidget.cpp:199-200`） | **没有**。3D 视口在编辑器中**始终**是 SubViewport 纹理合成进主窗口，**分屏亦然** |

两个引擎都是**外部未 pin 的 checkout**（rbfx：`External/CMakeLists.txt:298` `add_subdirectory(${CEE_RBFX_ROOT})` + `git describe --dirty` provenance；Godot：out-of-tree scons，`External/CMakeLists.txt:334`）。
对未 pin 的上游打私补丁是最难交代的组合，直接顶到 C5（接入成本）与"可插拔后端 = 消费 stock 引擎"。**驳回，记入 §9.9。**

**五引擎横评（2026-08-25 初版；⚠️ 2026-08-26 双线独立复核后重写，全文 `material_editor_research.md`）**：

引擎级多窗口/多 swapchain 能力几乎人人都有——Godot 架构完备（上表）、Unreal 每 `SWindow` 一个 RHI viewport（`SlateRHIRenderer.h:140-141` `WindowToViewportInfo` → `.cpp:315,328` `GetOSWindowHandle()`+`RHICreateViewport`，逐窗口 Draw+Present `.cpp:1486,1500`）、Blender 每窗口独立 GPUContext（`wm_window.cc:1048`）、O3DE Atom 按窗口建（`RenderPipeline::CreateRenderPipelineForWindow`）。

**订正 ①：材质预览不是"5/5 都用 RTT"，而是 4/5——O3DE 恰恰用窗口句柄。**
Unreal 即便浮出独立窗口，预览仍是离屏 RT 合成的 widget（`SViewport.h:37` 默认 `_RenderDirectlyToWindow(false)` → `SceneViewport.cpp:76,2399-2462` 建 "BufferedRT" → `SViewport.cpp:164-169` 当贴图合成）；Blender 连主 3D 视口都走 region FBO → 主窗口合成（`wm_draw.cc:722,838,1148`），材质球更是离屏渲染 + CPU 回读（`render_preview.cc:1281,1289`）；rbfx/Godot 见上表。
但 **O3DE 是 `RenderViewportWidget::winId()` 取的独立 HWND 上的独立 swapchain**（`RenderViewportWidget.cpp:62-66` → `EntityPreviewViewportScene.cpp:33-37,107` `CreateRenderPipelineForWindow` → `WindowContext.cpp:216-232` `RHI::SwapChain`）。本文初版写的"主 swapchain 的 scissor 子区域"**是错的**——`EntityPreviewViewportScene.h:60` 的 `SwapChainPass` 恰恰证明**有**独立交换链。

**订正 ②：另外四家是无效样本，这张表撑不起"全行业零判例"。**
「要不要给引擎窗口句柄」这个选择题，**只在宿主 UI 不是引擎自己画的时候才成立**。rbfx（ImGui）/ Godot（Control 树）/ Unreal（Slate）/ Blender（自家 GPU 模块）的编辑器 UI **本身就跑在引擎渲染管线里，压根没有"外来窗口"可以塞句柄**——它们不是"评估后否决了句柄方案"。唯一与 CEE 同处境（Qt 宿主）的样本是 O3DE，**而它选了句柄**。
⇒ **驳回论据必须靠 §9.9(a) 的引擎能力硬事实，不能靠这张表。**

**订正 ③：多 swapchain 不是"能力闲置"，而是"能力在用、但被一致限定在 UI/窗口合成层"。**
Unreal 浮动 tab 新建 `SWindow`（`FDockingDragOperation.cpp:335-363`）、Godot 浮动 dock 默认开（`editor_settings.cpp:664`）、rbfx ImGui 多视口默认开（`UrhoOptions.cmake:282` + `EditorApplication.cpp:206-208` → 每浮出窗口一条 secondary swapchain `ImGuiDiligentRendererEx.cpp:338`）、O3DE 同窗口 Default+XR 三链并存（`WindowContext.h:131-132`）。**但即便把面板浮出成独立 OS 窗口，里面的预览球依然是一张贴图**——3D 场景渲染一律不借用第二块表面。这比"能力闲置"是更强的论据。

**净结论（未变）：材质预览 0/5 用第二个 OS 表面；多表面路线驳回不变**——但理由见 §9.9(a)。

附带结论：**v2 分屏场景编辑也不需要多表面**——两个引擎原生的分屏就是多张 RTT。v2 真正的缺口是"大尺寸纹理低成本进 Qt"（共享 GPU 纹理），与本节的契约形状一致，不是多 swapchain。
（⚠️ 该缺口的技术形态已澄清：Qt 侧承载体只能是 `QRhiWidget`/Qt Quick，2026-08-26 裁决暂不采用——§6.4.4/§11.8，留到分屏立项再议。）

#### 6.4.2 RTT 是两引擎自家的做法；回读是"宿主与引擎不共享合成层"的通用代价（2026-08-25 口径修正；2026-08-26 松口）

| | rbfx | Godot |
|---|---|---|
| 渲进纹理 | `SceneRendererToTexture`（`Utility/SceneRendererToTexture.cpp:85-113`），`RGBA8_UNORM` + `BindRenderTarget` | `SubViewport` + `Viewport::get_texture()`（`scene/main/viewport.h:573`） |
| **按需渲一帧** | `SURFACE_MANUALUPDATE` + `RenderSurface::QueueUpdate()`（`RenderSurface.cpp:79-82`） | `VIEWPORT_UPDATE_ONCE`，渲完**自动转 DISABLED**（`renderer_viewport.cpp:955-957`） |
| 取回像素 | `Texture2D::GetImage()/GetData()` → `RawTexture::Read`（`RawTexture.cpp:955-1045`，内部自建 staging + 自销毁） | `ViewportTexture::get_image()`（`viewport.cpp:200-206`）→ `RS::texture_2d_get`（`texture_storage.cpp:1888-1898`）→ `RD::texture_get_data`（`rendering_device.cpp:2665-2782`） |
| **停顿点** | `WaitForIdle()`（`RawTexture.cpp:1017`） | `_flush_and_stall_for_all_frames()`（`rendering_device.cpp:2729`） |
| 运行时 resize | `SetTextureSize` + `Update()`（`SceneRendererToTexture.cpp:47-60, 71-82`） | `SubViewport::set_size` |
| **上游同款先例** | 场景页 PNG 预览 `SceneViewTab.cpp:1085-1105`；截图存盘 `SceneScreenshot.cpp:70-152` | **材质预览缩略图** `EditorMaterialPreviewPlugin::generate`（`editor/inspector/editor_preview_plugins.cpp:339-361`）：设材质 → 等一帧 → `texture_2d_get` → `convert(FORMAT_RGBA8)` |
| 多实例同帧 | 上游自证：主场景视图 + 材质预览并存 | SubViewport 本就多实例 |
| 像素格式 | `RGBA8_UNORM` | `Image::FORMAT_RGBA8` |
| 线程纪律 | — | `ERR_RENDER_THREAD_GUARD_V`（`rendering_device.cpp:2666`）：须在驱动 RenderingServer 的线程调用。CEE 单主循环天然满足 |

两个引擎的停顿形状**完全同构**（全管线 flush + stall；rbfx 源码自己的注释写着 *"This operation is very slow and shouldn't be used in real time."*，`RawTexture.h:157-158`）。
所以**"按需重渲、绝不每帧回读"是跨引擎不变量**，写进契约语义，不留给后端各自发挥。
而两个引擎都把"按需渲一帧"做成了一等公民（上表第 2 行），契约可直落。

**口径修正（R5，2026-08-25 opus review）**：RTT 是两引擎自家预览的做法；**回读不是**——rbfx 自家拿到 `Texture2D*` 后直接 `Widgets::ImageItem(sceneTexture, …)` 喂给 ImGui（`MaterialInspectorWidget.cpp:409-411`，**零 CPU 回读**），Godot 是 `SubViewportContainer` 引擎自合成（`material_editor_plugin.cpp:269-276,308-318`）。上表引用的两处上游"回读判例"（场景 PNG 预览、材质缩略图）都是**一次性、非交互**的。

**⚠️ 口径松口（2026-08-26 复核）**：R5 原文写"回读是 **Qt 宿主强加的 CEE 独有成本**"——**过重**。
**Blender 的材质预览球就是 RTT + CPU 回读**：`shader_preview_render` 完整离屏渲染（`render_preview.cc:1204,1281`）
→ `RE_ResultGet32` 回读进 `PreviewImage` 的 CPU uchar 数组（`:1289,1533`）
→ 上屏时**每次绘制**新建 GPU 纹理上传再销毁（`glutil.cc:68-118`）。它是出货多年的 DCC，且它自绘 UI、根本没有 Qt。
另外 **O3DE 的缩略图同样是 RTT + `AttachmentReadback` → `QImage`**（§6.4.4 末）。
⇒ 准确表述是：**回读是"宿主与引擎渲染器不共享合成层"时的通用代价，不是 Qt 造成的畸形**；
CEE 的 `RTT → 回读 → QImage` 是 DCC 常规做法，缩略图更是 5/5 全行业同款。
这反而让 §6.4.4 的限频显得**保守而非冒险**——但限频与本节"绝不空闲回读"**仍必须同时成立**，
因为 rbfx 那条同步路径每次回读都是全管线停顿（`RawTexture.h:157` 自注 "very slow, shouldn't be used in real time"），
缺一条就退化成每帧停顿。Blender 的缓解手段是异步 job（`WM_jobs_*`），CEE 的对应物是 Godot `texture_get_data_async`（§6.4.3）——
rbfx 侧的同款是 Diligent fence 异步回读（§6.4.4 升级路径，v1.5 候选）。

#### 6.4.3 契约新增：一个哨兵方法

`PreviewResult { Unsupported, Unchanged, Updated }` 与哨兵 `AcquirePreviewImage(width, height, outPixels)` 的完整定义见 §4.1 头文件草案，语义要点：

- 后端内部维护 dirty 位：`SetPropertyValue` / `SetPreviewMaterial` / `SetPreviewModel` 会置脏并按需渲一帧；
  稳态下返回 `Unchanged`，**不触发任何回读与停顿**。
- `IMaterialSource` 由「8 纯虚 + 1 哨兵」变为「**8 纯虚 + 2 哨兵**」。
  **`ISceneRenderer`、`IViewportTick`、主循环、四个后端的表面生命周期：一行不动。**

**Godot 异步红利（R5）**：`RenderingDevice::texture_get_data_async(rid, layer, callable)`（`rendering_device.h:470`）与三态天然吻合——发起回读 → 后续帧返回 `Unchanged` → 回调到达后返回 `Updated`。v1 Godot 直接用异步（**零停顿**），rbfx 用同步 + 限频（§6.4.4）。这是三态哨兵设计的真正红利。

#### 6.4.4 刷新驱动：不新增第二个时钟（Plan §B3）

预览是**事件驱动**的，不轮询：编辑器侧的 `SetPropertyValue` / `SetPreviewMaterial` / `SetPreviewModel`
是仅有的三个改变预览内容的入口，面板在这三处置脏。**置脏后**才在现有 60fps 节流门内
调一次 `AcquirePreviewImage`：后端已渲好则返回 `Updated`（清脏），未渲好返回 `Unchanged`（下一帧再试）。
**调用点必须在 `backend->Tick()` 之后**（`CrossEngineEditorApplication.cpp:356-371` 的门内尾部）——门内顺序是 `TickRender` → `backend->Tick`，Godot 的预览帧在 `Tick()` 里出，放在 `TickRender` 后恒定晚一帧。

**交互限频（R5，2026-08-25）**：置脏只挂 60fps 门不够——拖滑杆时每帧都在 `SetPropertyValue`，不加限频就是**每帧一次全管线停顿**（`RawTexture.h:157` 自注 "very slow, shouldn't be used in real time"）。三条硬规则：
1. **`PreviewRefreshIntervalMs` = 66~100 ms（10~15 Hz）**：脏位 + 上次取回时间戳，在现有 60fps 门内"到点才取"。**不新增 QTimer**（§B3 不破）。材质预览球是"看趋势"，10~15 Hz 肉眼无感，每帧回读是纯浪费。
2. **回读尺寸 clamp ≤ 512²（物理像素）**，面板放大用 `QPainter` 拉伸——停顿与拷贝量都随面积走。
3. **`isVisible()` 门一行**：面板不可见（ADS auto-hide / 非激活文档）不置脏。

**rbfx 后端升级路径：fence 异步回读（2026-08-26 裁决，v1.5 候选，零契约改动）**。
v1 的 rbfx 是「同步 `GetImage()` + 限频兜底」——每次取回都吃一次 `WaitForIdle()`（`RawTexture.cpp:1017`）**全管线停顿**。
升级手段在 CEE 侧、不动引擎：绕过 `RawTexture::Read` 的同步路径，直接用 Diligent `IDeviceContext::ReadTexture` + fence——
**渲染一帧后发起带 fence 的 staging 拷贝，到下一轮限频窗口（66~100 ms 后）再 `Map`**，届时 fence 早已 signal，
停顿退化为「延迟一帧取回」（交互期额外 ≤2 帧延迟，10~15 Hz 限频下肉眼无感）。
落点不变：仍只在脏位 + 限频 + `isVisible()` 三重门后发起，稳态依旧零调用。
**契约形状零变化**（`AcquirePreviewImage` 实现内部换路，签名/三态/频率全不动），
升级后 rbfx 与 Godot（`texture_get_data_async`）**同为零停顿**，R5 的限频从"兜底"降为纯节流。
> 2026-08-26 用户裁决：**「QRhiWidget + D3D11 shared handle」零拷贝路线暂不采用**（需动 Plan §B2/§B3、引入额外技术债）；
> 非 QRhiWidget 下本段 fence 方案 + §6.4.2 的零拷贝 `QImage` 构造已是当前技术栈内的性能上界。§11.8 有裁决记录。

**稳态下零调用、零回读、零停顿。** 不新增 `QTimer` —— Plan §B3 的 233 ms 多渲染尖峰正是第二个渲染 QTimer 造成的，这条铁律不破。

Qt 侧：面板持有像素 `AZStd::vector<AZ::u8>` 成员（`QImage` 包外部缓冲不拷贝，须保证生命周期），
`QImage(m_pixels.data(), w, h, QImage::Format_RGBA8888)` → `QPainter::drawImage`。**零格式转换。**

**同宿主判例（2026-08-26 复核补录）**：本节的控制流在 **O3DE 自家就有现成实现**——`AtomToolsFramework::PreviewRenderer`
（缩略图路径）：`CreateRenderPipeline`（**无窗口句柄**，`PreviewRenderer.cpp:80`）
→ `AddToRenderTickOnce()`（**按需渲一帧**，`:249`）
→ `CapturePassAttachmentWithCallback(..., "Output", ...)`（`:252-257`）
→ 回调里 `QPixmap::fromImage(QImage(result.m_dataBuffer->data(), w, h, QImage::Format_RGBA8888))`（`:222-232`）
→ `RemoveFromRenderTick()`（**渲完退出 tick**，`:271`）。
与 `AcquirePreviewImage` + `PreviewResult{Unsupported,Unchanged,Updated}` 形状同宗，**且出自同一个 Qt 宿主**——
可作契约注释里的设计出处。（`PreviewRenderer` 本身深绑 RPI，§9.8 判定不引入，此处只借形状。）
另：Godot 缩略图的"等帧回读"也是同一形状——`frame_pre_draw` 单发连接 + `VIEWPORT_UPDATE_ONCE`
+ `request_frame_drawn_callback` + 信号量（`editor_resource_preview.cpp:101-118,131-134`），至少等 1~2 帧再取。

#### 6.4.5 预览场景内容：后端自管，惰性构建

后端在首次 `SetPreviewMaterial` 时**惰性建预览场景**（一个网格 + 一盏灯 + 环境 + 相机），
**不需要给 `IEngineBackend::Initialize` 加任何新参数**。每个引擎的配方都在它自己的编辑器里现成存在：

| 后端 | 现成配方 | 证据 |
|---|---|---|
| rbfx | `Scene` + `Octree` + `Zone` + `Skybox` + 平行光 + `StaticModel(Sphere)` → `SceneRendererToTexture` | `SystemUI/MaterialInspectorWidget.cpp:199-215, 388-466` |
| Godot | `SubViewport`(自有 `World3D`、透明背景、`MSAA_4X`) + `Camera3D`(0,0,1.1) **FOV 20°**(低 FOV 防畸变) + 双 `DirectionalLight3D` + sphere/box/quad `MeshInstance3D` 挂在一个 `rotation` 节点下 | `editor/scene/material_editor_plugin.cpp:308-316`(视口)、`321-331`(相机)、`333-340`(灯)、`342-363`(网格) |
| Filament | `filament::Renderer` + 程序生成球 + 方向光 + IBL → 离屏 target | `samples/` 标配 |

> ⚠️ **落地风险（2026-08-26 复核新增，P2 必读）**：上表 rbfx 那一格引用的 `SystemUI/MaterialInspectorWidget.cpp`
> **不在 CEE 的构建里**——rbfx `Source/Urho3D/CMakeLists.txt` 中 `SystemUI` 在 `if (URHO3D_SYSTEMUI)` 门控内，
> 而 CEE `External/CMakeLists.txt:264` 明确 `set(URHO3D_SYSTEMUI OFF CACHE BOOL "" FORCE)  # drops bundled ImGui/ImGuizmo`。
> ⇒ **它只能当参考配方"手抄"，不能 `#include`、不能调用。**
> 好消息是真正要用的 `SceneRendererToTexture` 在 `Utility/` 目录，处于**无条件编译**列表内（同文件 `define_engine_source_files(... Utility)`），**可用** ✅。
> 附带收益：rbfx 侧的 ImGui 第二 swapchain 代码（`ImGuiDiligentRendererEx.cpp`，`CreateSecondarySwapChain` 的全仓唯一调用者）
> 也随 `SystemUI` 一起被编译掉——§9.9 的第三层阻断在 CEE 里是**编译期**的。
> 同理，§7.2 rbfx 落地行、§6.1 与 §11 中所有以 `MaterialInspectorWidget` 为"上游判例"的引用，
> 都只作**行为参照**，不构成可复用代码。

Godot 的预览网格集合 **sphere / box / quad**（`material_editor_plugin.cpp:342-363`）与契约的
`PreviewModel { Sphere, Cube, Plane }` 逐项对应——该哨兵的取值不是拍脑袋定的。
Blender 的缩略图预览场景同样内置 **Sphere / Cube / Shader Ball**（`render_preview.cc:238-266`）——三引擎同款取值。

预览相机 v1 **不做交互**（不 orbit、不 dolly）：固定机位，`rotation` 节点缓慢自转由后端自管。
这样"预览内容何时改变"完全由那三个入口决定，dirty 语义封闭。交互式转视角排 v1.5（届时多一个置脏入口，契约形状不变）。

视口设置沿用官方 `SettingsDialog` 直绑 SettingsRegistry（Atom-free）+ `QToolBar`/`QComboBox`，
**不复刻** Atom 的 LightingPreset/ModelPreset 体系。overlay / gizmo / 拾取全链**不接**（材质编辑器无 gizmo）。

### 6.5 面板清单（全部复用，零自建）

| 面板 | 来源 | 状态 |
|---|---|---|
| 文档切换条 | **自建薄壳（R1-C，~80~120 行）**：`QComboBox`（已打开文档）+ Save/Close 按钮 + 3 个 `OnDocumentOpened/Modified/Closed` 通知处理 | ③ 级。**`DocumentTabWidget` 不存在**（2026-08-25 实锤，§6.3）——标签簿记 ~420 行是 `AtomToolsDocumentMainWindow` 私有成员，不可复用；dock 形态业界零标签页判例 |
| 材质属性检视器 | `AtomToolsDocumentInspector`（Core） | 直用 + 5 行 override（§5.4），同上挂 dock |
| 资产浏览器 | CEE `CeeAssetBrowserPanel` + `IAssetSource`（过滤 = 材质/纹理扩展名） | 直用（已 dock） |
| **材质预览** | **新建 `CeeMaterialPreviewPanel`**：一个 `QWidget` + 6.4 的按需回读 + **限频（10~15 Hz）与 ≤512² clamp**（§6.4.4）。**不是** `EngineViewport`/`ISceneRenderer` 路径 | 唯一新面板，规模 ~150 行，挂在检视器同 dock 区 |
| 日志 | `AzToolsFramework::LogPanel::StyledTracePrintFLogPanel` | 直用 |
| 设置对话框 | `AtomToolsFramework::SettingsDialog`（Core） | 直用 |
| 预览模型切换 | 预览面板工具栏一个 `QComboBox` → `SetPreviewModel` 哨兵 | v1 |

dock 注册复用 `EditorMainWindow.cpp:410` 现成的通用建 dock 助手（Qt dock area → ADS area 映射已就位），
面板内自由摆放由 ADS 的 saveState/restoreState（`EditorMainWindow.cpp:82`）白送。

---

## §7 各后端落地

### 7.1 schema 的两种来源，以及为什么 rbfx/Filament 用 JSON **数据**而不是 C++ 表

| 派别 | 引擎 | 做法 |
|---|---|---|
| **反射免费派** | Godot、（部分）Filament | 运行时从引擎反射直接生成 `MaterialTypeDesc`，零手写 |
| **策展派** | rbfx、Filament 的 UI 元数据 | CEE 侧一份 JSON **数据文件**，`AZ::JsonSerialization` 直接反序列化成 `MaterialTypeDesc` |

**JSON 而不是 C++ 硬编码表的三条理由**：
① `MaterialTypeDesc` 里没有值（值走 `MaterialPropertyValueMap`），所以它就是一个纯 schema 结构，天生可序列化；
② 加一个 shader 参数不需要重新编译编辑器 —— 这正是 Atom `.materialtype` 数据驱动的价值；
③ 让 "C5 接入成本 = 8 纯虚 + 1 份 schema" 这句话成立。

落点：`Gems/CrossEngineEditor/Assets/MaterialTypes/rbfx/*.json`，读写与校验放
`Source/MaterialEditor/MaterialSchema.{h,cpp}`。
配套一个**极小的校验器**（~60 行，仅开发期）：空 `m_description` 告警、重复 `m_id` 告警、L0 语义槽覆盖度告警（§8.1）。
**它不是契约的一部分，不在运行时路径上。**

### 7.2 rbfx（v1 首实现，主后端）

rbfx 是最好的第一后端：材质模型最简单，且 `RbfxBackend` 已有 `ReadResourceProperties`/`WriteResourceProperties`/`SaveResource`
的真实现基础（`rbfx_migration.md` §3）——**这些实现是 `IMaterialSource` 的移植底料，P2 同批退役旧路径（R4，§7.6）**。

- **schema 来源**：rbfx 的 `Material` **没有属性反射**（`Material.h:130` 无任何 `URHO3D_ATTRIBUTE`，
  `RegisterObject` 只注册工厂），但它自己的编辑器把表手写好了。把三样东西写成 JSON：
  - `MaterialInspectorWidget.cpp:111-193` 的 10 条渲染态（Cull / Shadow Cull / Fill / AlphaToCoverage /
    LineAntiAlias / RenderOrder / Occlusion / ConstantBias / SlopeScaledBias / NormalOffset）；
  - `:85-90` 的 5 个标准纹理槽（Albedo / Normal / **Properties** / Emission / Reflection0）；
  - `RenderPipeline/ShaderConsts.h:81-95` 的 12 个标准 PBR 参数（注释原文 "Serialized in Material, do not rename"）：
    `MatDiffColor / MatEmissiveColor / MatSpecColor / MatEnvMapColor / Roughness / Metallic /
    DielectricReflectance / NormalScale / UOffset / VOffset / FadeOffsetScale / LMOffset`。
- **"Custom Parameters" 兜底组**：`Material::GetShaderParameters()`（`Material.h:352`）返回的、schema 里没有的自由参数，
  按 `Variant` 类型追加到一个兜底组 —— 对齐 rbfx 官方检查器的 Shader Parameters 区，保证新增 uniform 不会静默消失。
- **应用**：`SetShaderParameter(name, Variant, isCustom)` / `SetTexture(string_view, Texture*)`。
  ⚠️ **`isCustom_` 必须保留** —— `rbfx_migration.md` §3.2 已踩过这个坑。
  **不需要任何刷新调用**：`PipelineStateTracker` 的 hash 变化让 `BatchStateCache`（`BatchStateCache.cpp:63-88`）
  下一帧自动重建 pipeline state。返回 `Applied`。
- **`AppliedSchemaChanged` 触发点**：改 technique 或 vs/ps defines（`CloneWithDefines`，`Material.cpp:1100-1113`）。
- **`m_colorSpace`**：`MatDiffColor` / `MatEmissiveColor` 填 `Srgb`（gamma 存，`GLTFImporter.cpp:2522` LinearToGamma），
  其余 `Linear`。
- **保存**：`Material::Save(XMLElement)`（`Material.cpp:473-582`），原生 XML，全程不碰 GPU。
- **预览（RTT + 按需回读，§6.4.2）**：`SceneRendererToTexture`（RGBA8 + BindRenderTarget）+ `SURFACE_MANUALUPDATE`；
  置脏后下一帧 `QueueUpdate()`，渲完 `GetImage()` 回读。零引擎改动，与 rbfx 自家 `MaterialInspectorWidget` 同构。
- **两个坑**：
  ① 贴图路径是**相对资源根目录的资源名**（`CoreData;Cache;Data`，`Engine/Engine.cpp:1329`），不是相对材质文件 ——
     后端负责绝对路径 ↔ 资源名转换；
  ② **贴图必须来自文件**：无名内存贴图序列化为空，所以拾取器只给文件路径。

### 7.3 Godot（v1.5）

- **schema 来源：运行时零手写。** `Object::get_property_list()`（`core/object/object.cpp:472-535`）一把梭，
  每条 `PropertyInfo{type,name,hint,hint_string,usage}` 直译：
  - `PROPERTY_HINT_RANGE` 的 `"min,max,step[,or_greater][,suffix:m]"`（格式定义 `core/object/property_info.h:41`）
    → 有 `or_greater`/`or_less` 填 `m_softMin/m_softMax`，否则填 `m_min/m_max`；尾部 `suffix:` 直接填 `m_suffix`（S6）。
    **软硬范围语义天然对齐 Blender 双轨，单位串也是现成的。**
  - `PROPERTY_HINT_ENUM` + 逗号分隔 hint_string → `m_enumValues`。
  - `PROPERTY_HINT_RESOURCE_TYPE == "Texture2D"` → `m_type = Texture`（值走 `res://` 路径）。
  - **丢弃规则（只写接受不写丢弃 = 返工）**：`Variant::OBJECT` 只接受 `RESOURCE_TYPE == "Texture2D"`，其余（如 `next_pass` 的 Material、`RID` 等无法表达项）**整条丢弃**。丢弃 ≠ 丢数据——§2.2 的结构性无损保证它们照样被 `ResourceSaver` 原样写回。
  - `PROPERTY_USAGE_GROUP` / `SUBGROUP`（hint_string 是**属性名前缀**，`class_db.cpp:1384-1408`）
    → 直接生成两级 `MaterialPropertyGroupDesc`。**Godot 的分组模型和我们的一模一样。**
  - `PROPERTY_USAGE_NONE` → `m_visible=false`；`PROPERTY_USAGE_NO_EDITOR` → `m_readOnly=true`。
  - `PROPERTY_HINT_GROUP_ENABLE`（`property_info.h:82`，`BaseMaterial3D` 用了 **15 次**）
    → 填 `MaterialPropertyGroupDesc::m_toggleProperty`，编辑器把它提升到组标题栏（S7，§9.4）。**零额外代价**。
- **动态可见性白送**：`get_property_list` 内部已过 `validate_property`（`object.cpp:537-564`），
  重新调一次就自动拿到新的可见性 —— 完美契合 `AppliedSchemaChanged` + `GetMaterialState`。
- **应用**：ClassDB `set(name, Variant)`。数值属性只重传 UBO（`material_storage.cpp:1178-1181`）；
  凡是会改 `MaterialKey` 的（feature / flag / mode 类）返回 `AppliedSchemaChanged`。
  **风险已被现有代码证伪**：CEE Godot 后端已在节点属性镜像里用同一套 `get/set` 机制（Plan §B4）。
- **保存**：`ResourceSaver::save()`，`.tres` 文本或 `.material` 二进制（`RES_BASE_EXTENSION("material")`，`material.h:40`）。
  **只写与默认值不同的属性**（`resource_format_text.cpp:1986-1990`），与 `m_defaultValue` 语义天然一致。
- **纹理 `.import` 缓存（A 稿 R10 风险，缓解已在 `godot_migration.md` G6）**：Godot 源资源依赖
  `EditorFileSystem` 导入缓存；首次打开含纹理的 `.tres` 前先跑 `godot --headless --editor --quit-after 1`
  生成缓存，不阻塞 UI。
- **预览（SubViewport + 按需回读，§6.4.2）**：自有 `World3D` 的 SubViewport + `VIEWPORT_UPDATE_ONCE`
  （渲完自动转 DISABLED，`renderer_viewport.cpp:955-957`）→ `ViewportTexture::get_image()` → `convert(FORMAT_RGBA8)`。
  与 `EditorMaterialPreviewPlugin::generate`（`editor_preview_plugins.cpp:339-361`）同构；须在驱动 RenderingServer
  的线程调用（`ERR_RENDER_THREAD_GUARD_V`，`rendering_device.cpp:2666`）——CEE 单主循环天然满足。
  **v1 走异步 `texture_get_data_async`（`rendering_device.h:470`）**：零停顿，与 `PreviewResult` 三态吻合（§6.4.3）。
- **`ShaderMaterial` 也免费支持**（v2）：其 `_get_property_list`（`material.cpp:247-357`）把 shader uniform 反射成
  `shader_parameter/xxx` 属性，走同一条路。

### 7.4 Filament（v2 PoC，用于契约证伪）

Filament 目前**不是 CEE 后端**（`Backends/` 下只有 Null/Diligent/Rbfx/Godot），接入它 = 新写 `ISceneRenderer` +
窗口/Swapchain + 构建集成，工作量主体不在材质。但调研确认契约对它成立：

- **不重编译即可改 PBR 参数：成立。** `libs/gltfio` 的 **ubershader** 预编译 19 个变体（`libs/gltfio/CMakeLists.txt`
  的 `build_ubershader()`），运行时 `UbershaderProvider` 查表取 Material + `MaterialInstance::setParameter` 设全部参数，零编译。
  **v1 策略：只预编译 3 blending × 2 doubleSided 的子集，产物提交仓库，用户不跑 matc。**
- **参数全集**：`libs/gltfio/materials/base.mat.in:1-178`（`baseColorFactor/Map/Index/UvMatrix`、`metallicFactor`、
  `roughnessFactor`、`normalScale`、`aoStrength`、`emissiveFactor/Strength`、`clearCoat*`、`reflectance`…）。
- ⚠️ **Filament 的反射只给 name / type / precision，没有任何 UI 元数据**（`MaterialParser.cpp:116-156`）。
  → schema 必须是 CEE 侧手写的 JSON sidecar，`Material::getParameters()`（`Material.h:97-116`）用作**交叉校验**
  （名字/类型对不上就告警），防 schema 与 `.filamat` 漂移。
- **保存**：Filament 没有"材质实例文件"格式。后端自定义一个参数值 JSON（引用 `.filamat`），
  gltfio 的 extras 是官方先例。**这是 Filament 唯一的真实缺口。**
- ⚠️ `.filamat` 有 `MATERIAL_VERSION` 版本锁 → matc 必须与引擎库同源构建，写进接入 checklist。
- 运行时编译 `.mat` 也可行（`MaterialBuilder::init()` 是公开 API，`JitShaderProvider.cpp:93-97` 是官方用法），
  但那是 MaterialCanvas 级别的能力，不在本方案。

### 7.5 Null / Diligent

`NullMaterialSource`：内置 "Demo PBR" schema（覆盖 8 个 L0 槽）+ 内存 map + JSON 落盘。
**C2 可移除性验收**：NullBackend 单后端构建 + 新建/打开/改属性/undo×10/保存全通。
Diligent 不实现材质面（无引擎场景语义，维持现状不扩张），委托 `NullMaterialSource`。

### 7.6 接入成本（C5 口径，新增一行到 Plan §A6 C5）

| 后端 | schema 来源 | 预估 | stub 率 |
|---|---|---|---|
| **Null**（P1） | 内置 Demo PBR | ~2 天 | 0（no-op ≠ stub） |
| **rbfx**（P2） | 策展 JSON（~80 行数据） | ~1.5 周 | 0 |
| **Godot**（P3） | 运行时反射，零手写 | **~1 周（最低）** | 0 |
| **Filament**（P5，P3 后触发） | JSON sidecar + `getParameters()` 交叉校验 | PoC（P3 后评估） | 0 |
| **Diligent** | 不实现 | 0 | — |

**接一个新引擎的材质面 = `IMaterialSource` 8 纯虚 + 1 份 schema。**
对比场景面 19 方法（Godot stub 10/15，`godot_migration.md` §3），材质面约为其一半且**零空壳** ——
与用户"材质编辑器相对简单"的判断一致。

**退役冲抵（R4，2026-08-25 opus review 实锤）**：新契约不是"净 +8"——`IEntityMirror` 的
`ReadResourceProperties`/`WriteResourceProperties`/`SaveResource` 三个方法随 P2 **同批删除**（材质是那条通用资源路径**唯一的真实消费者**，rbfx 自注 `RbfxBackend.cpp:1350-1353`："rbfx Resource is NOT Serializable … the one live editable surface is Material's shader parameters"；Godot 三个方法全 stub）。净收益：`IEntityMirror` **15 → 12** 纯虚，Godot stub **10/15 → 7/12**，直接兑现 `rbfx_migration.md` §4 E2 的契约瘦身目标。`ResourcePropertiesPanel`（无 undo，`ResourcePropertiesPanel.cpp:95` 自认）与 `CeeAssetBrowserPanel.cpp:314` 的材质入口一并退役，改走 `OpenDocument`；Animation 入口（`:322`）同一 PR 评估——若无真实编辑面一并退役，否则面板只留非材质类型。C5 成本表口径：材质面 = **净 −3 方法**。

---

## §8 跨引擎 PBR 基准（设计时策展模板，**不进契约、不进运行时**）

这张表**不进任何头文件**。用途有二：① 后端作者照它策展自己引擎的 schema，使同一个编辑器在不同后端下看起来一致；
② `MaterialSchema` 校验器的 L0 覆盖度断言 —— 这是"跨引擎"从愿景变**可检查断言**的唯一手段。
后端完全可以偏离（rbfx 的 `Properties` 打包贴图、Godot 的 `metallic_texture_channel` 都是引擎特色，照实暴露即可）。

### 8.1 语义槽分级

- **L0 核心（8 槽，所有后端必须覆盖，校验器断言）**：
  baseColor / metallic / roughness / normal / occlusion / emissive(+strength) / alphaMode+cutoff / doubleSided。
- **L1 常用扩展（引擎有则必须暴露）**：specular-ior / clearCoat / anisotropy / parallax / uvTransform / vertexColor / unlit。
- **L2 小众（引擎有才暴露，策展时注明语义）**：sheen / transmission / volume / subsurface / dispersion。

### 8.2 L0 五引擎对照表（列 = 引擎原生属性名，即 schema 里的 `m_id`）

| 语义槽 | Atom StandardPBR | rbfx | Godot BaseMaterial3D | Filament (gltfio) | Blender→glTF |
|---|---|---|---|---|---|
| baseColor | `baseColor.color` / `.textureMap` | `MatDiffColor`（**gamma 存**）/ 槽 `Albedo` | `albedo_color` / `albedo_texture` | `baseColorFactor` / `baseColorMap` | Base Color |
| metallic | `metallic.factor` / `.textureMap` | `Metallic` / 槽 `Properties`.**G** | `metallic` / `metallic_texture` / `_channel` | `metallicFactor` / `metallicRoughnessMap`.B | Metallic |
| roughness | `roughness.factor` / `.textureMap` | `Roughness` / 槽 `Properties`.**R** | `roughness` / `roughness_texture` / `_channel` | `roughnessFactor` / `.G` | Roughness |
| normal | `normal.textureMap` / `.factor` | 槽 `Normal` + `NORMALMAP` define / `NormalScale` | `normal_enabled` / `normal_texture` / `normal_scale` | `normalMap` / `normalScale` | Normal(+Map Strength) |
| occlusion | `occlusion.diffuseTextureMap` / `.diffuseFactor` | 槽 `Properties`.**A**（无独立 AO 槽） | `ao_texture` / `_channel` / `ao_light_affect` | `occlusionMap` / `aoStrength` | glTF 输出节点组 |
| emissive | `emissive.enable/.color/.intensity/.textureMap` | `MatEmissiveColor`（**gamma 存**）/ 槽 `Emission` | `emission_enabled` / `emission` / `emission_energy_multiplier` / `emission_texture` | `emissiveFactor` / `emissiveStrength`（nits）/ `emissiveMap` | Emission Color × Strength |
| alphaMode+cutoff | `opacity.mode` / `.factor`（ShaderOption） | **技法维度**：`LitOpaque` / `LitAlphaMask`+`AlphaCutoff` / `LitTransparent` | `transparency` / `alpha_scissor_threshold`（属性） | **类型维度**：编译期 `blending` + `maskThreshold` | 节点图反推 |
| doubleSided | `general.doubleSided` | `<cull value="none">` | `cull_mode = Disabled` | `doubleSided` | 背面剔除取反 |

### 8.3 L1/L2 覆盖度（差异要点，策展必读）

| 特性 | Atom | rbfx | Godot | Filament | Blender/glTF |
|---|---|---|---|---|---|
| specular / IOR | `specularF0.factor`（**0.5**） | `MatSpecColor` + `DielectricReflectance`（**0.5**） | `metallic_specular`（**0.5**） | `reflectance`（**0.5**）+ `specularFactor` + `ior` | Specular IOR Level（**0.5**，导 glTF **×2**） |
| clearCoat | ✅ 完整（factor/roughness/normal + 3 贴图） | ❌ | ✅ `clearcoat*` | ✅ `clearCoat*` | ✅ Coat → KHR_clearcoat |
| anisotropy | ❌ | ❌ | ✅ `anisotropy` [-1,1] + flowmap | ✅ `anisotropy` + `anisotropyDirection` | ✅ → KHR_anisotropy |
| sheen | ❌ | ❌ | ❌ | ✅ `sheenColor` / `sheenRoughness` | ✅ → KHR_sheen |
| transmission / volume | ❌ | ❌ | `refraction_*`（屏幕空间近似） | ✅ `transmission` / `ior` / `thickness` | ✅ → KHR_transmission/ior/volume |
| parallax | ✅（5 算法 × 4 档质量） | ❌ | ✅ `heightmap_*` + deep parallax | ❌ | ❌ |
| subsurface | ✅ functor | ❌ | ✅ `subsurf_scatter_*` | ✅ SUBSURFACE 模型 | ✅（glTF 不导出） |
| uvTransform | `uv.center/.tileU/V/.offsetU/V/.rotateDegrees` | `UOffset` / `VOffset`（无旋转） | `uv1_scale` / `uv1_offset` / `uv1_triplanar` | `*UvMatrix`（mat3） | Mapping → KHR_texture_transform |
| vertexColor | `vertexColor.enable/.factor/.blendMode` | ❌ | `vertex_color_use_as_albedo` / `_is_srgb` | `vertexColor` | 顶点色 → COLOR_0 |
| unlit | `lightingModel`（类型维度） | Unlit 技法（类型维度） | `shading_mode=Unshaded`（属性） | UNLIT 模型（类型维度） | Emission-only 反推 |

> **`specular` 一行的五个 0.5 是本方案最硬的一条收敛实证**（Atom `SpecularPropertyGroup.json:11`、
> rbfx `Material.cpp:1007`、Godot `material.cpp:3913`、Blender `node_shader_bsdf_principled.cc:167-211`、Filament）。
> **注意 Blender/glTF 的 ×2**：`Specular IOR Level 0.5` ↔ `KHR_materials_specular.specularFactor 1.0`
> （`io_scene_gltf2/.../specular.py:31-60`，`node_shader_bsdf_principled.cc:757` 的 OpenPBR 导出同样 ×2 互证）。

### 8.4 三类"小异"及其归位（无需翻译层）

| # | 差异 | 归位 |
|---|---|---|
| ① | **模式类参数（alpha/unlit/doubleSided）维度归属不同**：rbfx 切技法、Filament 编译期烘焙 = 类型维度；Godot/Atom = 普通属性 | **契约不规定维度，schema 说在哪就在哪**：属性维度 → schema 里一个 Enum 属性 + `AppliedSchemaChanged`；类型维度 → `EnumerateMaterialTypes` 里多个类型 + `CreateMaterial(typeId)`。**两个已有机制刚好用满，零新增概念** |
| ② | **贴图打包不同**：glTF MR（G=rough,B=metal）/ rbfx `Properties`（R=rough,G=metal,A=occl）/ Godot ORM（R=AO,G=rough,B=metal） | 编辑器每语义槽独立一行，打包语义写进 `m_description`；rbfx v1 强制三槽同图 + 异图告警，**不静默重打包**（算法 `GLTFImporter.cpp:2271-2332` 留 v2） |
| ③ | **单位与色彩空间不同**：rbfx gamma 存色 / Filament emissive 用 nits | 色彩空间由 `m_colorSpace` 逐属性声明（§4.1）；单位**不做跨引擎归一**，照引擎原生语义显示与编辑 —— 与原生编辑器一致才是对的 |

### 8.5 分组与元数据策展规则（后端实现者必读）

1. **分组骨架照 L0 → L1 → L2 顺序**；分组名用引擎原生词汇（Godot 直接用 `ADD_GROUP` 原样，rbfx 用官方检查器分区）；
   进阶组 `m_defaultCollapsed=true`（Blender 判例：只暴露 BaseColor/Metallic/Roughness/IOR/Alpha/Normal，其余默认折叠）。
2. **factor / texture 成对相邻**（Blender"值-连接二态"的扁平化）；纹理槽 `m_fileExtensions` 填该引擎支持的格式。
3. **可见性三档**（Blender hide / fold / gray-out 的落地）：
   **特性关 → `m_readOnly=true`（灰化，仍可见）；模式互斥 → `m_visible=false`（隐藏）。**
   Godot 端直接映射：`USAGE_NONE` → 隐藏、`USAGE_NO_EDITOR` → 灰化。
4. **范围/单位照搬引擎原生**（Godot `hint_range`、rbfx 官方检查器范围），**不做跨引擎归一**
   （`Plan.md` §A6 C4 归一化适用范围：编辑器不参与计算的量不归一）。
   单位填 `m_suffix`（S6）——Godot 的 `hint_range` 尾部 `suffix:xxx` 直接搬过来；
   Filament 的 `emissiveStrength` 填 `"nits"`；无单位的留空。
5. **`m_description` 视为必填**（Blender 的 Principled 几乎每个 socket 都有 `.description()`），校验器对空值告警。
6. **`m_colorSpace` 必填**：按引擎实际存储填（rbfx `MatDiffColor`/`MatEmissiveColor` = `Srgb`，其余多为 `Linear`）。
7. **L0 覆盖度**：8 个语义槽按 §8.2 表人工标注一次，校验器断言，缺失告警。
   **校验器只保留三条机械断言**（§7.1，~60 行）：L0 八槽覆盖度 + 重复 `m_id` + 空 `m_description`。
   L1/L2 的"策展注明语义"是**文档纪律**（§8.1），不是可断言的东西，不进校验器。
8. **特性开关提升组头**：若某组由一个 bool 总开关控制（`emission_enabled` 这类），把该 bool 的 id 填进
   `m_toggleProperty`（S7，§9.4）。Godot 从 `PROPERTY_HINT_GROUP_ENABLE` 自动填；rbfx/Filament 手工策展时按同一判据填。
   **留空 = 普通组**，不是错误。

---

## §9 明确驳回的设计（含理由）

### 9.1 ❌ 中立材质格式 `.cematerial`（C 稿提案）
**理由 1（规则）**：Plan §A0 明文——"CEE = 跨引擎的编辑器前端（UI/交互/工具链可复用），**不是跨引擎的内容**"。材质是内容。
一个中立文件就是**第三种真相源**（引擎原生文件之外），必然带来同步纪律与有损项。
**理由 2（技术）**：§8.3 显示高级特性交集近乎空集，中立超集里绝大多数字段对任一后端都是死的；
中立**子集**（只存 8 个 L0 槽）在保存时丢失引擎特有属性，是数据损坏。
**理由 3（工程）**：中立格式意味着"中立 ↔ 4 种原生"共 8 个转换器要维护，而直接编辑原生格式是 0 个。
**理由 4（自证）**：C 稿自己给该功能加了"自毁条款"（不落地就删字段）——需要自毁条款本身就说明它是脆弱的。
**替代**：直接编辑引擎原生文件。跨引擎材质**转换**若将来确有需求，走 v2 **独立离线工具**（纯数据，不进编辑器运行时、
不进契约），映射表即 §8.2，范式即 rbfx 官方 `GLTFMaterialImporter`（`GLTFImporter.cpp:2345-2660`，一个完整的
glTF→引擎材质转换器，现成可抄）。

### 9.2 ❌ 属性上的语义标签 `m_semantic`（B 稿初版、C 稿提案）
**理由**：它唯一的消费者就是 §9.1 被驳回的中立格式，失去消费者后即是**死字段**。
留着就得维护"枚举 ↔ 各引擎属性"的映射表，是典型的 speculative generality。
**替代**：§8.2 的对照表作为**设计时命名约定 + 校验器覆盖度断言**，零契约成本。

### 9.3 ❌ 跨引擎 Functor / Lua / 可见性谓词 DSL
Atom 的 functor 是**任意 C++ 或 Lua 代码**（`MaterialFunctor.h:340-349` 三个 Process 上下文，
`LuaMaterialFunctor.h` 全套绑定），不是数据，无法查表映射。
谓词 DSL 的否决理由见 §4.5（重建通道存在后是冗余的第二套机制，且表达不了 Godot 的真实规则）。
**替代**：`SetResult::AppliedSchemaChanged` + 重取 schema 与值，一个机制统吃四套动态元数据。

### 9.4 ✅ 组标题上的启用勾选框 `m_toggleProperty`（B 稿提案）—— **2026-08-25 由驳回改为采纳**

**本条曾被本文以"违反 C3"为由驳回，2026-08-25 已撤销**（C3 本意是"不重复造轮子"而非"禁止扩展"，`Plan.md` §A6 C3 澄清；错误复盘见 §11.4-6）。
按三级优先序与两条判定手段重审：

**(a) stock 为什么满足不了**（带证据）：`InspectorGroupHeaderWidget : AzQtComponents::ExtendedLabel`
（`InspectorGroupHeaderWidget.h:20-21`）是**自绘 label**，只有 `SetExpanded/IsExpanded` + `clicked/expanded/collapsed`
三个信号 + `mousePressEvent/paintEvent`，**没有 layout 可以插入 QCheckBox**。

**(b) 为什么不是过度设计**：`BaseMaterial3D` 用了 **15 次** `PROPERTY_HINT_GROUP_ENABLE`
（`material.cpp:3603/3613/3618/3622/3628/3634/3639/3646/3657/3663/3670/3675/3681/3720/3734`——
emission / normal / bent_normal / rim / clearcoat / anisotropy / ao / heightmap / subsurf_scatter ×2 /
backlight / refraction / detail / grow / proximity_fade）。**Godot 材质的整个进阶区都靠它呈现**，
不是边缘特性。降级成"组内第一行 bool"要为 15 个组各多一行，且"这个开关管着整组"的从属关系在视觉上消失。
（本文初稿"95% 的价值，0% 的成本"是未经计数的估计，实测后不成立。）

**落地形态 = ② 级扩展，不是 ③ 级自建（S7）**：

| 选项 | 做法 | 判定 |
|---|---|---|
| A：零上游改动 | CEE 子类整个重写 `AddGroup` | ❌ `m_groups` / `GroupWidgetPair` 是 **private**（`InspectorWidget.h:91-101`），`SetGroupVisible`/`RefreshGroup`/`RebuildGroup`/`ExpandGroup`/… 全查它 → 要影子实现 ~200 行框架簿记。**这才是真正的"重复造轮子"** |
| **B：S7（采纳）** | 上游加 `virtual InspectorGroupHeaderWidget* CreateGroupHeader()`（默认返回原类，**3 行**）；CEE 侧 `CeeInspectorGroupHeaderWidget` override `paintEvent` 用 `QStyle::PE_IndicatorCheckBox` 画原生风格指示器 + 命中测试；toggle 分发走**现成的** protected 虚钩子 `InspectorWidget::OnHeaderClicked`（`InspectorWidget.h:89`） | ✅ 与 S2/S5/S6 同形状：极小的上游虚工厂解锁 CEE 扩展，**上游行为零变化**。CEE 侧 ~100 行 |

**契约代价**：`MaterialPropertyGroupDesc::m_toggleProperty` 一个字段（§4.1）。纯虚计数不变，CI 断言不受影响。
Godot 从 `PROPERTY_HINT_GROUP_ENABLE` **免费填**；rbfx/Filament 留空 = 普通组。

**排期（2026-08-25 裁决）**：**契约字段 v1 就加**（避免契约后期增长、避免按 C4 同步改四个后端）；
**S7 上游虚工厂 + CEE header 子类排在 P3 Godot 后端**——它唯一的消费者是 Godot，消费者与实现同时落地。
**P2 rbfx 首发不受影响**（`m_toggleProperty` 留空即退回普通组，与本条采纳前的行为一致）。

**渲染细节**：被提升到组头的那个 bool **不再在属性列表里出现一行**（否则重复）；它的编辑仍走正常的
`DynamicProperty` → `ApplyEdit` 通路，undo/`AppliedSchemaChanged` 语义完全不变——组头只是它的第二个视图。

### 9.5 ❌ 父材质 / 继承链 / `SaveAsChild`（A 稿初版曾留哨兵）
Atom 独有（`MaterialSourceData.h:62`；`MaterialSourceData.cpp:217-341` 构建期扁平化）；rbfx（`Clone` 是独立副本）、
Godot（`next_pass` 是多趟渲染不是继承）、Filament（`.filamat` 是类型不是父实例）都没有——其余三家保存时必须扁平化，
生成的文件不可 round-trip（§4.4 同）。
**替代**：`m_defaultValue` = 材质类型默认值，修改指示器语义变为"与类型默认值不同"。
Godot 的 `.tres` 本来就只写非默认值（`resource_format_text.cpp:1986-1990`），语义完全一致。

### 9.6 ❌ 复合类型（SamplerState / Matrix3x3 / Matrix4x4）
Atom 的属性类型里有 `SamplerState`（`MaterialPropertyDescriptor.h:70-87`），`ArePropertyValuesEqual` 还支持三种矩阵
（`MaterialPropertyUtil.cpp:123-125`）。但 StandardPBR 一个都没暴露；Godot 用 `texture_filter`/`texture_repeat` 两个枚举；
Filament 用 `TextureSampler` 的 filter/wrap 枚举。
**替代**：采样器拆成普通枚举属性（filter / wrapU / wrapV），四引擎都能表达。矩阵 v1 不进 variant，
真有后端需要时再加一个 alternative（封闭 variant 的好处：加类型时编译器会指出所有需要更新的地方）。

### 9.7 ❌ 节点图 / MaterialCanvas / shader 编辑与变体管理
本方案对标的是 **MaterialEditor**（属性网格），不是 **MaterialCanvas**（节点图，需要 `GraphCanvas` + `GraphModel`
两个额外 Gem + 每引擎的 shader 代码生成器）。
**强反证**：Blender 的 glTF 导出器为把任意节点图逆推回 PBR 参数写了 **1169 行图模式识别**（`search_node_tree.py`）
且经常失败 —— 这正说明引擎必须以 **PBR 参数表**为一等公民；Blender 自己的主路径也是"单个 Principled BSDF 节点即完整材质"。
**节点图是表达力债务，不是能力。**
shader 编译/变体是引擎内部事务（Atom `MaterialTypeBuilder` / rbfx technique / Godot `MaterialKey` / Filament `.filamat`），
类型枚举已经归位（§8.4 ①）。

### 9.8 ❌ 其余非目标（逐条给理由）

| 项 | 理由 |
|---|---|
| 运行时后端热切换 | Plan §A6 C6（引擎全局单例 + 表面句柄） |
| 贴图通道重打包（异路径 RMO） | v1 = 约束 + 告警，不静默重打包；算法现成（`GLTFImporter.cpp:2271-2332`），留 v2 |
| 属性动画（rbfx `parameteranimation`） | 单引擎特性；`SaveMaterial` 序列化活实例 → 已存在的动画节点原样保留不丢 |
| AssetProcessor / 外部文件监听（v1） | 无 AP 进程；dock 形态下同进程同一后端实例，材质改了场景立即变（§6.1 ④），跨进程同步桥**整条不再存在**。外部修改的监听接缝（`AtomToolsDocumentNotificationBus::OnDocumentExternallyModified`）v1.5 再议 |
| 材质缩略图 / `PreviewRenderer`（v1） | 官方 `PreviewRenderer` 深绑 RPI（九个 Atom 头）；扩展名图标判例已定；v2 沿 `SetPreviewMaterial` 路径延伸 |
| `DocumentPropertyEditor` / `AZ::Dom` 适配层 | `ReflectedPropertyEditor` + `DynamicProperty` 已覆盖且零新代码，引入 = 重复轮子 |
| ~~单位/subtype 控件~~ | **本条已撤回**（见 §3.4b）：`Suffix` 有 stock 支持，单位是白送的，已升格为 S6 落地项。撤回理由记在 §11.4-5 |
| 位掩码 / 图层控件（Godot `PROPERTY_HINT_LAYERS_*`） | 确无 stock handler，做了要新继承 QWidget 一族 → 撞 C3 判定手段。且材质域几乎不用（渲染层掩码属于对象而非材质），YAGNI |
| 统一物理光单位 / 统一 shading model | 自发光单位各异（KHR 倍数 vs Filament nits）、shading model 各异（Filament cloth、Godot toon、rbfx 非 PBR）——交集外无消费者，YAGNI |
| 无证据的自建/扩展控件 | C3（2026-08-25 澄清后）允许扩展，但要求落两条带证据的记录：**(a) stock 为什么不够（带 `文件:行号`）** + **(b) 为什么不是过度设计**。本方案里走到 ② 级的六项 S2/S5/S6/S7/S8/S9 均已按此格式落账（§3.2 / §3.4b / §9.4） |
| `PreviewModel` 扩到 Cylinder/ShaderBall（B 稿提案 5 项） | ShaderBall 是 O3DE 专属资产；Cylinder YAGNI。收窄为 Sphere/Cube/Plane（枚举可扩展） |
| `PreviewConfig`（hdri/exposure/grid，A 稿提案） | v1 无消费者；视口设置走官方 `SettingsDialog`，环境预设列 v2 |

### 9.9 ❌ 预览用第二个 OS 表面（多 swapchain 绑 Qt HWND）（2026-08-25 驳回；2026-08-26 承重论据替换，驳回不变）

本文初稿曾以"独立进程内唯一场景 = 预览场景"回避此问题；dock 改判后必须正面回答。
按 C3 判定手段落账（完整技术论证与五引擎横评在 §6.4.1，RTT/回读先例与限频在 §6.4.2/6.4.4；
2026-08-26 双线复核全文见 `material_editor_research.md`，处置记 §11.8）：

**⚠️ 承重论据已于 2026-08-26 双线复核后替换**（全文 `material_editor_research.md`）。
旧版主论据是"上游反证 / 全行业零判例"——那是**可被一条反例推翻的经验归纳，且已被 O3DE 推翻**
（O3DE 材质编辑器预览恰恰是 `winId()` + 独立 swapchain，§6.4.1 订正 ①）。现降为旁证，主论据换成引擎能力硬事实。

**先澄清一个常见误解：这不是 Qt 的限制，CEE 也不是没在给句柄。**
CEE 主视口本来就是**纯句柄路径**：`EngineViewportWindow`（`QWindow` + `setSurfaceType`，Qt 不分配 backing store）
→ `winId()` → 后端（rbfx `params[EP_EXTERNAL_WINDOW]`，`RbfxBackend.cpp:383`；Godot `--wid`，`GodotBackend.cpp:463`），
见 `Plan.md` §B2。**问题不在"能不能给第 1 个"，而在"能不能给第 2 个"。**

**(a) 为什么不做 —— 两个后端在源码层面收不了第 2 个表面**

- **rbfx（三层阻断，层层独立）**：
  ① **类型系统级**：`RenderSurface` 的**唯一构造函数只接受 `Texture*`**
     （`Graphics/RenderSurface.h:43`，成员 `:143` `const WeakPtr<Texture> parentTexture_`）
     ⇒ 一个 `Viewport` 只有「主交换链 或 一张纹理」两种归宿，**没有第三种**。
  ② **回退硬编码**：`renderTarget_` 为空时回退到那条唯一主链
     （`RenderPipeline/RenderBuffer.cpp:188,223` → `RenderAPI/RenderTargetView.cpp:71,73,119,121` 无参 `GetSwapChain()`；
     `RenderDevice.h:194,201` 的 `window_`/`swapChain_` **各只有一个，非集合**；`RenderPipeline/` 对 `ISwapChain` **零引用**）。
  ③ **次级链不通场景管线**：`CreateSecondarySwapChain` 存在（`RenderDevice.h:84`），但参数是 **`SDL_Window*` 不是 HWND**，
     且**全仓唯一调用者是 ImGui 多视口**（`SystemUI/ImGuiDiligentRendererEx.cpp:338`，绕开 `RenderPipeline` 直画顶点），
     返回的裸 `ISwapChain` **没有任何路径能变成 `RenderSurface` 交给 `Viewport`**（因 ①）。
     且该文件在 CEE 里被 `URHO3D_SYSTEMUI=OFF` **编译掉**——这层阻断在 CEE 是**编译期**的（§6.4.5 落地风险注）。
  ⇒ rbfx 走句柄 = **动渲染目标的抽象层**，不是"改 4-5 处"。
- **Godot（一行硬编码，但在 DisplayServer 里）**：`create_sub_window` 给 `_create_window` 的第 7 参 `p_parent_hwnd`
  **写死 `nullptr`**（`display_server_windows.cpp:1855`）；只有主窗口创建（`:8094`、`:8291`）才收来自 `--wid` 的
  `parent_hwnd` ⇒ **外部 HWND 只能喂 `MAIN_WINDOW_ID`，而 CEE 已经用掉了这唯一一次机会**。
  叠加 libgodot 无 "render into external surface" API（只能自建窗口，`Plan.md:124`）。
- **共同放大项**：两者都是**外部未 pin 的 checkout**（`External/CMakeLists.txt:298,334`），
  对未 pin 上游打私补丁直接顶到 C5 接入成本。
- **旁证（降级，需带限定）**：五引擎材质预览 0/5 用第二 OS 表面（§6.4.1）。
  **但必须同时说明**：其中 4 家是**无效样本**（UI 自绘，不存在"外来窗口"这个选项），
  唯一同处境的 O3DE 反而用了句柄——所以此条只能作旁证，不能承重。
- **对照——O3DE 为什么能**：Atom 是它**自己的 RHI**，`CreateRenderPipelineForWindow(desc, windowContext)` 是一等公民
  （`EntityPreviewViewportScene.cpp:107`），不需要 fork 任何人。
  **它的两个前提 CEE 都不满足**：① 它的预览是独立进程里**不可浮动的 `centralWidget`**
  （`MaterialEditorMainWindow.cpp:52-53`，dock 位只给纯 Qt 面板），CEE 是 **dock 面板**；② 它的后端是 Atom，CEE 是 rbfx/Godot。

**(a2) 即使引擎肯收，Qt 侧仍有一层代价（次要，但需记账）**

① **airspace**：原生子窗口恒浮于 Qt 绘制之上，不参与裁剪/半透明/z 序。**两处自证**——
   CEE `EngineViewport.cpp:41` `WA_DontCreateNativeAncestors` 注释原文 *"otherwise makes docking re-create HWNDs on every drag and stutters"*；
   `Plan.md §B8 B3` 记着 `ViewportOverlayLabels` 与"native 表面上不叠 Qt widget"约束冲突。
   而预览面板天生要和 ADS auto-hide / 浮动拖拽 / tooltip / 顶部 `QComboBox` 工具条（§6.5）混排。
② **成本不对称**：主视口 1 条 swapchain 换全屏交互；预览球 1 条 swapchain（三重缓冲 VRAM）+ 整套表面生命周期
   （懒建/DPI 去抖/0×0 防御/销毁 latch，`Plan.md` §B2 那段要再来一遍）只换一个 256² 的球。
③ **撞单循环**：第二个表面 = 第二次 present + 拍频，破 `Plan.md` §B3「一帧一次 present」；RTT 路径对主循环**零改动**。
> 这三条**不足以**单独否决句柄（O3DE 扛下了 ①②）。记账是为了：将来若接入"能绑任意 swapchain"的后端（如 Filament），
> 这三条仍需付账，别以为 (a) 解锁了就白送。

**(b) 替代为什么不是过度设计**：RTT 是两引擎自家预览做法（rbfx `SceneRendererToTexture` / Godot SubViewport，
零引擎改动、上游各两处同款先例）；按需单帧让稳态停顿为零（§6.4.2）。
**回读的定性已于 2026-08-26 松口**（§6.4.2）：它不是"Qt 宿主强加的畸形"，而是"宿主与引擎渲染器不共享合成层"的通用代价——
**Blender 自绘 UI、没有 Qt，材质球照样 RTT + CPU 回读**（`render_preview.cc:1281,1289`），
缩略图更是五引擎 5/5 全走 RTT + 回读（含 O3DE `PreviewRenderer` → `AttachmentReadback` → `QImage`）。
成本仅"预览面板 ~150 行"，面板本身是 C3 ③ 级——stock 没有"引擎 RTT→QImage"面板，
且两个引擎自己的编辑器都有预览面板，非过度设计。

**(c) 同批已考虑并驳回的变体**

| 变体 | 驳回理由 |
|---|---|
| **只给 Diligent 后端开第二 swapchain**（Diligent 是 CEE 自有代码，成本近乎零） | 会使预览机制 **backend-dependent**（Diligent 走句柄、rbfx/Godot 走 RTT），契约不统一、`AcquirePreviewImage` 哨兵在一个后端上形同虚设、两条路径各自的 resize/DPI/生命周期都要单独验收。**收益仅限一个非主力后端，不值。** |
| 把 Qt HWND 包成 `SDL_Window` 再喂 rbfx `CreateSecondarySwapChain` | 绕不过 (a) 第①层——拿到的裸 `ISwapChain` 无路径变成 `RenderSurface`；且该代码在 CEE 被编译掉 |
| v1.5 交互预览时改走句柄 | 阻断 (a) 届时仍在。**升级正解 = fence 异步回读**（§6.4.4，v1.5 候选，零契约改动），不是改走句柄。**终极零拷贝 = 共享 GPU 纹理**（D3D11/12 shared handle → Qt RHI 纹理），但 Qt 侧承载体只能是 `QRhiWidget`/Qt Quick（QWidget+QPainter 合成器消费不了外来 GPU 纹理），**2026-08-26 裁决暂不采用**（撞 Plan §B2/§B3、引入额外技术债，§11.8）；它同时也是 v2「大尺寸纹理低成本进 Qt」的缺口，留到分屏场景编辑真正立项时再议。**v1 不做，不进契约。** |

---

## §10 分期与验收

| 阶段 | 内容 | 出口条件 |
|---|---|---|
| **P0**（**2 周**） | `AtomToolsFramework` 切 `.Core`/`.Static` + 4 处接缝（S1–S4）+ 3 个纯增字段（S2 `m_colorSpace` / S5 `m_fileExtensions` / S6 `m_suffix`）+ **S8 `ReflectCoreTypes` + S9 `AZ::RPI::MaterialUtils` 别名改名**（同一 PR）；`check_no_atom.ps1` 精化。~~抽 `CrossEngine::EditorFramework` 静态库~~**已取消**（§6.2：无第二个 app） | **回归门**：MaterialEditor / MaterialCanvas / ShaderManagementConsole **三个官方 Atom 工具照常构建且能启动**；`check_no_atom.ps1` 对 `.Core` 三层断言 PASS |
| **P1**（**2 周**） | `IMaterialSource` 契约（四后端同变更补齐，C4，**8 纯虚 + 2 哨兵**）+ `NullMaterialSource` + `CeeMaterialDocument` + dock 面板挂载（§6.1/6.3）+ **单文档 combo 薄壳（R1-C，~120 行）+ 主窗口三冲突处理（§6.3）** + `CeeMaterialPreviewPanel`（§6.4，含 10~15 Hz 限频与 ≤512² clamp，Null 后端返回 `Unsupported` 显示占位）+ `MaterialSchema` JSON 读写与校验器（3 条机械断言） | **C2 验收**：NullBackend 单后端构建；新建/打开/改属性/undo×10/保存全通；dock 面板可浮可停、布局 saveState/restoreState 生效 |
| **P2**（1.5 周） | rbfx 后端（§7.2）：schema JSON + XML 存取 + 参数/纹理/渲染态读写 + 预览场景（`SceneRendererToTexture`，§6.4.5）+ 按需回读（§6.4.2）+ `m_colorSpace` 接线 + `LoadMaterial` 走 `ResourceCache` 共享实例（§4.1）。**+ R4 退役**：删 `IEntityMirror` 三个 `*Resource*` 方法 + `ResourcePropertiesPanel` + AssetBrowser 材质入口改 `OpenDocument`（Animation 入口同 PR 评估，四后端同批，C4） | 见下尺子 1/2/3/5/6 + rbfx 清单 |
| **P3**（1.5 周） | Godot 后端（§7.3）：`get_property_list` → schema 自动生成（含**丢弃规则**）；`.tres` 存取；`AppliedSchemaChanged` 通道；预览（`SubViewport` + `UPDATE_ONCE` **异步** `texture_get_data_async` 回读，§6.4.2/6.4.3）。**+ S7 组头勾选框**（上游 3 行虚工厂 + CEE header 子类 ~100 行，§9.4）——消费者与实现同批落地 | 同上 + Godot 原版 Inspector 交叉验证：**15 个 `GROUP_ENABLE` 组全部显示为组头勾选框**，且被提升的 bool 不在列表里重复出现 |
| **P4**（0.5 周） | 预览交互收尾（`QComboBox` → `SetPreviewModel`；置脏→刷新链路 §6.4.4 全链联调）；六把尺子跑通 | 见下 |
| **P5**（0.5 周，**P3 完成后评估触发**） | Filament 后端最小集 = 契约证伪 | 只实现 8 纯虚**零额外方法**；做不到即回炉改契约（同 `rbfx_migration.md` §4 E5 的 Diligent PoC 手法）。**触发条件（2026-08-25 定）**：P2/P3 过尺子、契约冻住后再花这 0.5 周——证伪实验只给已定稿的契约买保险，不验证旧契约 |
| **v1.5 / v2** | AssetBrowser 拖放建贴图行；预览相机交互（orbit/dolly，置脏入口 +1，契约形状不变）；贴图缩略图；材质缩略图（AssetBrowser，沿 `SetPreviewMaterial`/`AcquirePreviewImage` 同路径按需渲一帧 + 缓存——B 稿后续可选，dock+RTT 下自然延伸）；**贴图集一键导入**（Blender Node Wrangler 判例：`add_principled_setup.py` 关键词表 base_color/metallic/rough/normal/ao/emission + gloss 图自动 invert 提示，关键词表可直接抄）；离线 glTF↔引擎材质转换工具；rbfx 异路径图重打包；`ShaderMaterial` 支持；**rbfx fence 异步回读**（§6.4.4 升级路径，零契约改动，双后端零停顿） | — |

### 六把验收尺子（可执行、可证伪）

1. **视觉等价尺子**（"跨引擎"从愿景变事实）：同一进程分别以 `--backend rbfx` 与 `--backend godot` 启动，
   输入同一组 L0 参数（白 albedo / metallic=1 / roughness=0.3 / 同一张法线图 / 同一张 AO 图）→ 两次预览球截图**肉眼一致**。
   重点验色彩空间：不发灰、不过曝 —— 这是 `m_colorSpace` 的直接验收。**另配每后端 gamma↔linear 往返恒等单测**
   （A 稿 R2 判例：编码→解码→编码比特恒等；肉眼验收抓不住 ±1 LSB 漂移，单测能）。
2. **无损尺子**：Godot `.tres` / rbfx `.xml` 打开 → **不做任何编辑** → 保存 → 文件差异仅限格式化
   （`next_pass`/`stencil_*`/`proximity_fade`/`parameteranimation`/`depthbias`/`renderorder` 全部保留）。
   —— 结构性保证（§2.2），不靠字段覆盖率。**共享实例子断言**（§4.1）：打开 → 不编辑 → 关闭 → 不保存，
   引擎侧活实例始终是 `ResourceCache` 同一份（rbfx）/ `CACHE_MODE_REUSE` 命中的同一份（godot），
   引擎运行时状态不变。
3. **契约尺子**：`IMaterialSource` 纯虚 == 8 封顶（CI grep 断言，哨兵不计数）；四后端 stub 率 0；Filament PoC 零额外方法；
   **且 `IEntityMirror` 纯虚 15→12**（R4 退役同批落地，旧材质编辑路径三方法随 `ResourcePropertiesPanel` 删除）。
4. **框架尺子**（C3 三级优先序）：**③ 级自建 = 1**（`CeeMaterialPreviewPanel`，判据落账 §9.9：stock 无"引擎 RTT→QImage"面板，且两引擎自己的编辑器都有预览面板故非过度设计）；
   ② 级扩展共 6 项（S2/S5/S6 属转发，S7 属子类化，S8 纯增函数，S9 纯改名），
   **每项都已落 (a) stock 为什么不够（带 `文件:行号`）+ (b) 为什么不是过度设计** 两条证据（§3.2/§3.4b/§9.4）；三官方 Atom 工具回归门 PASS；
   `check_no_atom.ps1` 对 `AtomToolsFramework.Core` 与 `CrossEngineEditor` 双 target PASS（C1）；
   NullBackend 单后端全流程（C2）。
5. **undo 尺子**：20 次属性修改后 20 次 Ctrl+Z 回到初始（拖一次滑条 = 一步，框架现成映射）。
6. **交互性能尺子**（R5，2026-08-25 opus review 吸收）：连续拖动 `Roughness` 滑杆 3 秒，主视口帧率下降 < 20%
   （拖另一面板的属性滑杆作对照）；Tracy 中 `RawTexture::Read` / `texture_get_data_async` 计数 ≤ 15 次/秒
   （10~15 Hz 限频生效）；松手后 1 秒内回读归零（isVisible() 门 + 无脏无回读）。

### rbfx 首发的具体验收清单（P2 出口）

- [ ] 打开 `bin/Data/Materials/PBR/Lead.xml`，属性面板出现 Albedo/Normal/Properties 三个纹理槽
      与 `MatDiffColor/Roughness/Metallic/MatSpecColor/MatEnvMapColor/UOffset/VOffset` 参数
- [ ] 拖动 `Roughness` 滑杆，预览球在 **2-3 帧内**（约 50 ms）变化——1 帧按需渲染 + 回读停顿（`WaitForIdle`，`RawTexture.cpp:1017`）+ 1 个 60fps 门内取回（§6.4.4）。稳态（不拖任何东西）时**零回读零停顿**：profiler 里不出现 `RawTexture::Read`
- [ ] 改 `cull` 枚举 → 返回 `Applied`；改 technique → 返回 `AppliedSchemaChanged` 且面板重建
- [ ] `MatDiffColor` 拾色器按 sRGB 显示（`m_colorSpace = Srgb`），对着 `1,1,1,1` 显示纯白
- [ ] 贴图槽的浏览对话框**只列 rbfx 支持的图片格式**（S5 是否落地的直接验收）
- [ ] 保存后 XML 结构与 rbfx 原生一致，**rbfx 原版编辑器打开无差异**

---

## §11 四方裁决记录（2026-08-25）

> **口径说明（诚实披露）**：本文阶段一独立成稿期间，会话的 MEMORY.md 索引里已存在一条对 A 稿（deepseek）的
> 一行压缩摘要（"两层模型 / Core-Atom 切分 / 独立 app / IMaterialSource 8 纯虚+1 哨兵 / m_colorSpace 必修；
> .cematerial 与 m_semantic 驳回"），属自动加载的上下文，无法屏蔽。
> 本文所有架构结论仍是从源码测量独立推出的（§3.1 的逐目录 Atom 引用计数、§3.2 的 4 处接缝、§3.4 的 S5 缺口
> 都是本文独有的一手测量），但读者应知悉这一重合并非完全独立。

### 11.1 四方一致项（无需裁决）

新增第四子契约 `IMaterialSource`（均否 `InvokeCustom` / 均否扩 `IEntityMirror`）；两层模型（值不出引擎）；
Core/Atom CMake 切分（否 vendoring / 否重写）；不继承 `AtomToolsApplication`；`SetResult` 三态重建通道（否谓词 DSL）；
undo 100% 复用框架；纹理 = 路径字符串；`m_colorSpace` 必修；软/硬范围双轨；
驳回中立格式 / `m_semantic` / 父材质 / 节点图 / shader 编辑 / 运行时热切换；0 stub 设计目标。

> 上表中的「独立 app 形态」「后端自管预览场景 + 单表面直渲」「组头 toggle 驳回」「工作量 6 人周」四项
> 均已被同日/后续裁决推翻（§11.6 / §11.7 / §9.4），**上表按裁决当天立场如实保留**，
> 现行立场以 §6.1 / §6.4 / §9.4 / §10 为准。

### 11.2 从三稿吸收的内容（逐条落点）

| 来源 | 吸收内容 | 落点与理由 |
|---|---|---|
| A+B+C | **`MaterialHandle`（后端分配，0=invalid）替代本稿初版的 `MaterialId = AZ::Uuid`（编辑器分配）** | §4.1。失败态由返回 0 表达（省掉一个 bool 出参），后端拥有自己的 id 空间；与 `IEntityMirror::CreateObject → AZ::EntityId`（`IEntityMirror.h:80`）家族同形 |
| A+B+C | **schema 与值分离**：`GetMaterialState(handle, MaterialTypeDesc&, MaterialPropertyValueMap&)` 替代本稿初版把当前值塞进 `MaterialPropertyDesc` | §4.1。理由不止是整洁——`MaterialTypeDesc` 不含值，才能**直接从策展 JSON 反序列化**（§7.1），零额外类型 |
| A+B+C | **`MaterialPropertyType` 单一类型枚举**（Enum / Texture 作为显式类型）替代本稿初版的 `MaterialValueKind` + `MaterialStringRole` 双枚举 | §4.1。少一个概念，且消除"两字段必须互相吻合"的隐式约束 |
| A+B+C | **`AZStd::optional<double>` 范围**替代本稿初版的 variant 范围字段 | §4.1。"未设置"语义更明确；编辑器按值类型广播（§5.2） |
| A+B+C | `SetResult::AppliedSchemaChanged` 命名 | §4.1。本稿初版叫 `AppliedAndDirty`，与文档 dirty 语义撞车 |
| A+B+C | **`m_id` = 扁平引擎原生名，不做分组点分拼接** | §2.2。**对本稿初版的实质修正**：Godot `ADD_GROUP` 的 hint_string 本就是属性名前缀（`class_db.cpp:1384-1408`），再点分会破坏 `set(name,…)` 可路由性 |
| A+B+C | `MaterialTypeInfo::m_fileExtensions`（vector）+ `MaterialPropertyDesc::m_fileExtensions` | §4.1。本稿初版只有单个 `m_extension`；rbfx 接受 `.xml`+`.material`，Godot 接受 `.tres`+`.res` |
| B+C | `MaterialPropertyGroupDesc::m_defaultCollapsed`（Blender `default_closed` 判例） | §4.1 + §5.4（落地路径本文修正为 `ShouldGroupAutoExpanded` override） |
| B+C | **`m_description` 视为必填 + schema 校验器告警**（Blender 判例） | §8.5 规则 5 + §7.1 |
| B+C | **可见性三档策展规则**：特性关 → 灰化；模式互斥 → 隐藏 | §8.5 规则 3 |
| B+C | **Godot 赋贴图自动 factor=1.0 的规范化必须回显** | §4.5。直接解释了"为什么 `GetMaterialState` 要同时返回值" |
| A+B | **L0/L1/L2 语义槽分级 + 校验器覆盖度断言** | §8.1 / §8.5 规则 7。把"跨引擎"变成可检查断言 |
| A+B | **五家 specular 默认全 0.5 的收敛实证** | §0 / §8.3。比本稿初版的任何论据都硬 |
| A+B | **Blender 1169 行 `search_node_tree.py` 图模式识别的反节点图硬证** | §9.7。本稿初版只用了 `get_factor`（`:452-490`），三稿的框法更强 |
| A+B+C | **rbfx `SetShaderParameter` 必须保留 `isCustom_`**（`rbfx_migration.md` §3.2 已踩坑） | §7.2 |
| A+B+C | **rbfx "Custom Parameters" 兜底组** + **贴图必须文件路径**（无名内存贴图序列化为空） | §7.2 |
| A+B+C | **Filament 反射只给 name/type/precision，无 UI 元数据**（`MaterialParser.cpp:116-156`） | §7.4。**修正本稿初版"schema 来源 = `.filamat` 自描述"的过度乐观表述**；补 sidecar + `getParameters()` 交叉校验 + `MATERIAL_VERSION` 锁 |
| A+B+C | ~~**CMake 前置：CEE 必须先抽 `CrossEngine::EditorFramework` 静态库**~~ | §6.2。**2026-08-25 改判 dock 后取消**（无第二个 app，无需抽库）。`Code/CMakeLists.txt:27-28` 单 APPLICATION target 保持不动 |
| A+B+C | **P0 回归门 = 三个官方 Atom 工具构建 + 能启动** | §10。本稿初版只写"MaterialEditor/MaterialCanvas 重建通过"，缺 ShaderManagementConsole 与"能启动" |
| A+B+C | **契约不增长 CI 断言（纯虚 == 8）** | §3.6 / §10 尺子 3 |
| A+B | **视觉等价尺子 + 无损尺子** | §10 尺子 1/2。把"跨引擎"和"无损"都变成可证伪命题 |
| A+B+C | ~~**两进程无实时同步 → `QFileSystemWatcher` 桥（v1.5）**~~ | **2026-08-25 改判 dock 后该限制整条消失**（同进程同一后端实例，材质改了场景立即变，§6.1）。外部修改监听接缝（`OnDocumentExternallyModified` + `QueueReopenModifiedDocuments` 现成）v1.5 再议（§9.8） |
| A+B+C | **模式类参数维度归位表**（属性维度 vs 类型维度，零新增概念） | §8.4 ① |
| B+C | **`PreviewModel` 枚举替代本稿初版的 `SetPreviewModel(string)`** | §4.1。C4 数据面禁 stringly-typed；且收窄为 Sphere/Cube/Plane |
| A+B+C | **删除本稿初版的 `SetPreviewEnvironment` 哨兵** | §4.1。v1 无消费者、无后端实现 = 死代码。契约从 8+2 收敛为 **8+1**。（2026-08-25 又因 RTT 预览加回第 2 个哨兵 `AcquirePreviewImage`，§6.4.3——两次 8+2 成分不同） |
| **B** | **组头启用勾选框 `m_toggleProperty`（2026-08-25 补吸收）** | §4.1 字段 + §9.4 完整论证 + §5.4/§7.3/§8.5 接线 + P3 排期。**本文初稿曾以"违 C3"驳回，是误读规则；B 稿的原始判断成立** |
| A+B | Node Wrangler 贴图集一键导入（v2）；离线 glTF 转换工具走 rbfx `GLTFMaterialImporter` 范式 | §10 v2 |
| B | **材质缩略图接 AssetBrowser**（2026-08-25 新规则重扫吸收） | §10 v1.5/v2 行。dock+RTT 后与预览同路径（`SetPreviewMaterial`/`AcquirePreviewImage` 按需渲一帧 + 缓存），三引擎缩略图同款机制 |
| A | **每后端 gamma↔linear 往返恒等单测**（R2，2026-08-25 重扫吸收） | §10 尺子 1。`m_colorSpace` 最难缺陷类（±1 LSB 漂移肉眼抓不住）的直接验收 |
| A | **Godot 纹理 `.import` 缓存风险**（R10，2026-08-25 重扫吸收） | §7.3 交叉引用 `godot_migration.md` G6 既有缓解（首次 headless 导入），不新增机制 |

### 11.3 驳回三稿的提案（含理由）

| 提案 | 出处 | 驳回理由 |
|---|---|---|
| `.cematerial` 中立交换格式 | C 稿（B 稿初版亦有，已自删） | §9.1：A0 第三真相源；且 C 稿自带"自毁条款"即证其脆弱。四方中三方驳回 |
| 契约字段 `m_semantic` | C 稿、B 稿初版 | §9.2：唯一消费者被驳回后即死字段 |
| ~~组头勾选框 `m_toggleProperty`~~ | B 稿 | **驳回已撤销（2026-08-25）**——原驳回理由"违 C3"建立在对 C3 的误读上。C3 澄清本意后按判定手段重审通过：`BaseMaterial3D` 用了 15 次，stock header 是自绘 label 无法插控件，落地形态是 ②级扩展（S7）。**B 稿在这一点上是对的，本文初稿错了**。详见 §9.4 |
| `PreviewModel` 扩到 5 项（Cylinder / ShaderBall） | B 稿 | §9.8：ShaderBall 是 O3DE 专属资产，Cylinder YAGNI |
| `CanSaveAsChild`/`SaveAsChild` 契约哨兵、`PreviewConfig` 结构 | A 稿初版 | §9.5 / §9.8：四引擎无父材质；hdri/exposure/grid v1 无消费者。哨兵也是死代码 |
| vendoring `AtomToolsFramework` 子集 | A 稿初版 | §3.5：同仓第一方代码，vendoring = 永久分叉。降为退路 R1 |
| 内嵌 dock + RTT 回读预览 | C 稿初版 | §6.1：与 Plan §B2"无 QImage 拷贝"冲突；Godot 离屏嵌入可用性未证伪。**方向已被 2026-08-25 双裁决证实**（形态改判 dock、预览改判 RTT 按需回读）——两条驳回理由同日均被推翻：按需渲一帧 ≠ §B2 的每帧全屏拷贝；Godot 离屏嵌入已源码核实 (a) 零 fork。**C 稿初版先于裁决对了，本文初稿错了** |
| 运行时通用 PBR 结构体 + 扩展袋 | C 稿初版 | 隐性翻译层；主动丢弃引擎白送的元数据；边界要逐后端维护"同义字段跳过表" |
| "只移植模式不移植代码" | C 稿初版 | 要重写 `AtomToolsDocumentSystem`(~700 行) + `AtomToolsDocumentMainWindow`(~850 行) + undo 栈，与"100% 复用"的要求有实质距离。（2026-08-25 opus 复核 R1 补证：该文件实为 857 行，标签页簿记占 `AtomToolsDocumentMainWindow.cpp:289-708`；dock 改判后这笔簿记成本本会回归——本文已按单文档 combo 薄壳（R1-C，~120 行）处置，不搬簿记，§6.5/§10 P1） |
| **`AtomToolsAnyDocument` 归 Core（3 处泄漏点处置）** | A+B+C 三稿一致 | **§3.3：本文判定归 Atom 侧，那 3 处不必处理。** 该类只服务 `AnyAsset` 型文档（ShaderManagementConsole / PassCanvas），CEE 不经过它；泄漏点从 7 降到 4，少改 3 处上游代码。三稿的改法已备为退路 |
| **New Material 模板向导；多选材质批量编辑** | B 稿后续可选 | 不吸收（2026-08-25 重扫，v2 再议）：模板向导 v1 的 `CreateDocumentDialog` + `MaterialTypeInfo` 已够（rbfx `MaterialFactory` 判例留 v2）；批量编辑与多 tab 文档模型冲突——rbfx"值不一致高亮"依赖资产选择式编辑的共享检视器，CEE 是文档式，且 §B10 交互契约无此需求 |
| **Godot 长尾 hint（exp 范围 / radians_as_degrees）** | C 稿"刻意不做"清单 | 不吸收：新 C3 下技术上可做（S5/S6 同类转发），但 v1 无消费场景（L0 参数无 exp 型，数值仍可精确输入）；slider 曲率是呈现不是语义，列 v2 可缓 |

### 11.4 本文对三稿承重事实的独立复核结果

按 Progress.md:51 铁律，阶段二吸收的每条承重声称都重新 grep 过。**3 处三稿偏差 + 1 处四方全漏 + 3 处本文初稿错误**：

| # | 复核结果 | 处置 |
|---|---|---|
| 1-4 | 三稿偏差（已并入正文）：S5 贴图扩展名过滤缺口（`DynamicProperty.cpp:130-181` 从不转发 `Extensions`/`Title`）、`m_defaultCollapsed` 实为虚钩子 `InspectorWidget::ShouldGroupAutoExpanded`（`InspectorWidget.h:86`）非数据字段、泄漏点 7→4（`AtomToolsAnyDocument` 按文件归属消 3 处）、`Util.cpp` 两处调用（S4） | §3.2-3.4 / §5.4 |
| **5** | **本文初稿错误 ①**：§9.8 曾断言"单位控件无 stock 支持"——实测 `AZ::Edit::Attributes::Suffix` 被四族 stock 控件全部消费（`EditContextConstants.inl:154` + `PropertyDoubleSliderCtrl.cpp:246` 等，全列见 §3.4b） | 撤回，升格 **S6** |
| **6** | **本文初稿错误 ②**：§9.4 以"违 C3"驳回组头勾选框——**规则读错**（C3 本意 = 不重复造轮子）+ **代价估错**（`GROUP_ENABLE` 实为 15 处，"95%"未计数） | 驳回撤销，采纳 **S7**（§9.4） |
| **7** | **本文初稿错误 ③**（2026-08-25 opus 复核）：§6.5 曾列 `DocumentTabWidget` 直用——幻影 API（全仓零命中） | 单文档 combo 薄壳（§6.3/§6.5，§11.7） |

其余复核通过的承重项：`ColorUtils.h:22-24` 双函数、`CrossEngineEditorApplication.h:35-37` 双继承、
`Code/CMakeLists.txt:27-28` 单 APPLICATION target、`IEntityMirror.h:80`、`AtomToolsMainWindow.cpp:9,613-615`、
`AtomToolsDocumentInspector.cpp:93-119`（`IPropertyEditorNotify` → `BeginEdit/EndEdit` 现成映射）。
**同一条教训**：错误 ①②③ 根因相同——**拿规则当省掉验证的借口**（"没查"写成"规则不让" / "没数"写成比例 / 幻影 API 当现成件）。

### 11.5 本文相对三稿的净增量

九项净增量全部已并入正文：§1.3（Atom 不是后端的分工认知）；§3.4/§3.4b（S5/S6 缺口与落地）；
§3.3（泄漏点 7→4）；§5.4（`m_defaultCollapsed` 真实钩子）；§8.3（L1/L2 覆盖度矩阵）；
§9.4（S7 落地形态：私有簿记 `InspectorWidget.h:91-101` 使"重写 `AddGroup`"不可行 → 3 行虚工厂 + 子类 `paintEvent`）；
§6.1/§6.4/§9.9（形态与预览的源码级裁决，多表面证伪；两引擎停顿点同构 `RawTexture.cpp:1017` vs
`rendering_device.cpp:2729` → "按需渲一帧"升格为契约语义）。

### 11.6 本轮规则裁决（2026-08-25，与 `Plan.md` 变更日志同步）

| 议题 | 裁决 | 落点 |
|---|---|---|
| C1 检验手段挡住 Atom-free 的 `AtomToolsFramework.Core` | **只精化 `check_no_atom.ps1` 第 1/2 层，C1 规则原文不动**——规则本就只禁"渲染类模块"且明写"不列白名单"，是代理指标严于规则且严错了对象（拦名字非拦依赖） | §3.6 |
| 是否给"本工具编 Atom 材质"开 target 口子 | **不开**。原版 MaterialEditor 已覆盖，收益≈0；单底线一旦有例外，护栏对其余全部 target 的约束力打折 | §1.3 |
| ~~是否为 Godot `PROPERTY_HINT_GROUP_ENABLE` 破 C3 自建组头 checkbox~~ | **同日推翻**（见下一行）。原裁决"维持降级"是在"这会破 C3"的错误前提下作出的 | §9.4 |
| **C3 本意澄清**：框架/控件满足不了设计需求、且需求不属于过度设计时，**允许扩展框架与自建控件**；C3 反对的是"重复造轮子"而非"扩展" | **写入 `Plan.md` §A6 C3**：三级优先序（① 复用 ＞ ② 扩展 ＞ ③ 自建）+ 判定手段保留牙齿（走到 ②/③ 须落 **(a) stock 为什么不够（带 `文件:行号`）** + **(b) 为什么不是过度设计** 两条证据）+ 单位后缀事件落为反面判例 | `Plan.md` §A6 C3 |
| 澄清后重审组头勾选框 | **由驳回改为采纳（S7）**。(a) stock header 是自绘 `ExtendedLabel` 无 layout；(b) `BaseMaterial3D` 用了 15 次（§11.4-6 复核订正），是 Godot 材质进阶区的主要呈现方式。落地为 ②级扩展：上游 3 行虚工厂 + CEE header 子类 ~100 行。**契约字段 v1 加，控件实现排 P3**（消费者与实现同批） | §9.4 / §4.1 / §10 P3 |
| 澄清后重审贴图槽缩略图 | **维持 v1.5**——但理由从"Thumbnailer 太重"改为**单纯的排期取舍**（不阻塞任何验收尺子）。备选实现（`QPixmap` 直读，②级）记在 §5.4 | §5.4 |
| A0 是否禁止跨引擎材质**离线**转换工具 | **不禁**——`Plan.md` §A0 已补脚注：独立离线工具不进编辑器运行时/契约、不新增第三真相源，不受此限 | §9.1 v2 出口 |
| C4 归一化条款对材质是否适用 | **不适用**——`Plan.md` §A6 C4 已补适用范围：归一化只针对**编辑器自身参与计算的量**（transform/bounds/相机/拾取）。材质值编辑器只显示与路由，归一化无消费者，只会复制 §B4 已知限制的代价 | §8.4③ / §8.5 规则 4 |
| C5 是否补材质面接入成本 | **补**——`Plan.md` §A6 C5 已加一行：材质面 = 8 纯虚 + 2 哨兵 + 1 份 schema，stub 率 0 目标（同日 RTT 预览裁决后由 1 哨兵更新为 2） | §7.6 |
| 材质编辑器形态：独立 exe vs CEE 内 dock 面板 | **dock 面板**（同日先裁决形态后核实渲染路径）。用户先纠正前提——**业界常态是进程内编辑**（Unreal/Unity/Godot/rbfx/Blender 均如此，O3DE 双进程是孤例，Godot `MaterialEditorPlugin : EditorPlugin`、rbfx `MaterialInspectorWidget` 均为面板）；随后核实推翻初稿两条承重理由：双 `AzQtApplication` 不承重（§6.3 本就不继承 `AtomToolsApplication`）、崩溃隔离不成立（引擎已在进程内）。dock 另免费消掉 §6.2 静态库抽取与跨进程同步桥 | §6.1-6.3 |
| 预览像素：多表面 vs RTT 回读 | **RTT 按需回读**（用户拍板）。多表面 = 同时 fork rbfx（4-5 处引擎内部）+ fork Godot（`display_server_windows.cpp:1855`），且**上游两个引擎自己的编辑器都不这么做**；RTT 回读 = 零引擎改动、两引擎均裁决 (a)、上游各两处同款先例。契约新增 `PreviewResult` + `AcquirePreviewImage` **第 2 个哨兵**（`ISceneRenderer`/`IViewportTick`/主循环/四后端表面生命周期零改动），"按需渲一帧、绝不空闲回读"（停顿点两引擎同构：`RawTexture.cpp:1017` / `rendering_device.cpp:2729`）写入契约语义。**⚠️ 2026-08-26 双线复核：结论维持，但本行括号内的"上游都不这么做"论据已降级（4/5 家是无效样本，O3DE 反而用句柄），承重论据已换成引擎能力硬事实——见 §11.8** | §4.1 / §6.4 / §9.9 |
| P5 Filament PoC 的触发条件 | **由"可选 PoC（默认做）"改为"P3 完成后评估触发"**（2026-08-25 用户拍板）。证伪实验的价值 = 给已定稿的契约买保险；P2/P3 落地若契约返工，P5 验的是旧契约。rbfx+godot 过尺子、契约冻住后再花这 0.5 周 | §10 P5 |

### 11.7 opus 独立复核处置（2026-08-25，`review_material_migration.md` 五条 R1–R5 全吸收 + 排期重估）

| 编号 | opus 的发现 | 独立复验 | 处置 |
|---|---|---|---|
| **R1**（阻断） | 初稿 §6.5 把 `DocumentTabWidget` 当 Core 现成件"直用"——**幻影 API**（全仓 0 grep 命中）；标签页簿记实为 `AtomToolsDocumentMainWindow.cpp:289-708` ~420 行 + 私有 `m_tabWidget`（`.h:142`）；ctor 无条件建 FancyDocking + WindowDecorationWrapper + 第二个 AssetBrowser + Python 终端 + log 面板 + metrics（`AtomToolsMainWindow.cpp:36-37,54-67`） | 成立（857 行文件逐段核实） | **采纳 C 案：v1 = 单文档 dock + combo 薄壳（~120 行，§6.5），不搬簿记**。用户拍板。记为**本文初稿自己的错误 ③**（§11.4 第 5/6 条同根因：把"没查"写成"直用"） |
| **R2**（阻断） | 5 个 Core 类（`AtomToolsDocument`/`AtomToolsDocumentSystem`/`DynamicProperty`/`DynamicPropertyGroup`/`InspectorWidget`）的 `Reflect` 唯一调用方 = `AtomToolsFrameworkSystemComponent::Reflect`（`AtomToolsFrameworkSystemComponent.cpp:34-48`），切 `.Static` 后 CEE 无处反射 | 成立 | **S8**：自由函数 `AtomToolsFramework::ReflectCoreTypes`（~10 行，P0 同 PR，§3.2/§10） |
| **R3**（阻断） | `AtomToolsDocument.h:16-19` 在框架头里声明 `namespace AZ::RPI::MaterialUtils` 别名（`:95` 使用）——撞本文第一层护栏（`AZ::RPI` 禁入 Core） | 成立 | **S9**：切分时改名 `AtomToolsFramework::SourceDependencySet`。护栏拦到自己时**改代码不改护栏**（§3.2/§10） |
| **R4**（高） | 旧材质编辑路径必须退役：`IEntityMirror` 15 纯虚含 3 个 `*Resource*` 方法；`ResourcePropertiesPanel`（无 undo，`.cpp:95` 自认）接线于 `EditorMainWindow.cpp:345-352`；AssetBrowser `.mat/.material` 双击 → `OpenResource("Material")`（`CeeAssetBrowserPanel.cpp:314`）；rbfx/godot 后端存量实现（rbfx `RbfxBackend.cpp:1350-1353` 自认"唯一活编辑面是 shader 参数"） | 成立（逐一核实） | **P2 同批退役**（用户拍板）：`IEntityMirror` 15→12、Godot stub 10/15→7/12、净 −3 方法；Animation 入口同 PR 评估。§7.6 / §10 P2 / 尺子 3 |
| **R5**（高） | 交互回读**无上游先例**：rbfx 直接把 GPU 纹理喂 ImGui（`MaterialInspectorWidget.cpp:409-411` `ImageItem(sceneTexture)`，零回读，`:395-396` 渲一帧后延迟关）；Godot 用 `SubViewportContainer`；上游回读先例只有一次性缩略图；`RawTexture.h:157` 明写 "very slow and shouldn't be used in real time"。初稿 §9.9"回读就是两引擎自家做法"口径过宽 | 成立 | **限频三硬规则**（§6.4.4）：`PreviewRefreshIntervalMs` 66~100 ms（10-15 Hz，复用现有 60fps 门，不新增 QTimer）、≤512² clamp、isVisible() 门；Godot 改异步 `texture_get_data_async`（`rendering_device.h:470`，与 `PreviewResult` 三态对齐）。§6.4.2/§9.9 口径修正 + **尺子 6** |
| 事实订正 | `GROUP_ENABLE` = **15** 次（非 14）；`.Static` 里 `3rdParty::Python` 只服务 `EditorPythonConsoleBus` 旧引用（`Code/CMakeLists.txt:30-51` 核实）；`MaterialEditorMainWindow.cpp` 仅 105 行 | `grep -c` = 15 等全部复验成立 | 全文修正（6 处 14→15 等）；§3.5 补 "`3rdParty::Python` 不进 Core" |
| 排期重估 | P0 1.5→**2 周**（+S8/S9）、P1 1.5→**2 周**（+combo 薄壳、主窗口三冲突、限频与 clamp）、总计 6→**8~9 人周** | 采纳 | §0 / §10 已更新；验收尺子 五把 → **六把** |

### 11.8 预览渲染路径双线复核（2026-08-26，`material_editor_research.md` 为唯一记录，两份源稿已删）

触发：用户质疑"五家都用 RTT"并追问"为什么不给引擎一个窗口句柄"。两组独立复核（各自从零 grep 五引擎源码）
后合并，**驳回结论维持，承重论据替换**。处置如下：

| 编号 | 复核发现 | 处置 |
|---|---|---|
| **C1**（事实错误） | "O3DE 预览是主 swapchain 的 scissor 子区域"是错的——它是 `winId()` 取的独立 HWND 上的独立 swapchain（`RenderViewportWidget.cpp:62-66` → `EntityPreviewViewportScene.cpp:33-37,107`）；`SwapChainPass` 恰恰证明有独立交换链。**五引擎实为 4/5 RTT，O3DE 是唯一例外** | §6.4.1 横评整段重写（订正 ①） |
| **C2**（论据错误） | 五引擎横评里有 **4 家是无效样本**：rbfx/Godot/Unreal/Blender 的编辑器 UI 自绘、跑在引擎渲染管线里，根本不存在"外来窗口"可塞句柄——"全行业零判例"是过度解读，且已被 O3DE 推翻 | §6.4.1（订正 ②）+ §9.9 该条降为旁证 |
| **C3**（论据加固） | rbfx 最靠上的阻断点不是"四处硬编码"，是 `RenderSurface.h:43` **唯一构造函数只收 `Texture*`**——Viewport 只有"主链或纹理"两种归宿；`CreateSecondarySwapChain` 收 `SDL_Window*`、唯一调用者 ImGui 且被 `URHO3D_SYSTEMUI=OFF` 编译掉。Godot `:1855` parent 写死 null 复验成立 | §9.9(a) 整段重写（三层阻断 + 对照 O3DE） |
| **C4**（补账） | ① Qt 侧代价三条（airspace 自证两处 / 成本不对称 / 撞 §B3 单循环）；② "只给 Diligent 开第二 swapchain"变体已考虑并驳回（backend-dependent、契约不统一）；③ v1.5 交互预览正解 = 共享 GPU 纹理，不是改走句柄（**同 日 C11 递进：正解降格为终极选项暂不采用，当前正解 = fence 异步回读**） | §9.9(a2)/(c) 新增 |
| **C5**（论据加固） | 多 swapchain 不是"能力闲置"而是"**在用**"（Unreal 浮动 tab / Godot 浮动 dock 默认开 / rbfx ImGui 多视口默认开 / O3DE XR+多视口），但被一致限定在 UI 合成层——浮出窗口里的预览球依然是贴图 | §6.4.1（订正 ③） |
| **C6**（口径松口） | "回读是 Qt 宿主强加的独有成本"过重：**Blender 自绘 UI、无 Qt，材质球照样 RTT + CPU 回读**（`render_preview.cc:1281,1289`），且每次绘制重传纹理（`glutil.cc:68-118`）；缩略图 5/5 全行业 RTT+回读 | §6.4.2 R5 口径松口 + §9.9(b) |
| **C7**（落地风险，P2 必读） | `MaterialInspectorWidget` **不在 CEE 构建里**（`URHO3D_SYSTEMUI=OFF`，`External/CMakeLists.txt:264`）——只能手抄配方不能 `#include`；`SceneRendererToTexture` 在 `Utility/` 无条件编译、可用 | §6.4.5 加落地风险注 |
| **C8**（补判例） | 本方案控制流在 O3DE 自家就有同宿主现成实现：`PreviewRenderer` = `AddToRenderTickOnce` → `AttachmentReadback` → `QImage::Format_RGBA8888` → `RemoveFromRenderTick`；Godot 缩略图"等帧回读"同形 | §6.4.4 末 |
| **C11**（回读性能，2026-08-26 用户追问后裁决） | 回读性能优化三层递进：① Godot `texture_get_data_async` 已零停顿（引擎原生异步 = Blender job 线程的更优版）；② rbfx 升级路径 = Diligent `ReadTexture` + fence 延迟 `Map`（CEE 侧改动，绕开 `WaitForIdle()` 全管线停顿，契约零变化）→ **v1.5 候选**；③ 终极零拷贝 = `QRhiWidget` + D3D11 shared handle（Qt 官方外部渲染通道全在 RHI/Quick 侧，QWidget+QPainter 消费不了外来纹理）——**用户裁决暂不采用**（撞 Plan §B2/§B3、额外技术债；业界同宿主判例 O3DE `PreviewRenderer` 也选读回）。非 QRhiWidget 下 fence + 零拷贝 `QImage` 构造已是性能上界 | §6.4.4 新增升级路径段；§6.4.2 末句补 rbfx 对应物；§9.9(c) 该行重写；§10 v1.5/v2 行改列 fence |
| **C9/C10**（行号失效） | Blender `DNA_space_types.h:834` 实为 `DNA_space_enums.h:1173`；rbfx `RenderTargetView.cpp:137` 未命中（71/73/119/121 成立） | 两处已改；`137` 已删 |
| 承重引用复验 | `RenderSurface.h:43`、`RenderBuffer.cpp:188,223`、`RenderTargetView.cpp` 四处、`RenderPipeline/` 对 `ISwapChain` 零引用、Godot `:1855` vs `:8094/:8291`、CEE `RbfxBackend.cpp:383`/`GodotBackend.cpp:463` —— 全部亲自 grep 复验通过 | §9.9 新 (a) |
| 纪律 | 未 pin 的外部 checkout 上行号会漂——**凡引用外部引擎行号必须同时记 commit**（本次两处失效行号即教训） | 调研文 §1 |

**维持不变的项**：§9.9 驳回结论、§6.4 的 RTT+按需回读+限频方案、§6.4.3 三态哨兵契约、§6.4.5 预览场景配方、§6.1 dock 形态——全部复核后确认成立。

---

## 附录 A：关键证据索引

**O3DE / AtomToolsFramework（复用基座）**
- 属性模型：`Include/AtomToolsFramework/DynamicProperty/DynamicProperty.h:21-48`（Config 全字段）、`DynamicPropertyGroup.h:18-32`
- 属性→控件的全部转发：`Source/DynamicProperty/DynamicProperty.cpp:130-181`（**S5 缺口所在**）、`:157/162`（范围）、`:170-180`（枚举）、`:241-248`（数据变更回调）、`:274-300`（类型校验）、`:357-395`（滑杆升级）
- 文档基类与 undo：`Include/AtomToolsFramework/Document/AtomToolsDocument.h:43-124`（`UndoRedoFunctionPair`）
- 检视器与编辑生命周期映射：`Source/Document/AtomToolsDocumentInspector.cpp:93-119`
- 分组折叠虚钩子：`Include/AtomToolsFramework/Inspector/InspectorWidget.h:86`、`Source/Inspector/InspectorWidget.cpp:249-274`
- 组头扩展点（**S7 依据**）：`Include/AtomToolsFramework/Inspector/InspectorGroupHeaderWidget.h:20-38`（`: AzQtComponents::ExtendedLabel`，自绘无 layout，`paintEvent`/`mousePressEvent` protected virtual）；现成点击分发虚钩子 `InspectorWidget.h:89` `OnHeaderClicked`；私有簿记 `InspectorWidget.h:91-101`（`m_groups` / `GroupWidgetPair`，这是"整个重写 AddGroup"不可行的原因）
- 文件路径控件：`Source/Inspector/PropertyWidgets/PropertyStringBrowseEditCtrl.h:85-109`、`.cpp:203-266`
- 单位后缀 stock 支持（**S6 依据**）：`AzCore/Serialization/EditContextConstants.inl:154`（`Suffix`）；消费点 `AzToolsFramework/UI/PropertyEditor/PropertyDoubleSliderCtrl.cpp:246`、`PropertyDoubleSpinCtrl.cpp:262`、`PropertyIntCtrlCommon.h:218`、`PropertyVectorCtrl.cpp:40`
- 4 处接缝：`Source/Document/AtomToolsDocument.cpp:368`、`Source/DynamicProperty/DynamicProperty.cpp:9,167`、`Source/Window/AtomToolsMainWindow.cpp:9,613-615`、`Source/Util/Util.cpp:9-12,60-62,614`
- 反射唯一入口（**S8 依据**）：`Source/AtomToolsFrameworkSystemComponent.cpp:34-48`——5 个 Core 类的 `Reflect` 唯一调用方（切 `.Static` 后须抽 `ReflectCoreTypes` 自由函数）
- RPI 别名（**S9 依据**）：`Include/AtomToolsFramework/Document/AtomToolsDocument.h:16-19,95`（`AZ::RPI::MaterialUtils` 别名，切 Core 时改名 `SourceDependencySet`）
- 标签页与窗口簿记（**R1 依据**）：`Source/Document/AtomToolsDocumentMainWindow.cpp:289-708`、`Include/AtomToolsFramework/Document/AtomToolsDocumentMainWindow.h:142`、`Source/Window/AtomToolsMainWindow.cpp:36-37,54-67`

**O3DE MaterialEditor（复用范式来源）**
- 文档实现：`Gems/Atom/Tools/MaterialEditor/Code/Source/Document/MaterialDocument.h:26-146`、`.cpp:598-723`（schema→UI）、`:314-355`（undo diff）、`:192-194`（只存 override）

**Atom 材质模型（对照用）**
- `MaterialPropertyValue.h:53-66`、`MaterialPropertyDescriptor.h:32-40,70-87`
- `MaterialTypeSourceData.h:80-129,166-242`、`MaterialSourceData.h:58-118`、`MaterialSourceData.cpp:217-341`（父材质扁平化）
- Functor：`MaterialFunctor.h:211-262,340-349`
- 底表：`Gems/Atom/Feature/Common/Assets/Materials/Types/StandardPBR.materialtype` + `MaterialInputs/*.json`（`SpecularPropertyGroup.json:11`）

**rbfx**（`I:\rbfx`）
- `Source/Urho3D/Graphics/Material.h:74-82,130,161-252,352`、`Material.cpp:205-208`（无反射）、`:324-582`（XML 读写）、`:629-662`（SetShaderParameter）、`:1002-1012`（默认值，`:1007` `DielectricReflectance=0.5f`）、`:1100-1113`（CloneWithDefines）
- 参数常量表：`Source/Urho3D/RenderPipeline/ShaderConsts.h:81-95,114-126`
- 自动生效：`Source/Urho3D/RenderPipeline/BatchStateCache.cpp:63-88`
- 编辑器手写表 + 预览场景：`Source/Urho3D/SystemUI/MaterialInspectorWidget.cpp:85-90,111-193,203-215,388-466,894-1009`
- glTF 映射与 gamma 存色：`Source/Urho3D/Utility/GLTFImporter.cpp:2271-2332,2345-2660,2522`
- 资源根：`Source/Urho3D/Engine/Engine.cpp:1329`；真实材质 `bin/Data/Materials/PBR/Lead.xml`
- **预览 = RTT（§6.4 依据）**：`Utility/SceneRendererToTexture.cpp:47-101`（`SURFACE_UPDATEALWAYS/MANUALUPDATE`、运行时 resize）；编辑器主场景视图 `SystemUI/SceneWidget.cpp:133-140`；材质预览 `MaterialInspectorWidget.cpp:199-200,395-396`（`SetActive(true)` 渲一帧后延迟关——**按需单帧的上游判例**）；**交互零回读（R5 依据）**：`:409-411`（`ImageItem(sceneTexture)` 直接把 GPU 纹理喂 ImGui，无 CPU 回读）；多实例同帧由 `Renderer.cpp:453-462,523-545`（去重键只含 surface+viewport）支撑
- **回读链（§6.4.2 依据）**：`Texture2D.cpp:134-150` → `Texture.cpp:459-495`（仅 RGBA8/BGRA8/BGRX8）→ `RawTexture.cpp:955-1045`（内部自建 staging、**`:1017` `WaitForIdle()` 全管线停顿**，注释 "shouldn't be used in real time" 见 `RawTexture.h:157-158`）
- 回读先例：`Editor/Foundation/SceneViewTab.cpp:1085-1105`（场景页 PNG 预览）、`Editor/Foundation/SceneViewTab/SceneScreenshot.cpp:70-152`
- **多表面证伪（§9.9 依据）**：`RenderAPI/RenderDevice.h:82-84`（`CreateSecondarySwapChain` 公开）、`.cpp:1110-1170`；主链硬编码 `RenderAPI/RenderTargetView.cpp:70-73,119-121,137`、`RenderPipeline/RenderBuffer.cpp:185-189,220-232`；次级链唯一消费者 `SystemUI/ImGuiDiligentRendererEx.cpp:311-339`（ImGui 顶点直画，绕开 RenderPipeline）；`RenderPipeline/` 对 `ISwapChain` 零引用

**Godot**（`I:\godot`）
- 属性表：`scene/resources/material.cpp:3558-3753`；uniform 名映射 `:560-643`；默认值 `:3908-3979`（`:3913` `set_specular(0.5)`）
- 显隐与变体：`:2468-2519`（notify 触发源）、`:2547-2726`（`_validate_property`）、`:656-2066`（`_update_shader`）、`:2102-2112`
- 组头开关（**S7 需求依据**）：`scene/resources/material.cpp` **15 处** `PROPERTY_HINT_GROUP_ENABLE` —— `:3603` emission、`:3613` normal、`:3618` bent_normal、`:3622` rim、`:3628` clearcoat、`:3634` anisotropy、`:3639` ao、`:3646` heightmap、`:3657`/`:3663` subsurf_scatter、`:3670` backlight、`:3675` refraction、`:3681` detail、`:3720` grow、`:3734` proximity_fade；编辑器侧消费 `editor/inspector/editor_inspector.cpp:5044,5069`
- 反射基础设施：`core/object/property_info.h:39-183`（`:41` range 格式、`:82` GROUP_ENABLE）、`core/object/object.cpp:472-564`、`core/object/class_db.cpp:1384-1408`
- ShaderMaterial 反射：`scene/resources/material.cpp:247-357`；`material.h:40`（`RES_BASE_EXTENSION`）
- 序列化：`scene/resources/resource_format_text.cpp:1800-1998`（`:1882-1889` ext_resource、`:1986-1990` 只写非默认）
- 预览与 factor 规范化：`editor/scene/material_editor_plugin.cpp:264-448,473-506`（**§6.4.5 配方**：`:308-316` SubViewport 自有 World3D/透明背景/MSAA_4X、`:321-331` FOV 20° 相机、`:333-340` 双方向光、`:342-363` sphere/box/quad）；**dock 判例**：`material_editor_plugin.h:134`（`MaterialEditorPlugin : EditorPlugin`）、`material_editor_plugin.cpp:468-470`（`add_custom_control(editor)`，主窗口内面板）
- **SubViewport 回读链（§6.4.2 依据）**：`scene/main/viewport.h:573`（`get_texture()`）→ `viewport.cpp:200-206`（`ViewportTexture::get_image`）→ `servers/rendering/rendering_server.h:131`（`texture_2d_get`）→ `renderer_rd/storage_rd/texture_storage.cpp:1888-1898` → `servers/rendering/rendering_device.cpp:2665-2782`（**`:2729` `_flush_and_stall_for_all_frames()`**、`:2666` `ERR_RENDER_THREAD_GUARD_V`、`:2784` async 变体）
- **按需渲一帧（§6.4.2 依据）**：`rendering_server_enums.h:498-504`（`VIEWPORT_UPDATE_ONCE`）；渲完自动转 DISABLED `renderer_viewport.cpp:955-957`
- 回读先例：`editor/inspector/editor_preview_plugins.cpp:339-398`（`EditorMaterialPreviewPlugin::generate`：设材质→等一帧→`texture_2d_get`→`convert(FORMAT_RGBA8)`）；`editor/editor_interface.cpp:161-248`（`make_mesh_previews`）；等待机制 `editor/inspector/editor_resource_preview.cpp:101-138`
- **多表面证伪（§9.9 依据）**：多窗口架构完备——`servers/display/display_server.h:376`（`create_sub_window`）、`platform/windows/display_server_windows.cpp:1855`（**parent 硬编码 `nullptr`，第二个窗口无法嵌外部 HWND**）、`:7181-7290`（主窗口 owner 创建）；每窗口 surface/swapchain `servers/rendering/rendering_context_driver.h:46`、`rendering_device.h:1310`、帧末批量 present `rendering_device.cpp:8177-8246`；单进程单实例 `platform/windows/libgodot_windows.cpp:39`；`--wid` 入口 `main/main.cpp:1983-1992`

**Filament**（`I:\filament`）
- MaterialInputs 全集：`shaders/src/surface_material_inputs.fs:20-130`；C++ 镜像 `libs/filabridge/include/filament/MaterialEnums.h:255-292`
- **无 UI 元数据**：`libs/filabridge/src/MaterialParser.cpp:116-156`；反射接口 `filament/include/filament/Material.h:97-116`
- Ubershader：`libs/gltfio/materials/base.mat.in:1-178`、`libs/gltfio/src/UbershaderProvider.cpp:41-102,163-389`
- 运行时编译（v2 参考）：`libs/gltfio/src/JitShaderProvider.cpp:93-97,384-509,627-628`
- 参数类型：`filament/backend/include/backend/DriverEnums.h:697-717,762-769`

**Blender**（`I:\blender`）
- Principled socket 全表：`source/blender/nodes/shader/nodes/node_shader_bsdf_principled.cc:22-326`（`:167-211` Specular IOR Level 默认 0.5、`:757` OpenPBR ×2）
- 软/硬范围双轨：`source/blender/makesrna/intern/rna_node_socket.cc:537-552`、`RNA_define.hh:483,529`、`rna_access.cc:1634-1644`
- glTF 折叠规则（反节点图硬证）：`scripts/addons_core/io_scene_gltf2/blender/exp/material/search_node_tree.py:452-490`（全文 1169 行）
- Specular ×2：`scripts/addons_core/io_scene_gltf2/blender/exp/material/specular.py:31-60`
- **形态与预览（§6.4.1 依据）**：无独立材质编辑器窗口——shader 编辑器 = 主窗口内 `SPACE_NODE` SpaceType（`makesdna/DNA_space_enums.h:1173`、`editors/space_node/space_node.cc:1717-1724`）；Material Preview = 3D 视口 `OB_MATERIAL` 着色模式（`DNA_object_enums.h:41`、`DNA_view3d_types.h:591-592`、`view3d_draw.cc:1677-1689` 走 EEVEE）；连主 3D 视口都是 region FBO → 全屏 quad 合成主窗口（`wm_draw.cc:748-754,789,1129-1148`、`gpu_viewport.cc:488-542`），无第二窗口/swapchain/CPU 回读
- **多窗口 = 每 wmWindow 独立 GPUContext**（`wm_window.cc:1047-1048` `GPU_context_create` per window、`:1634-1635` 切换、`BKE_wm_runtime.hh:169` 槽位）——多显示器工作流底座，材质预览不用它
- **缩略图 RTT+回读（§6.4.2 同款）**：物体/集合缩略图 `GPU_offscreen_create` → 渲染 → `GPU_offscreen_read_color` → `GPU_offscreen_free`（`view3d_draw.cc:2050-2153`；API 声明 `GPU_framebuffer.hh:686`）；材质缩略图走渲染引擎 `RE_PreviewRender` + `RE_ResultGet32`（`render_preview.cc:1247,1281,1289`）；内置预览场景 Sphere/Cube/Shader Ball（`render_preview.cc:187-205,238-266`）

**Unreal**（`I:\UnrealEngine`，UE 5.8.0，2026-08-25 调研补齐）
- 形态：MaterialEditor = `FAssetEditorToolkit` 的 **MajorTab 停靠面板**（`Editor/MaterialEditor/Private/MaterialEditor.cpp:726`、`Editor/UnrealEd/Private/Toolkits/AssetEditorToolkit.cpp:149-165,222`），默认宿主独立 SWindow、可 dock 回主窗口（`Editor/MainFrame/Private/MainFrameModule.cpp:546` "DockedToolkit" 槽）——同进程 Slate 窗口，非独立 exe
- **预览 = 离屏 RT + Slate 合成（§6.4.1 依据）**：`SViewport` 默认 `bRenderDirectlyToWindow=false`（`Slate/Private/Widgets/SViewport.cpp:29`）→ `FSceneViewport::InitRHI` 建离屏 "BufferedRT"（`SceneViewport.cpp:2399-2462`）→ 场景画入后由 `SViewport::OnPaint` 经 `FSlateDrawElement::MakeViewport` 当贴图合成进窗口 draw list（`SViewport.cpp:167-169`、`SlateCore/Public/Rendering/DrawElementTypes.h:679-686`）；MaterialEditor 模块内无 `RHICreateViewport`/`CreateSwapChain`（仅 Diff 工具自开窗口 `SMaterialDiff.cpp:505-527`，与预览无关）
- **多窗口多 swapchain 原生**：每 SWindow 首次显示 `Renderer->CreateViewport(window)`（`SlateCore/Private/Widgets/SWindow.cpp:1437`）→ `WindowToViewportInfo` 一对一（`SlateRHIRenderer/Private/SlateRHIRenderer.cpp:757`）→ `RHICreateViewport(OSWindow)`（`:328`）→ D3D12 `CreateSwapChainForHwnd`（`D3D12RHI/Private/Windows/WindowsD3D12Viewport.cpp:137-138`）；逐窗口 Draw+Present（`SlateRHIRenderer.cpp:1454-1500`，`:1482` 注释 "D3D12 can't handle more than 8 swap chains"）；**但材质预览不用它**——浮窗内的预览仍是 RT 合成 widget

**CEE 既有**
- 契约风格：`Code/Source/BackendAPI/IAssetSource.h:36-46`、`IEngineBackend.h:31-49`、`IEntityMirror.h:80,156,188-202`、`ISceneRenderer.h:67-81`（**单表面：9 纯虚无表面标识**——预览因此不进此契约，§6.4.3）
- 应用与主循环：`Source/Application/CrossEngineEditorApplication.h:35-37`、`.cpp:123-198,240-266,324-396`（**`:356-371` 单 60fps 节流门、TickRender→backend->Tick 顺序、"One present per backend/frame"**——预览刷新挂这里，不新增时钟，§6.4.4）
- 单表面第二处：`Source/BackendAPI/IViewportTick.h:25-33`（`AZ::Interface` 单注册）；`Source/Viewport/EditorViewportWidget.cpp:249`（`Register(this)`）
- 单表面第三处（后端侧）：`Source/Backends/RbfxBackend.cpp:370-398`（`OnSurfaceCreated` = **启动整个引擎**，`EP_EXTERNAL_WINDOW` 是 init 期参数）、`Source/Backends/GodotBackend.cpp:302-305`（`--wid <HWND>` 实例创建期参数）；`DiligentBackend.h:96`（单一 `m_swapChain`）
- dock 就绪：`Source/Window/EditorMainWindow.cpp:82-86`（ADS dock manager）、`:362-368`（中央视口 dock）、`:410-421`（**通用建 dock 助手**）
- 构建现状（单 app target，**保持不动**）：`Code/CMakeLists.txt:27-28`；rbfx 未 pin 外部 checkout：`External/CMakeLists.txt:234-244,298`；Godot out-of-tree scons：`External/CMakeLists.txt:334`
- 护栏：`Scripts/check_no_atom.ps1`
- 规则与判例：`Plan.md` §A0 / §A6 C1-C7 / §B2 / §B3 / §B4 / §B12；`rbfx_migration.md` §2 / §3.2 / §3.4 / §4；`godot_migration.md` §1 / §3；`Progress.md:44,51`
