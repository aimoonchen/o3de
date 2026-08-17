# rbfx_migration —— 迁移终案与实现记录（AssetBrowser + rbfx 编辑器）

> 定位（2026-08-19 合并定稿）：本文 = CEE 迁移的**唯一终稿**——AssetBrowser 100% 复用接入（原 `AssetBrowersPlan.md` 并入）+ rbfx 编辑器迁移闭环落地（原 `review_rbfx_migration.md` 并入）+ 全案口径收敛（原 `rbfx_editor_migration.md` §0' 并入）+ 四份 review 裁决并入 §4（2026-08-19，`deepseek_review.md` 及其输入稿已删）；只记最终态，每项声称均有 `file:line` 锚点，供后续 review 逐点核验。
> 权威关系：愿景硬规则与路线 = `Plan.md` §A6/§B12；Godot 后端终稿 = `godot_migration.md`（两文档共享 §4 总路线）。
> 总判定（多轮独立 review 一致）：正确、KISS、无过度设计；文档与实现零漂移；控件 100% 复用、契约纯虚强约束、闭环 ≠ 全量。第三轮 review（kimi）源码级复验截获的 2 个 P0 已修复（§3.5：`GetTypeName`+`HasObjectFactory` 与 `SetTemporary`×2）。

## 1. Review 基准（愿景硬规则，Plan.md §A6）

| 规则 | 内容 | 本文证据 |
|---|---|---|
| C1 | Atom 唯一底线 | `Scripts/check_no_atom.ps1` 三层断言（源码/声明/构建链接），每轮构建复验 PASS |
| C3 | 控件 100% 复用，只替换数据源 | §2 官方 AssetBrowser 栈 + §3.3 官方 EntityPropertyEditor / TracePrintFLogPanel |
| C4 | 契约纯虚 = 漏改后端编译失败；契约禁引擎专属语义（空间约定 O3DE Z-up RH 米，转换收口后端边缘） | §3.1 批次 1/2 全纯虚，四后端同变更（Diligent 委托 Null 自动覆盖） |
| C5 | 接入新引擎成本显式化 | 渲染面 3 原语 + 编辑面 19 方法（Godot stub 10/15）+ 资产面 2 枚举 |

**目标口径（2026-08-17 用户澄清，全案唯一基准）**：迁移目标是"rbfx 后端编辑**相对完整、流程闭环**"，不是原版 100% 复刻。收敛判据（三选一即降级/剔除）：

1. O3DE 框架已提供通用能力 → 直接用 O3DE 的，**不复刻 rbfx 专属实现**（闭环够用即可）；
2. 不在场景编辑闭环主干 → 降 v2，不进 MVP；
3. rbfx 专属资源类型工作流 → 用 O3DE 通用反射属性面板兜底，不做逐类型定制编辑器。

据此分层：**L0 闭环 MVP（8-11 人周）+ L1 相对完整（+4-6）= 目标 12-17 人周**；L2 打磨/L3 独立大块（Play-in-Editor、导入管线等）= v2 按需。**L0+L1 已全部落地（§3）**，v2 待办见 §3.6。

## 2. AssetBrowser 100% 复用接入（C3 判例 1，2026-08-17 落地）

### 2.1 终案接线（官方最小复用集，裁剪自 AssetBrowserComponent 接线）

```cpp
auto root = AZStd::make_shared<CeeRootEntry>();             // 子类，见 2.2
BuildEntryTree(root.get(), &backend->GetAssetSource());     // IAssetSource 枚举 → 内存树
auto* model = aznew AssetBrowserModel(parent);              // ctor 公开
model->SetRootEntry(root);                                  // 纯赋值，不自动 reset
auto* filterModel = aznew AssetBrowserFilterModel(parent);  // QSortFilterProxyModel
filterModel->setSourceModel(model);
auto* treeView = new AssetBrowserTreeView(parent);          // setModel 内部断言必须是 filterModel
treeView->setModel(filterModel);                            // 且自动 SetSortMode(Name)
model->BeginReset();                                        // 换根后必须
model->EndReset();
```

构造顺序沿用官方配方（AssetBrowserComponent.cpp:44-59 最小接线）：先 SetRootEntry 再 attach（`index` 对 null root 返回无效索引、不崩溃——`rowCount()` 有 `!m_rootEntry` 防护，AssetBrowserModel.cpp:157-160）；ModelRequests 总线 Single policy（AssetBrowserBus.h:299）→ 全进程唯一 model，刷新只能换根、不能重建。

### 2.2 entry 子类化（friend/protected 门控解法）

官方 `AddChild` 与 `m_name/m_displayName/m_diskSize` 为 protected、`SourceAssetBrowserEntry` 的 `m_extension/m_fileId/m_sourceUuid` 为 private（仅 friend Root）→ 外部代码无权照抄官方构造方式，解法 = 子类化：

| 子类 | 继承 | 关键点 |
|---|---|---|
| `CeeRootEntry` | RootAssetBrowserEntry | 公开 AddEntry；**override `UpdateChildPaths` 置空**（官方 root 会用 rootFullPath/childName 覆写子节点 fullPath，RootAssetBrowserEntry.cpp:497-503，与 CEE 绝对路径语义冲突）；根名 "Assets" |
| `CeeFolderEntry` | FolderAssetBrowserEntry | ctor 设 m_name/m_displayName/m_diskSize + SetFullPath；官方 Folder 不动 fullPath，直接继承 |
| `CeeSourceEntry` | SourceAssetBrowserEntry | 同上；**无需** override `CreateThumbnailKey()`（CEE 图标路径不经过 Thumbnailer，见 2.3） |

必须声明 `AZ_RTTI`（`GetThumbnail` 文件夹分支 `azrtti_cast<FolderAssetBrowserEntry>` 须命中 CeeFolderEntry）；不声明 `Q_OBJECT`（v1 无信号槽，省 moc）。`GetExtension()/GetFileName()` 从 `GetFullPath()` 派生（SourceAssetBrowserEntry.cpp:63-74）→ 公开 `SetFullPath` 写满即可用。

### 2.3 图标：官方扩展点，零 Thumbnailer 依赖

渲染链（EntryDelegate → `AssetBrowserViewUtils::GetThumbnail`，AssetBrowserViewUtils.cpp:683-802）：预览分支（720-738）走 Thumbnailer 体系，CEE v1 的 source 分支（779-794）不经过；文件分支末端走 `AssetBrowserInteractionNotificationBus::GetSourceFileDetails(fullPath)`（AssetBrowserBus.h:270，默认返回空）。实施 = 自注册 `CeeAssetBrowserIconProvider`：

- 返回 `SourceFileDetails(绝对图标路径)`；图标 = 引擎自带 `Assets/Editor/Icons/AssetBrowser/*.svg`（`AZ::Utils::GetEnginePath()` 拼路径，与官方文件夹分支同手段）；扩展名映射按 rbfx 资源为主（mdl→FBX、scene/material/prefab/terrain→XML、lua/as→Lua……），未命中 Default_16；根节点（fullPath 空）给 Folder_80
- **GetPriority()=1 抢答**（树视图自身也是该总线 Handler 但不实现本方法；调度 = `BroadcastResult` + GetPriority 降序 find_if，AssetBrowserViewUtils.cpp:779-794）
- 前瞻（v2）：本 Handler 全局应答所有 source 路径查询；出现多面板或官方其他视图查询前，需按路径前缀过滤或按视图实例分流

### 2.4 为什么不用整组件 AssetBrowserComponent

`Activate` 要求 ThumbnailerService + **AssetProcessor SocketConnection（AZ_Assert 调试断言 + `if (socketConn)` 软退化，AssetBrowserComponent.cpp:83-92）** + 自建 AssetDatabaseConnection + 后台线程——它是资产管线入口本身，与 CEE"预烹饪枚举"语义冲突；独立最小接线才是 C1 与 KISS 正解。AssetBrowser 子系统零 Atom 引用（grep 核实）。

### 2.5 IAssetSource 契约瘦身（review 处置后）

仅 2 枚举纯虚（rbfx 项目目录 QDir 遍历 / Godot 项目目录 + 扩展名过滤）；删 `GetThumbnail` 纯虚与 `AssetEntryInfo::m_extension`（图标定稿后零消费点），头文件不再背 Qt 依赖。

### 2.6 v1 退化面（写死，防后人踩空）

- 仅 TreeView 单列：`ed_useNewAssetBrowserListView=false`（面板 ctor，filter model 创建前）；Path 列 `m_displayPath` 在 CEE 恒空
- 不启用 TableView/Search/Favorites/Previewer（DB 专属，按需再加）
- Refresh = 全量换根（对齐 IAssetSource v1 全量枚举语义），不保留展开/选中态（恢复逻辑 v2 backlog）
- 仅 SourceID/ScanFolderID 列与 `GetSourceUuid()` 退化（CEE 无 DB，无意义）；名称/显示名/路径/Type 列全部正常

### 2.7 生命周期所有权

entry 树裸指针父子，Root 持有整树（析构 RemoveChildren 级联，AssetBrowserEntry.cpp:79-86）；换根 = 构建完整新树 → SetRootEntry → BeginReset/EndReset → 旧根 shared_ptr 释放级联删除。**刷新期间不得持有旧树裸指针**（面板引用与选中态换根前落账）。

### 2.8 实现锚点与验证

- 实现：新增 `CeeAssetBrowserPanel.{h,cpp}`（entry 子类 + IconProvider + 面板组合 + Refresh 按钮）；删 `AssetBrowserPanel.{h,cpp}`（自建 QTreeView，C3 违例物证）；`EditorMainWindow` 双 dock 路径替换 + `ShowAssetBrowser()`；`CrossEngineEditorApplication.cpp` `BrowseForAssets` stub 接线（AssetSelectionModel v1 不消费）
- 构建 ✅ 2026-08-17（-j 8，0 error）；C1 三层断言 PASS（vcxproj 中 3 个 Atom 工程引用均 ReferenceOutputAssembly=false，系 AzToolsFramework 自身 build deps 传递的框架侧既有事实）
- ⏳ 实机清单待人工验收：树显示（名称+图标）/展开收起、D&D mime 携带取回、刷新后旧树无悬空、NullBackend 冒烟

## 3. rbfx 编辑器迁移（L0+L1 落地）

### 3.1 契约（C4：全纯虚，四后端同变更补齐，Diligent 委托 Null）

- **批次 1（P-1）**：七纯虚（`EnumerateObjectTypes/CreatePrefabFromNodes/AssignMaterial/AssignAnimation/SerializeNodes/PasteNodes/RaycastScene`）rbfx 真实现；Godot/Null stub。契约全量 **19 方法 = 15 纯虚 + 4 哨兵默认**——`FinishSync/SaveScene/GetWorldBounds/RaycastNode` 为哨兵默认接缝（默认值仅"不支持"语义，Plan §A6 C4）
- **批次 2（P1）**：`ReadResourceProperties/WriteResourceProperties`（通用属性面板数据源）+ `SaveResource`（面板 Save 落盘：序列化已编辑的缓存资源到源文件）；字节往返对 LoadResource/SaveResource 经 review 确认零消费者后已删除（防契约膨胀）
- **关键事实**：`SyncToEditor` 从不复用实体（总是 aznew）→ `RefreshFromEngine` = GetLooseEditorEntities 清镜像（带 EngineNodeComponent 判定）→ 重 SyncFromEngine；Delete 顺序必须先 `DestroyObject`（引擎节点 Remove，此时编辑器实体仍存活可解析）再 DeleteSelected；rbfx `CreateObject` 故意返回无效 id，靠全量重镜像出实体

### 3.2 实现期 API 修正（源码核实）

- rbfx `Resource` 不继承 `Serializable`（无 GetAttributes）→ 可编辑面 = **Material shader 参数**（GetShaderParameters/SetShaderParameter，即原版 MaterialInspector（InspectorTab/）编辑的同一张自定义参数表）；保留 Serializable 分支作通用兜底（混入该接口的资源类型自动生效）
- rbfx 构建不保证 C++ RTTI → 统一 `Cast<T>`（TypeId + static_cast），不用 dynamic_cast；Material 写回保留参数 `isCustom_` 标记（否则 SetShaderParameter 默认 false 会静默改写自定义参数语义）

### 3.3 通用资源属性面板（C3 判例 2，新文件 `Window/ResourcePropertiesPanel.{h,cpp}`）

- 100% 复用 `EntityPropertyEditor`：宿主 = 分离实体（仅 EngineNodeComponent + 动态 edit-data 网格 = ScriptEditorComponent 模式，零新增控件），仅经 ComponentApplicationBus AddEntity/RemoveEntity 注册 → Outliner/选择/gizmo/undo 全不可见；`SetOverrideEntityIds` 钉住（官方 Inspector 同机制）
- 生命周期：打开 = ReadResourceProperties → SetMirrorData(type, move(bag)) → Init/AddEntity/Activate → 变更总线 connect → 钉 grid；关闭 = 精确逆序（断总线 → 清 grid → Deactivate/RemoveEntity/delete），无悬空引用路径
- 回写：变更总线（按 entity id 寻址）→ 全量 bag WriteResourceProperties（后端按名匹配，未知名/只读项忽略）；全量回写 = KISS（无 undo 记账需求）
- Save：面板 Save 按钮 → 契约 SaveResource → 后端把已编辑的缓存资源序列化到源文件（rbfx: Material::Save 写 XML material 根；路径解析同 SaveScene）；失败弹框提示 + Console 告警
- 数据流：编辑期反射直读直写，不经字节往返；落盘两步 = 编辑进缓存（WriteResourceProperties）→ 序列化缓存（SaveResource）
- palette 头部可见但惰性（HideComponentPalette 为 private，EntityPropertyEditor.hxx:348，v1 外观项）

### 3.4 双击打开 / 拖放赋值 / Console / 关闭

- 双击：`.material/.mat` → Resource Inspector（type "Material"）；`.ani` → 同面板（Animation 无可编辑面 → 面板空 + Console 告警）；`.mdl/.xml` → spawn 原点
- 视口拖放：白名单 `.mdl/.xml/.mat/.material/.ani`（`IsWhitelistedAssetPath`；Godot 资源待后端真实现后并入）；`.mat/.material/.ani` → `AssignAssetToSelection`，失败（无选中/后端失败）Console 告警
- Console = 官方 `TracePrintFLogPanel`（100% 复用，右下 dock）；`closeEvent` → Save/Discard/Cancel（Save 走 OnSaveLevel，Save 弹框被取消则保持打开；v1 无脏标记，Save 恒提供）

### 3.5 正确性验证要点（独立核实，review 通过）

- Material 读写同源：`GetShaderParameters()` 返回裸 `shaderParameters_` 映射（Material.h:272），GetShaderParameter/SetShaderParameter 读写同一张表 → 无"读显写丢"错配；查无此项返回 `Variant::EMPTY`（Material.cpp:941）安全
- `ResolveNode` 两条查无路径均 AZ_Warning 后 nullptr（`RbfxBackend.cpp ResolveNode`）→ DestroyObject 对幽灵镜像/失效实体 no-op 无崩溃
- Create 菜单类型名（kimi 轮 P0 之一）：`Context::GetTypeName(typeId)` 取注册名（Context.h:125；`StringHash::ToString()` 是十六进制哈希码非类型名）+ `HasObjectFactory()` 过滤（ObjectReflection.h:56，对齐原版 CreateComponentMenu.cpp:139-140），类型化创建才能命中工厂产组件
- 注入对象临时标记（kimi 轮 P0 之一）：`DebugRenderer` 对齐原版 SceneViewTab.cpp:1132；`__EditorCamera` 为 CEE 自建注入点（原版相机 = `SceneRendererToTexture` 游离节点，不挂场景树），同用 `SetTemporary(true)` 机制 → SaveXML 跳过临时组件/节点（Node.cpp:421/433），用户场景文件不腐蚀、save→load 循环节点数不增长
- C4/C1 护栏每轮构建复验（§3.7）

### 3.6 v1 已知限制（记录不修，KISS）与 v2 待办

- 已知限制：创建后新对象不自动选中；删除撤销恢复幽灵镜像（再删告警 no-op、重镜像清除；Godot 异步删除幽灵已修=同步 free；Plan §B10 必测项 7）；可编辑资源 = Material（其余类型打开为空+告警）；资源编辑无 undo/redo；New/Open Level 走 O3DE prefab ownership、与引擎场景（SaveScene）正交（v1 不联动）；AssignAnimation 只赋值不播放（编辑器内动画不播放 → bounds 缓存稳定，Plan §B5b；动画/引擎侧自变几何 = 已知滞后，与 v2 增量同步一并解决）；全量重镜像使镜像实体 undo 历史失效（撤销早先编辑落在死 id 上 = no-op 告警非崩溃）
- v1.5 待办：undo 命令化 + 镜像身份稳定化（§4）
- v2 待办：新建材质、palette 隐藏、资源编辑 undo/redo、预览/截图/RenderPath 编辑、Play-in-Editor、Godot 导入管线进程内、IAssetSource 升 SQLite、icon provider 作用域分流、Refresh 展开态恢复

### 3.7 护栏与构建记录

- `check_no_atom.ps1` 每轮 PASS；构建四轮 ✅（2026-08-17 20:13/21:20 实施轮 + 2026-08-18 review 修复轮 + 2026-08-18 kimi/opus 采纳轮，-j 8，0 error；日志 build_assetbrowser*.log / build_p1*.log / build_p1_kimi_opus.log）
- 构建铁律：`-j 8`（-j 14 会 MSBuild OOM）；clangd 诊断是 include-path 噪音，构建为准

## 4. review 裁决与路线（2026-08-19 四份 review 终稿；2026-08-20 双方案评审并入）

> 四份 review 总判定：架构正确、KISS、无过度设计，无推翻项；v1 唯一语义缺陷 = undo 断裂与镜像身份湮灭（同根因两半）。以下仅保留路线裁决与验收尺子，论证过程随原稿删除。
> 2026-08-20 双方案评审（`cross_engine_editor_plan_deepseek_1.md` / `cross_engine_editor_plan_opus5.md`）：两稿架构主体与已落地实现收敛（deepseek_1 的"镜像投影/契约空间/确定性身份/10 类型值袋/哨兵默认"= 现有实现的平行推导；opus5 的 5 项增量提案 E1–E5 为现实缺失项）。裁决：吸收 E1–E5（并入下文章节）；deepseek_1 的 Godot 进程外 IPC、EngineProfile JSON 配置层、"数据面存引擎原生值"三点驳回（理由见"已驳回/降级"）；两稿吸收完毕已删除。Godot 后端终稿 = `godot_migration.md`。

- **v1.5（验收驱动，同批）**：① undo 命令化——`EngineCommand : URSequencePoint`（唯一新增概念，**零新增契约方法**；命令对象直接持 AZ::EntityId + 旧/新值，进程内无需任何中间表示）：

  | 操作 | 正操作（现有契约方法） | 逆操作（现有契约方法） |
  |---|---|---|
  | Create / Paste / Duplicate | `CreateObject(spec)` | `DestroyObject(entityId)` |
  | Delete | `SerializeNodes(ids)` 存 blob → `DestroyObject` | `PasteNodes(blob, parentId)` |
  | SetProperty | `OnEditorPropertyChanged(id)` | 同上（回放旧 `PropertyBag`） |
  | SetTransform | `OnEditorTransformChanged(id, newXf)` | 同上（回放 oldXf） |
  | Reparent | `TransformBus::SetParent` + 引擎侧同步 | 反向 |

  ② 镜像身份稳定化——EntityId 由引擎 handle 确定性派生 + 差集式重镜像（新增建/消失删/存留原地更新）→ undo 记录、选中、展开态存活，幽灵镜像自动清除。验收 = **10 次 Ctrl+Z 回到初始状态**；身份稳定化另消除每次创建 O(N·P) 全量重建。
- **v1.6（低成本高收益，可与 v1.5 并行）**：① E3 属性元信息增量——`EngineProperty` 基类增 4 可选字段：`m_min/m_max/m_step`（rbfx `P_APPLICATION_MIN/MAX` / Godot `PROPERTY_HINT_RANGE`；Min+Max → O3DE 官方 Slider handler 自动生效，`RebuildEditData` 追加 attribute，**零新控件零新 handler**）、`m_description`（tooltip；归还被 `m_category` 占用的语义，`EngineNodeComponent.cpp:118`）、`m_resourceTypeFilter`（资源选择器过滤）。② E2 第一步契约阀门——`IEngineBackend` 增**最后一个**方法 `InvokeCustom(command, PropertyBag& args, PropertyBag& result)`（默认 false = 不支持，符合 C4 哨兵语义）；从此引擎专属功能一律走此路，`IEntityMirror` 冻结。验收：契约方法数不再增长（CI grep 断言纯虚计数）。
- **v2 首选（L2）**：Open Scene——Save 走引擎（`SaveScene`）、Open 目前走 prefab 不 round-trip；rbfx = 清场景 + `LoadFile` + 重镜像（`RbfxBackend.cpp OnSurfaceCreated`），Godot = ResourceLoader 载 PackedScene 换根。PIE 现成范式（v2 需要时）：rbfx 原版 `SimulateSceneAction` = `PackedSceneData` 全量存档 → 恢复场景更新 → 停止还原（`Editor/Foundation/SceneViewTab.cpp:1327-1368`，可撤销），直接支撑 `ISimulation::Play/Stop` 实现。
- **可缓（第三后端接入前为 E2 第二步准入线）**：E2 第二步契约瘦身——`AssignMaterial/AssignAnimation/CreatePrefabFromNodes/SaveResource/Read|WriteResourceProperties` 迁回 `InvokeCustom`，`IEntityMirror` 回落 9–10 纯虚（基线：Godot stub **10/15**，`GodotBackend.cpp:1306-1378`；瘦身后 stub 尺子目标 **≤3**）；`BackendCaps`（硬约束字段 ≤10：engineName/engineVersion/supportsPreciseRaycast/supportsResourceEditing/supportsPrefab/supportsSaveScene/sceneExtensions/assetExtensions——`m_assetExtensions` 顺带消灭 shell 4 处 rbfx 扩展名硬编码，吸收原"ClassifyAsset 下沉"项；能力位以引擎数据承载，替代 deepseek_1 的 EngineProfile JSON）；E5 第三后端 PoC 契约证伪——`DiligentBackend` 补齐镜像面（渲染已真、镜像委托 Null），验收 = 最小集（`Initialize/Shutdown/Tick` + 表面 3 + overlay 3 + `SyncToEditor/OnEditorTransformChanged/OnEditorPropertyChanged/CreateObject/DestroyObject/GetWorldBounds` + 枚举 2）之外**零实现**，做不到即回炉 E2；Diligent 默认 OFF（一行 CMake；rbfx 默认 ON 时本就编译期互斥不编译它）。
- **已驳回/降级（防过度设计先例）**：dynamic_cast 能力探测、ViewportId 预留、契约拆分 Plan A、v1 语义中立化；ActionManager 全量接入与属性懒加载暂不做（仅 §B2 快捷键实测出问题时接 `HotKeyWidgetRegistrationInterface` 一小块）。deepseek_1 三点：**Godot 进程外 IPC**（libgodot 同进程已实测跑通，见 `godot_migration.md` §1）、**EngineProfile JSON 配置层**（按引擎裁剪面板集/布局 = 无消费者即 YAGNI，能力位需求由 BackendCaps 承载）、**数据面存引擎原生值**（违反 C4，坐标转换从单点散布到各消费者）。**防复发清单（从零设计已证伪路径，勿重推）**：跨进程/IPC、自建数据层（SceneDocument/自写树模型/DPE 适配 ≈1900 行重复轮子）、输入上行 + 1 帧延迟、TypeRegistry + 磁盘缓存（进程内可随时直查引擎反射）、Edit POD + Kind 枚举、多 Gem 拆分、ImmediateMesh 逐顶点、octree/物理拾取。
- **两把验收尺子**（"跨引擎"从愿景变事实）："接 Unreal 要 stub 几个方法"（Plan §A6 C5 口径；基线 10/15 → 瘦身后 ≤3）、"10 个操作后 10 次 Ctrl+Z 能否回到初始状态"（本 §v1.5）。
- 裁决中的 P0 已随本轮落地并入正文：§2.1/2.3/2.4 锚点与软退化、§3.1 哨兵默认接缝（19 方法）、§3.5 kimi P0 标注、§3.6 动画不播放与 replaceWithRef 删除；Godot 同步 free 与 transform 抑制位已入代码。

## 5. 待人工验收与悬空项

| 项 | 状态 |
|---|---|
| AssetBrowser 实机清单（§2.8） | ⏳ 待人工 |
| Plan §B10 实测验收 1-7（点选全链路/回写/gizmo/快捷键/幽灵镜像等） | ⏳ 待人工 |
| A 修复回归 | ⏳ Create 菜单显示真实类型名（StaticModel/Light/...）；类型化创建后 Inspector 出现对应组件属性 |
| B 修复回归 | ⏳ 编辑→Save→重启→重载：场景 XML 无 __EditorCamera/DebugRenderer 条目、节点数不增长 |
| C 处置后 | ⏳ Resource Inspector 改材质参数→Save→重启→重开：参数保持 |
| 资源面板首验 | ⏳ 双击 .material 出网格、编辑即视口生效、.ani 打开为空+Console 有告警 |
| 撤销边界 | ⏳ 任意 create/delete 后再撤销早先的变换编辑：死 id 撤销是 no-op 告警而非崩溃 |
