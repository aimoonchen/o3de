# gizmo_polish —— CEE 变换 Gizmo 打磨方案（唯一终稿）

> 定位（2026-08-28 定稿）：本文 = CEE 变换 gizmo 打磨的**唯一可执行方案**。吸收 `gizmo_review.md`（功能级对比，Godot 锚点）与 `gizmo_review_opus.md`（实现级复核，框架层 13 条更正）两份调研稿——两稿已删，本文自包含，所有执行锚点已随条目收录。裁决与驳回记录见 §9。
> 核心诊断（两份调研共同指向）：CEE 的差距不在"缺功能"，在 **①变换没有统一状态机**（每把手一个 lambda 闭包，结构性封死了键盘轴锁/instant/numeric/Esc/多选枢轴）+ **②5 个可复现缺陷**（4 个局部缺陷 + 1 个非均匀缩放行为缺陷）+ **③若干 O3DE 现成能力未接线**。方案按此排布，不铺新摊子。
> 源码基线：CEE = `Gems/CrossEngineEditor/Code/Source`；框架 = `Code/Framework/AzToolsFramework`；Godot = `I:\godot`（锚点已复核）；Blender = `I:\blender`（对齐基准）。

## 1. 执行批次总览

| 批次 | 内容 | 工作量 | 依赖 |
|---|---|---|---|
| P0 缺陷修复 | 5 个缺陷：F1–F4 各自改几行；F5 非均匀缩放（行为缺陷，框架有现成组件通路） | 3.5 人日 | 无，可立即开工 |
| P1 TransformSession 状态机 | 统一变换求解与落盘，解锁全部键盘交互 + 中键切轴 + 多选枢轴 + 拾取修正 | 10.5 人日 | P0 |
| P2 吸附语义 | Ctrl 反转 / Shift 精细 / 读数精度 / 接框架吸附总线 / 落地吸附 | 3 人日 | P1 |
| P3 尺寸模型 | 投影反算替代纯距离模型，正交/FOV/小视口正确，设置接线 | 2 人日 | 无（可与 P1 并行） |
| P4 接线 | 锁定实体变换侧过滤 + 新动作注册 ActionManager | 1.5 人日 | P1 |
| 验收 | §7 九把尺子 | 2 人日 | 全部 |
| **合计** | | **≈22.5 人日** | |

零 CMake 改动；不建测试 target，人工点检（与 editor_polish.md 同一验收模式）。

## 2. P0 —— 缺陷修复（3.5 人日）

### F1 旋转角 360° 回绕（多圈旋转是假的）

现状 `GizmoManager.cpp:655-660`：从四元数反解角度（`2*acos(w)` 值域 [0,2π]，虚部符号翻转），转到 400° 时读数跳成 −320°，ghost 扇形/helpline 一起跳。注释宣称"Blender 支持多圈"，但当前实现产不出 |角度|>360° 的值。

框架已算好无界累加值 `action.m_current.m_deltaRadians`（`AngularManipulator.cpp:109/117` 逐帧累加），CEE 却消费了它的有损产物 `m_delta`。

**修复**：`angle = action.m_current.m_deltaRadians;` 一行替换；`delta` 无条件用 `AZ::Quaternion::CreateFromAxisAngle(axisVec, snappedAngle)` 重建（现有代码只在 snap 分支重建，`:668`）。求解值无界、显示取模——与 Godot `accumulated_rotation_angle`（`viewport.cpp:6529-6531`）同一分层。

### F2 缩放速率随镜头距离漂移

现状 `GizmoManager.cpp:545`：`LocalPositionOffset()` 是世界单位位移，gizmo 屏幕恒定尺寸 → 镜头拉远 10 倍，同样鼠标位移缩放量 ×10。平面缩放 `:599-608` 同病且三分量相加量纲无定义。

框架专为缩放场景设计的 `LocalScaleOffset()`（`LinearManipulator.h:93-96`，由 `m_screenToWorldScale` 归一化，`LinearManipulator.cpp:60/114`）CEE 从未使用（grep 0 命中）。Godot/Blender 都是比值语义：Godot `motion /= click.distance_to(center)`（`viewport.cpp:6348`）；Blender `ResizeBetween = len_v3(d2)/len_v3(d1)`（`transform_mode_resize.cc:43-62`）。

**修复**：两处改用 `LocalScaleOffset()`，且把加法改成乘法比值 `newScale = m_dragStartScale * (1 + offset)`，与 Blender/Godot 对齐。

### F3 Combined 模式拾取冲突（6 个稳定冲突点）

`BuildCombined` 把移动箭头只画头部（1.09–1.415，`GizmoManager.cpp:330-333`），但拾取线段始终"从枢轴到锥尖"（`GizmoViews.cpp:196-199`），恰在半径 1.0 处穿过旋转环 → 三轴×双环 6 个冲突点，点环常抓到轴。Godot 的做法：移动把手拾取体只是箭头头部的一个球（`viewport.cpp:1508-1513`，`GIZMO_ARROW_OFFSET = GIZMO_CIRCLE_SIZE + 0.3 = 1.4`，头整体在环外）。

**修复**：`GizmoViews.cpp:196` 拾取线段起点从枢轴改为 `m_axisLineStart * scale`（Combined 下即 1.09）。一行改动，Move 单模式不受影响（其 `m_axisLineStart` 仍是枢轴）。

### F4 Combined 布局常量硬编码 Blender 值

`GizmoManager.cpp:330-339`（1.09/1.415/0.2/0.775/0.825）与 `:1019`（`stemEnd`）无条件写死，不看 `m_style`。Unreal 主题环带在 1.371–1.6，移动箭头头 1.09→1.415 直接压在环带上。

**修复**：下沉进 `GizmoTheme`（`m_combinedMoveStart/End`、`m_combinedScaleStart/End/BoxCenter`），由 `MakeBlenderTheme()` / `MakeUnrealTheme()` 各自给值。

### F5 非均匀缩放缺失（轴把手 = 均匀缩放的假把手）

三个轴缩放把手与中心把手行为完全一致：全部走 `SetLocalUniformScale`（`GizmoManager.cpp:552` 轴、`:607` 平面）。视觉契约是 Blender 式"轴把手=单轴、中心=均匀"（本项目对齐基准即 Blender），行为却是三把手同一按钮——Godot/Blender/Unreal/Maya 的轴把手全部按轴缩放（Godot `motion_mask` 逐轴掩码，`viewport.cpp:6326-6335`）。注：O3DE 自带 ETCS 默认 gizmo 同样只有均匀缩放（`EditorTransformComponentSelection.cpp:4664`），此行为系从框架继承，不是抄错——但 CEE 的视觉对齐基准是 Blender，且框架自带第一公民通路，无需自建。

**框架通路（已核实）**：`EditorNonUniformScaleComponent`（`ToolsComponents/EditorNonUniformScaleComponent.cpp`）实现 `AZ::NonUniformScaleRequestBus`（`GetScale/SetScale(Vector3)`，`:86/101/106-125`，含钳制与变更信号）；编辑器 TransformComponent 有官方接入路径（`TransformComponent.cpp:1068-1097`）；框架 gizmo 视图已消费该总线（`ManipulatorView.cpp`）。CEE mirror 实体携带标准 O3DE TransformComponent（`EngineNodeComponent.h:11-12`），组件栈天然支持。

**修复**：
1. mirror 创建处挂 `EditorNonUniformScaleComponent`（`EntityMirrorBridge.cpp` 创建点，一行）；
2. 轴把手改写 `NonUniformScaleRequestBus::SetScale`（拖拽起点 Vector3 缩放 × 该轴 F2 比值偏移），平面把手同法乘两轴（Blender S+Shift+X / Godot 双轴 motion_mask 语义），中心把手保留 `SetLocalUniformScale`；
3. undo/redo 沿用每拖拽 ScopedUndoBatch（组件状态在实体捕获范围内）；
4. **开工确认点**：EntityMirrorBridge 取世界矩阵回传后端处，确认折叠了该总线的 Vector3 缩放（`AZ::TransformComponent` 自身不合并——其 `GetLocalScale` 已废弃为 `GetLocalUniformScale`，`TransformComponent.cpp:435-437`）；不折叠则桥接侧按 `GetScale` 显式合成矩阵。Godot/rbfx 后端原生支持 Vector3 scale（`set_global_transform`/`SetWorldTransform`），下游无阻塞。

## 3. P1 —— TransformSession 统一状态机（10 人日）

### 3.1 诊断（为什么这是根）

CEE 五处各写一遍 down/move/up 闭包（`BuildMoveAxes:389` / `BuildMovePlanes:453` / `BuildScaleAxes:517` / `BuildScalePlanes:568` / `BuildRotate:623`），`applyTranslation` lambda 在 `:398` 与 `:458` 逐字复制两份；基准与状态散落在 `GizmoManager.h:249-271` 成员上。`DragKind` 只是绘制标记，不参与求解。

Godot 的答案：一个 `_edit` 结构 + 六函数闭环——gizmo 点选（`_transform_gizmo_select:1487`）与键盘（`begin_transform:6133`）都是**填充同一个 `_edit`**，统一由 `update_transform:6265` / `update_transform_numeric:6564` 求解、`apply_transform:6221` 落盘、`commit_transform:6173` / `cancel_transform:574` 收尾、`finish_transform:6628` 复位。Blender 原版同一模型：gizmo 只是 modal operator 的前端（`transform_gizmo_3d.cc:1798/1807`）。

### 3.2 改造内容

1. **新增 `TransformSession` 结构**（`GizmoManager.h` 旁新文件或同文件）：mode/plane/DragKind、冻结基准（**每实体 originals 表**，替代现有三标量 `m_dragStartTranslation/Orientation/Scale`；**每项为完整 Transform：translation + rotation + Vector3 非均匀 scale**——S3 还原与 F5 都依赖）、选择集快照、snap 参数、undo batch。
2. **六函数骨架**照 Godot 移植：`BeginTransform(mode, instant)` / `ComputeEdit(point)` / `UpdateTransform(pointer)` / `UpdateNumeric()` / `ApplyTransform(motion, snap)` / `Commit() | Cancel()` / `Finish()`。
3. **manipulator 回调退化为适配器**：down → 填 session + ComputeEdit；move → UpdateTransform；up → Commit。**拾取继续复用 O3DE manipulator**（Linear/Planar 与 bound 体系不动，**仅旋转把手按 S6 换为会话级解析式命中判定**），只把求解与落盘抽出来。这是"收敛"不是"重写"。

### 3.3 本轮随改造落地（全部是解锁后的增量）

| # | 能力 | 语义锚点 |
|---|---|---|
| S1 | 拖拽中按 X/Y/Z 锁轴 | Blender 语义。Godot `viewport.cpp:6888-6890` 键位；求解侧约束平面由 session 持有，覆盖 manipulator 原始平面 |
| S2 | Shift+X/Y/Z 平面锁 | 同上 `:6891-6893` |
| S3 | Esc / 右键取消并还原 | 遍历选择集 `set_global_transform(original)`，还原子级基准。Godot `cancel_transform:574`。**只做取消不做出错恢复** |
| S4 | Instant 变换（不碰 gizmo 直接拖）+ 数值输入 | Godot `begin_transform(mode, true)`（`:6146/6167`）左键提交（`:2310-2313`）；numeric 收键 `:2739-2761` → `UpdateNumeric()`（Blender g4.5x）。**键位决策：不绑默认键**——CEE 已占用 Q/W/E/R/T（R=Scale 与 Blender R=Rotate 冲突），按 Godot 的做法注册为未绑默认的 `spatial_editor/instant_*` 等价动作，由 P4 的 ActionManager 重绑能力启用 |
| S5 | 多选共享枢轴 | 枢轴朝向直接复用框架 `CalculateSelectionPivotOrientation`（`ETCS.h:412`）；枢轴位置 = 选择集世界质心（Godot `node_3d_editor_plugin.cpp:190`）；每个实体从**自己的** originals 出发，绕共享枢轴旋转/缩放（Godot `_compute_transform:1834` 语义）。消灭"能选多个、只能动一个"的行为断裂（`GizmoManager.cpp:159-163` `selected.back()` + "pivot averaging is deferred" 注释） |
| S6 | 旋转环拾取修正（原 review B5） | 接管旋转把手命中判定：Godot 解析式判定 + 相机朝向过滤（半球法向过滤 `hit_normal.dot(camera_normal) < 0.05`、半环朝向过滤 `<= 0.005`、半径带宽 1.1±0.1，`viewport.cpp:1577-1609`）。修掉"画的半环、点的一整圈"（`GizmoViews.cpp:654-669` 全 torus bound） |
| S7 | **中键循环切轴** | 拖拽进行中按中键：约束在 自由 → X → Y → Z 间循环（Godot `viewport.cpp:2267-2304` 语义，不进入平面态，平面约束下中键无操作）；每步在拖拽读数里显示当前约束名；若中键被导航占用则导航优先。纯鼠标通道，不依赖键盘焦点（视口裸 QWindow 的键盘桥 editor_polish.md P0-2 若延迟也不阻塞本条） |

**依赖注记**：S1/S2/S3/S4 依赖键盘事件进入视口——裸 QWindow 视口需 editor_polish.md P0-2 的 ShortcutOverride 上行桥（该文档已规划，本表不做重复规划）；S5/S6/S7 为纯鼠标/求解侧改动，不依赖键盘桥。

### 3.4 明确不做（防过度设计，理由见 §9）

参考系 Parent 三态、枢轴模式切换、Influence Group/Individual、轴锁三态循环（全局→局部→取消）。

## 4. P2 —— 吸附语义（3 人日）

| # | 内容 | 锚点 |
|---|---|---|
| P2-1 | **Ctrl 异或临时反转**：`snapEnabled ^ ctrlHeld`，开着按住 Ctrl 临时关、关着按住临时开（Blender 行为） | Godot `node_3d_editor_plugin.h:415` + `:2302`。CEE 侧 `action.m_modifiers` 按下时即可读（`LinearManipulator.h`），无需新通路 |
| P2-2 | **Shift 精细吸附**：位移步长 ÷10、旋转 ÷3、缩放 ÷2 | Godot `node_3d_editor_plugin.cpp:4027-4048` |
| P2-3 | **读数精度随步长**：小数位由吸附步长决定，替代固定 `%.2f`/`%.4f`（`GizmoManager.cpp:858/1044/1050`） | Godot `Math::range_step_decimals`（`viewport.cpp:6269/6536`） |
| P2-4 | **吸附接框架总线**：删除自建 `GizmoSnapSettings` 步长存储，改读 `GridSnapSettings/AngleSnapping/AngleStep(viewportId)`（框架的 Linear/Angular manipulator 内部已在消费同一条总线，`LinearManipulator.cpp:101-108`、`AngularManipulator.cpp:94-108/156-157`）。**同时删除 CEE 自建吸附应用点**（`GizmoManager.cpp:403/461/547-549/602-604/667` 的 `SnapVector/SnapScalar`）——总线接通后 manipulator 内部已吸附，保留则双重吸附/步长叠加不一致。三档步长设置 UI 挂在 editor_polish.md 已规划的 Preferences 面板上，本批只做总线接线 | `ManipulatorSnapping.h:68-75` |
| P2-5 | **Snap-to-floor（PageDown）**：选择集各实体 AABB min 落到地面（网格平面），O(1)，不依赖拾取 | Godot `node_3d_editor_plugin.cpp:3475` |
| — | 顶点吸附 V / 碰撞重定位 Shift+G / SurfaceManipulator：**本批不做**。三者均依赖 mesh 级拾取，根因是后端契约 `RaycastNode` stub（godot_migration.md §3，三角精拾已排 v2）；后端数据通路落地后按本条同批补 | `_vertex_snap_update_source:1072`、`viewport.cpp:2976-2985` |

## 5. P3 —— 尺寸模型（2 人日）

CEE 已确认是屏幕恒定尺寸（`screenSizeFixed=true`，`GizmoViews.h:58`/`GizmoViews.cpp:272`），但乘子 `CalculateScreenToWorldMultiplier` 是**纯距离模型**（`EditorSelectionUtil.cpp:49-59`）：正交投影下完全错误、不看 FOV、不看视口高度。

**修复**（Godot `viewport.cpp:5018-5032` 模型）：
1. 用投影反算"该深度 1 世界单位 = 多少像素"，投影模式无关（正交单独分支）、FOV 自动含入；
2. 小视口钳制（基准高度 400px 以下线性收缩）；
3. 用户设置**直接写** `SetManipulatorViewBaseScale`（`ViewportSettings.h:49-50`，已被乘进每帧绘制，`ManipulatorView.cpp:340-344`，零额外接线），UI 同挂 P2-4 的 Preferences 面板；
4. gizmo 与相机近重合时隐藏整组（防除零，Godot `:5005-5016`）。

## 6. P4 —— 接线（1.5 人日）

| # | 内容 | 说明 |
|---|---|---|
| P4-1 | **锁定实体：变换侧过滤 + UI** | 框架已有 `EditorLockComponent`（`ToolsComponents/EditorLockComponent.h`），且拾取侧过滤**已经生效**（`EditorHelpers` 的 `IsVisibleEntityLocked` 过滤，CEE 点选走的就是这条链路）。缺的只是：TransformSession 遍历选择集时跳过锁定实体 + 工具栏 Lock 开关。Godot 对照 `_is_node_locked:1900` |
| P4-2 | **新动作注册 ActionManager** | S1/S2/S3/S4/P2-5 的新快捷键按 editor_polish.md 的 `cee.action.*` 约定注册，自动获得 `HotKeyManagerInterface` 重绑能力，替代硬编码 QAction |

## 7. 验收尺子（人工点检）

1. **多圈旋转**：拖旋转 2 整圈，读数连续递增过 720°，ghost 扇形/helpline 不回跳（F1）。
2. **缩放手感**：相机距 100m 与 2m 处拖缩放，同一鼠标位移产生的屏幕增量一致（F2）。
3. **Combined 拾取**：三轴×双环 6 个交点处点击，均命中环而非轴；可见半环之外悬停无高亮（F3/S6）。
4. **键盘约束**：拖拽中按 X/Y/Z 锁轴、Shift+X/Y/Z 锁平面；**中键循环 自由→X→Y→Z 且读数显示当前约束**；Esc 与右键均取消并完整还原（S1/S2/S7/S3）。
5. **Instant + numeric**：绑定后 G/R/S 直接拖；数值输入 "2.5" → 位移恰 2.5m，输入期间读数实时显示（S4）。
6. **多选枢轴**：选 2+ 实体绕共享质心旋转/缩放，各实体保持自身形状与相对位置（S5）。
7. **尺寸**：正交与透视视图 gizmo 表观一致；改 FOV 不变；小视口（<400px 高）线性收缩；设置项即时生效（P3）。
8. **吸附**：Ctrl 临时反转、Shift 精细三档生效；读数小数位随步长变化；PageDown 落地（P2）。
9. **非均匀缩放**：拖 X 轴缩放把手仅 X 轴缩放（Y/Z 不变）、平面把手两轴同变、中心把手均匀；镜头距离变化不影响速率；undo/redo 完整还原非均匀值（F5）。

## 8. 跨文档注记（不重复规划，只留指针）

- **框选**：editor_polish.md 已定稿（QPainter marquee + bounds-overlap），本方案不重复规划。补充事实：框架存在现成类 `ViewportSelection/EditorBoxSelect.h`（含 ClickDetector/CursorState/Display2d/三回调），落地时优先按框架类实现或对齐其回调模型。
- **顶点吸附/碰撞重定位**：依赖 godot_migration.md §4 v2 的三角精拾（`RaycastNode`），落地后按 §4 P2 补。

## 9. 处置记录（两稿吸收 / 驳回裁决）

### 9.1 吸收

- **opus §3.1 状态机诊断**：全盘采纳为 P1 根项（GizmoManager 五组 lambda + `GizmoManager.h:249-271` 散落状态已复核属实）。
- **opus §3.2–3.5 四缺陷**：源码复核全部属实（`GizmoManager.cpp:545/655-660/330-339/1019`、`GizmoViews.cpp:196-199/654-669`、框架 `LinearManipulator.h:93`、`AngularManipulator.cpp:117`），全部入 P0。
- **opus §7 框架层 13 条更正**：逐条抽查通过，重点更正吸收：①CEE 已是屏幕恒定尺寸（我原判"世界常数"错）；②旋转多圈原判"平"实为"有回绕缺陷"；③框选/锁定/快捷键重绑/吸附设置/表面吸附/逐组件绘制/顶点子编辑均存在框架现成契约或类，工作量从"实现"降为"接线"。
- **review（我稿）Godot 锚点**：opus §7.1 独立抽查通过，与本轮复核一致；Blender 对齐声明抽查（0.775/1.415、0.2×6=1.2、dial3d 三函数）维持为真。
- **两稿共识**（写入 §5 CEE 领先项并保留事实）：ghost arc/helpline/吸附刻度环/约束线全套严格超越 Godot（Godot 仅一根旋转线 `viewport.cpp:4213-4221`）；双主题；三合一 Combined（Godot 只有移动+旋转）；后端无关渲染；每拖拽一次 undo batch。
- **裁决更新（2026-08-28）**：中键循环切轴由驳回改**采纳**（S7）。理由：用户确认其价值；它是纯鼠标通道不占键位；CEE 视口为裸 QWindow，键盘快捷键依赖 editor_polish.md P0-2 上行桥，鼠标通道可绕开该风险。语义照 Godot `viewport.cpp:2267-2304`：自由→X→Y→Z 循环、不进平面态、导航键位优先。
- **工业级深度复审（2026-08-28，第三轮）**：以四引擎（Godot/Blender/Unreal/Maya）惯例 + O3DE 框架惯例复核全文，结论：方案成立、无过度设计。修正 6 处——①补入漏吸收的 opus 缺陷 **F5 非均匀缩放**（框架第一公民通路已核实：`EditorNonUniformScaleComponent` + `NonUniformScaleRequestBus`，P0 增 1.5 人日）；②P2-4 明确删除 CEE 自建吸附点（5 处，防双重吸附）；③§3.2 澄清 S6 仅换旋转把手命中判定、其余 manipulator bound 不动；④session originals 明确为完整 Transform（含 Vector3 scale）；⑤§3.3 补 S1–S4 依赖 editor_polish.md P0-2 键盘桥、S7 独立的依赖注记；⑥旋转环退化视角列为观察项（缓办）。另核实为真、无需改动：F1 单位（`m_deltaRadians` 无界弧度）、F2 比值公式（`LocalScaleOffset` 无量纲、拖拽起点相对）、F3 安全（`m_axisLineStart` 双主题默认 0.0，仅 Combined 覆盖 1.09，Move 模式零回归）、P4-1 拾取侧锁定过滤已生效（`EditorHelpers.cpp:207` + CEE 点选链路）。

### 9.2 驳回 / 缓办（防过度设计）

| 项 | 处置 | 理由 |
|---|---|---|
| 参考系 Parent 三态 | 驳回 | CEE 与 Godot 同为二态（World/Local），无用例 |
| 枢轴模式 / Influence Group-Individual | 驳回 | Godot 均无此能力，多选共享质心已满足当前需求 |
| 轴锁三态循环（全局→局部→取消） | 驳回 | 状态隐晦，Blender 也不做 |
| 列表选择 / 成组选择 / 三态 gizmo 可见性 / 轴导航控件 / 导航方案可配 / orbit 吸附 / follow / ruler / 富 tooltip / 自适应网格 / 触屏放大 / gizmo 透明度 | 缓办 | 体验级 P2，与本批键盘交互无依赖；轴导航控件与导航方案另属相机系统，宜单独评估 |
| subgizmo / 每节点类型 gizmo 插件 / 渲染快路径（静态几何+实例变换） | 缓办 | 依赖组件端 DisplayEntityViewport 实现与场景规模扩大，条件成熟再上；快路径的天花板判断（immediate 路径不可无限扩）已记录，不否定现有后端无关架构 |
| 多视口 | 缓办 | GizmoManager 单例形状（`GizmoManager.h:96-97`）需改，与视口分屏规划联动 |
| 旋转环退化视角重参数化（Godot `axis_is_orthogonal`，`viewport.cpp:6525-6547`） | 缓办（观察项） | CEE 依赖框架 `CircularRotateThresholdDegrees=80` 换投影平面（`AngularManipulator.cpp:17/37-42`），当前手感可接受；打磨后旋转体验仍差再按 Godot 重参数化 |
| 顶点吸附 / 碰撞重定位 / 表面吸附 | 阻塞 | 根因在后端 `RaycastNode` stub（godot_migration.md §3），非编辑器侧可单独完成 |

## 10. 文档去向

- `gizmo_review.md`、`gizmo_review_opus.md` 已删除，本文为唯一终稿；如需回溯调研细节，本文各条目内嵌的 file:line 锚点即完整线索。
- 本文与 editor_polish.md（编辑器打磨）平行，交叉项见 §8；与 Plan.md 冲突时以 Plan.md 为准。
