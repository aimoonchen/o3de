# 材质编辑器预览上屏方式：五引擎源码复核终稿

> **性质**：源码调研终稿（2026-08-26）。**由两组独立复核合并而成**：两线各自从零 grep、互不引用对方结论，
> 主结论一致、证据互补，三处分歧已逐条裁决（见 §9 附录 B）。
> **本文为唯一记录**——两份源稿已按指示删除，其中的有效信息全部并入本文。
>
> **触发问题**（用户原文）：
> 「你的结论是他们都是采用 RTT 方式进行材质编辑器的渲染？请再次确认（不要臆想，源码中调研）。
> 为什么不是给一个独立的窗口句柄给引擎进行渲染呢？QT 的窗口也是可以获取窗口句柄给引擎渲染的吧。」
>
> **用途**：复核 `material_migration_final.md` §6.4 / §9.9「预览走 RTT + 按需回读，不给第二个 OS 表面」
> 这组裁决的事实基础。§7 的订正清单**已全部实施**于 `material_migration_final.md`
> （落点 §6.4.1/§6.4.2/§6.4.4/§6.4.5/§9.9/§11.8，并修正两处失效行号）。
>
> **纪律**：每条事实带 `文件:行号`，全部由本次 grep/read 实际命中；与终稿冲突的明写「订正」；
> 未命中的明写「未验证」；承重引用由主线**亲自复验**，不采信转述（复验记录见 §9 附录 A）。

---

## §0 TL;DR —— 三问三答

### Q1：五家都用 RTT 吗？——**否，4/5 用 RTT，O3DE 恰恰用窗口句柄**

| 引擎 | 材质预览落点 | 判定 |
|---|---|---|
| rbfx | `SceneRendererToTexture` → `Texture2D` → ImGui `ImageItem` | **RTT** |
| Godot | `SubViewport` → `ViewportTexture` → `SubViewportContainer::draw_texture_rect` | **RTT** |
| Unreal | `SViewport`(默认 `RenderDirectlyToWindow=false`) → `FSceneViewport` "BufferedRT" → Slate `MakeViewport` 合成 | **RTT** |
| Blender | job 线程离屏 `RE_PreviewRender` → `ImBuf`/CPU 像素 → 每次绘制上传纹理画出 | **RTT + CPU 回读** |
| **O3DE** | **`RenderViewportWidget::winId()` → 原生 HWND → `WindowContext` → 独立 `SwapChain`** | **窗口句柄** |

**用户的质疑方向是对的。** 业界唯一一个「Qt 宿主 + 第三方引擎」的样本（O3DE），恰恰把窗口句柄交给了引擎。
「他们都用 RTT」这句话作为 CEE 决策的支撑**不够格**。

### Q2：为什么不给独立窗口句柄？Qt 明明能给

**Qt 能给，CEE 也已经给了——而且两个引擎都只肯收第 1 个。**

- CEE 主视口本来就是纯句柄路径：rbfx `params[EP_EXTERNAL_WINDOW] = nativeWindowHandle`（`RbfxBackend.cpp:383`）、
  Godot `--wid <hwnd>`（`GodotBackend.cpp:463`）。
- 但这两个入口在各自引擎里都是**「主窗口」语义的一次性入口**：
  rbfx `RenderSurface` 的**唯一构造函数只接受 `Texture*`**（`RenderSurface.h:43`）——一个 `Viewport` 只有
  「主交换链 或 一张纹理」两种归宿，没有第三种；Godot 的 `parent_hwnd` 只喂给 `MAIN_WINDOW_ID`
  （`display_server_windows.cpp:8094/:8291`），`create_sub_window` 那条路 parent **写死 `nullptr`**（`:1855`）。

**准确表述：「给引擎窗口句柄」这条路 CEE 已经走满了；材质预览要的是第 2 个表面，而两个引擎在源码层面都收不了第 2 个。**

### Q3：裁决变不变？——**不变，但承重论据必须换**

真正的判据不是「业界惯例」，而是一句可证伪的工程命题：

> **引擎的场景渲染管线，能不能绑定到「一条不是主交换链的交换链」上？**

O3DE **能**（Atom 是自家 RHI，`CreateRenderPipelineForWindow` 是一等公民）→ 所以它直接给句柄，一点都不"违背业界"。
rbfx **不能**（三层阻断，§5.2.1）。Godot **不能**（一行硬编码，§5.2.2）。CEE 的后端是 rbfx/Godot，不是 Atom。

⇒ **§9.9 驳回结论维持；但 (a) 项论据必须从「上游反证 / 全行业零判例」换成「两个后端的渲染目标抽象不支持第二个表面」。**
前者是可被一条反例推翻的经验归纳（且已被 O3DE 推翻），后者是硬事实。

---

## §1 复核方法与 provenance

- **双线独立复核**：两组调研各自从零 grep，互不引用对方结论；本文只收录**至少一线实际命中**的事实。
- **承重引用主线亲自复验**（§9 附录 A 逐条列出），不采信转述。
- **标注约定**：`✅` = 命中且与终稿一致；`订正` = 与终稿冲突且以本文为准；`⚠️` = 存疑或行号失效。

**checkout provenance**（本文所有行号的事实基线）：

| 仓库 | commit | 日期 | 备注 |
|---|---|---|---|
| O3DE（本仓库） | `a82bb0b352` | 2026-08-25 | |
| Godot | `51105ccbe5` | 2026-08-05 | **4.8.dev master，目录已重组**：材质编辑器在 `editor/scene/` 而非旧版 `editor/plugins/` |
| rbfx | `b1c13d4fd` | 2026-07-26 | |
| Blender | `c62ad1de100` | 2026-07-25 | |
| Unreal | `7deeb413d3dc` | 2026-06-17 | |

> **教训**：两稿各自发现了一处终稿的**行号失效**（Blender `DNA_space_types.h:834`、rbfx `RenderTargetView.cpp:137`）。
> 未 pin 的外部 checkout 上，行号会漂。凡引用外部引擎行号，**必须同时记 commit**——否则就是下一个
> `DocumentTabWidget` 式的幻影引用。本表即为此而设。

---

## §2 关键辨析：五个样本里有四个是**无效样本**

用户的问题（「为什么不给句柄」）**只在宿主 UI 不是引擎自己画的时候才成立**。而事实是：

| 引擎 | 编辑器 UI 技术栈 | 「3D 视图」怎么进 UI |
|---|---|---|
| rbfx | **自绘**（Dear ImGui，`SystemUI/`） | 只能是一张 `Texture2D` 喂 `ImageItem` |
| Godot | **自绘**（引擎自身的 `Control` 场景树） | 只能是 `SubViewport` 的 `ViewportTexture` |
| Unreal | **自绘**（Slate，走 `FSlateRHIRenderer`） | 只能是 `FSlateDrawElement::MakeViewport` 的一张 RT |
| Blender | **自绘**（自家 `GPU_` 模块 + `ARegion` 布局） | 只能是 region 的 `GPUOffScreen` |
| **O3DE** | **Qt（外来工具包）** | **两条路都有**：交互视口给 HWND，缩略图走 RTT + 回读 |

前四家**不是「评估了句柄方案后否决了」，而是它们的 UI 本身就跑在引擎的渲染管线里，压根没有"外来窗口"可以塞句柄。**
把它们当作"业界否决了句柄方案"的判例，是**过度解读**。

> **这条辨析是本次复核最重要的产出。** 终稿 §6.4.1 那张「五引擎横评」表把四个自绘 UI 的引擎和一个 Qt 宿主并列，
> 得出「全行业零判例」——**样本里有 4 个是无效样本**（问题在它们身上不成立），
> 而唯一的有效样本给出了**相反**答案。
> ⇒ 结论必须靠 §5.2 的引擎能力事实来撑，**不能靠这张表**。

**但同时要记一条反向事实**（避免论证走到另一个极端）：四家虽是无效样本，它们的**多窗口能力却都在实际使用**，
只是被一致地限定在 UI/窗口合成层——见 §3.6 表末列。这说明「不给预览开第二表面」不是能力缺失，而是**一致的工程选择**。

---

## §3 五家逐一核实

### 3.1 O3DE —— **窗口句柄 + 独立 swapchain**（关键反例，与终稿冲突）

完整链路，全程 Qt 不搬运像素：

```
Gems/Atom/Tools/MaterialEditor/.../MaterialEditorMainWindow.cpp:32
    m_materialViewport = new AtomToolsFramework::EntityPreviewViewportWidget(m_toolId, this);
MaterialEditorMainWindow.cpp:39-40
    auto viewportScene = AZStd::make_shared<AtomToolsFramework::EntityPreviewViewportScene>(
        m_toolId, m_materialViewport, entityContext, "MaterialEditorViewportWidget",
        "passes/mainrenderpipeline.azasset");
MaterialEditorMainWindow.cpp:52-53
    centralWidget()->layout()->addWidget(m_materialViewport);      ← 中央控件，不是 dock
```

`EntityPreviewViewportWidget : RenderViewportWidget`（`EntityPreviewViewportWidget.h:28-29`），
而 `RenderViewportWidget` 干的就是**把 Qt widget 的 HWND 交给渲染器**：

```
AtomToolsFramework/.../RenderViewportWidget.cpp:30-31   RenderViewportWidget(QWidget* parent, ...) : QWidget(parent)
AtomToolsFramework/.../RenderViewportWidget.cpp:39      setUpdatesEnabled(false);              ← 关掉 Qt 对该 widget 的绘制
AtomToolsFramework/.../RenderViewportWidget.cpp:62-63   params.device = RHISystemInterface::Get()->GetDevice();
                                                        params.windowHandle =
                                                            reinterpret_cast<AzFramework::NativeWindowHandle>(winId());
AtomToolsFramework/.../RenderViewportWidget.cpp:65-66   WindowRequestBus::Handler::BusConnect(params.windowHandle);
                                                        m_viewportContext = ...CreateViewportContext(AZ::Name(), params);
```

管线绑到这个窗口的 `WindowContext`（而非某张纹理）：

```
EntityPreviewViewportScene.cpp:33-37   m_windowContext = widget->GetViewportContext()->GetWindowContext();
EntityPreviewViewportScene.cpp:107     renderPipeline =
    AZ::RPI::RenderPipeline::CreateRenderPipelineForWindow(pipelineDesc.value(), *m_windowContext.get());
EntityPreviewViewportScene.h:58,60     AZ::RPI::WindowContextSharedPtr m_windowContext;
                                       AZ::Data::Instance<AZ::RPI::SwapChainPass> m_swapChainPass;
```

落到显卡：`ViewportContext.cpp:22,27` 建 `WindowContext::Initialize` → `WindowContext.cpp:216-232`
`descriptor.m_window = windowHandle; swapChain->Init(...)` → `SwapChain_Windows.cpp:62-63` HWND 直建 DXGI swapchain，
`:102-119` 逐帧 `Present`。

**Qt 不参与绘制的旁证**：`setUpdatesEnabled(false)`；重写的 QWidget 方法只有 `event/enterEvent/leaveEvent/mouseMoveEvent`
（`RenderViewportWidget.h:135-139`），**无 `paintEngine()` 重写**；未设 `WA_PaintOnScreen`/`WA_NativeWindow`/`WA_NoSystemBackground`。

**多 swapchain 在 O3DE 是设计目标，不是禁止项**：
- `WindowContext.h:131-132` `AZStd::vector<SwapChainData> m_swapChainsData;`，`WindowContext.cpp:256-298` 同窗口建 Default + XR left/right 三条。
- `CommandQueue.cpp:200-204` 每帧遍历 `request.m_swapChainsToPresent` 列表 Present，无单例。
- 主编辑器视口同样走 `RenderViewportWidget`（`EditorViewportWidget.cpp:800-803`），`CryEditDoc.cpp:374-378` 按 id 循环多视口。
- 全 RHI 目录搜「only one swapchain / single swapchain」**零命中**。

**O3DE 自己的划线（对 CEE 极有参考价值）**：

| | 交互式预览视口 | 缩略图 |
|---|---|---|
| 路径 | HWND + swapchain | RTT + `AttachmentReadback` → `QImage` |
| 刷新 | 持续 tick | `AddToRenderTickOnce` 单帧 |
| 摆放 | **`centralWidget()`**（`MaterialEditorMainWindow.cpp:52-53`），**不可浮动** | 任意 widget 里的 `QPixmap` |
| dock 位 | 只给纯 Qt 面板（`:26` Inspector / `:56` Viewport Settings） | — |

> **注意最后两行：O3DE 自己也没把原生表面塞进 dock 面板**，原生视口只待在不可浮动的 central widget 里。
> 这与 CEE `Plan.md §B8 B8`「视口锚定不可浮动（是意图）」是同一条工程直觉。

### 3.2 rbfx —— RTT ✅（全编辑器统一 RTT）

**材质预览**（`SystemUI/MaterialInspectorWidget.cpp`）：

```
:197-200   previewScene_(MakeShared<Scene>(context))
         , previewWidget_(MakeShared<SceneRendererToTexture>(previewScene_))
:395-407   previewWidget_->SetActive(true); SetTextureSize(textureSize); Update();
:409-411   Texture2D* sceneTexture = previewWidget_->GetTexture();
           Widgets::ImageItem(sceneTexture, ToImGui(sceneTexture->GetSize()));
```
→ `Widgets.cpp:1234-1239` 即 `ui::Image(ToImTextureID(texture), ...)`。面板挂接 `Foundation/InspectorTab/MaterialInspector.cpp:73,90`。

**不止预览，rbfx 编辑器每一个 3D 视图都是 RTT**：主场景页 `SceneViewTab.cpp:149,1140-1147`；
通用场景控件 `SceneWidget.cpp:70-77,133-141`；运行时游戏页 `GameViewTab.cpp:297-298`；
属性面板内嵌预览钩子 `StandardSerializableHooks.cpp:107,121,146`（同款 `SceneRendererToTexture` → `ImageItem`）。

**RTT 实现**（`Utility/SceneRendererToTexture.cpp`）：

```
:39      , texture_(MakeShared<Texture2D>(context_))
:74-75   texture_->SetSize(..., TextureFormat::TEX_FORMAT_RGBA8_UNORM, TextureFlag::BindRenderTarget);
:76-80   RenderSurface* renderSurface = texture_->GetRenderSurface();
         renderSurface->SetUpdateMode(isActive_ ? SURFACE_UPDATEALWAYS : SURFACE_MANUALUPDATE);
:99-102  renderSurface->SetViewport(0, viewport_);
```

按需渲一帧是**一等公民** ✅：`SetActive(false)` → `SURFACE_MANUALUPDATE`（`:62-67`），`Update()`（`:69`）触发单帧。

> **订正（对终稿 §6.4.2 用词）**：旧枚举 `TEXTURE_RENDERTARGET` 在当前 rbfx 源码中**已不存在**
> （`GraphicsDefs.h` 无此 token），现代 API 是 `TextureFlag::BindRenderTarget`，语义相同。

### 3.3 Godot —— RTT ✅（连主 3D 视口也是）

**材质预览**（`editor/scene/material_editor_plugin.cpp`）：

```
:308-309   vc = memnew(SubViewportContainer);  vc->set_stretch(true);
:312-319   viewport = memnew(SubViewport);
           viewport->set_world_3d(world_3d);          // 独立 World
           viewport->set_disable_input(true);
           viewport->set_transparent_background(true);
           viewport->set_msaa_3d(Viewport::MSAA_4X);
:321-352   camera / light1 / light2 / sphere / box / quad 挂进 viewport
```

挂载点 = **检视器内的自定义控件**：`EditorInspectorPluginMaterial::parse_begin` → `add_custom_control(editor)`（`:461,:470`），
`MaterialEditorPlugin : EditorPlugin`（`material_editor_plugin.h:134`）。成员只有
`SubViewportContainer *vc; SubViewport *viewport;`（`.h:55-56`）——**无 TextureRect、无任何窗口成员**。

**`SubViewport` 就是一张纹理，且不对应任何 OS 窗口**：
`viewport.h:900` `class SubViewport : public Viewport`，而 `window.h:42` `class Window : public Viewport`
——**只有 `Window` 才去 DisplayServer 建窗口**；`viewport.cpp:5570-5576` 构造即 `viewport_create()` + `viewport_get_texture()`；
显示走 `subviewport_container.cpp:135,143,145` `draw_texture_rect(c->get_texture(), ...)`。

**主 3D 编辑视口同构**：`node_3d_editor_viewport.h:269,275` `SubViewportContainer *subviewport_container; SubViewport *viewport;`
（实现 `node_3d_editor_viewport.cpp:6731-6739`）。

按需渲一帧是**一等公民** ✅：`VIEWPORT_UPDATE_ONCE` 渲完自动转 `DISABLED`（`renderer_viewport.cpp:955-957`）。

**「浮出成独立窗口」也不改变这一点**：编辑器的 `WindowWrapper` 建的是引擎自己的 `Window` 节点
（`editor/gui/window_wrapper.cpp:336` `window = memnew(Window)`，`:78/:85` `wrapped_control->reparent(...)`），
把 `Control` 搬进去；里面的 3D 视图仍是 SubViewport 纹理。
**「浮窗」= 引擎自己多开一个窗口，不是把句柄交给外人。**

**无关项澄清**：4.x「嵌入游戏进程」（`embedded_process.cpp:243-245` → `display_server_windows.cpp:3603-3656`
`_find_window_from_process_id` + `SetWindowPos`）是**跨进程原生 HWND 摆放**，属窗口管理，不在渲染路径上。

### 3.4 Unreal —— RTT ✅（Slate 的强制形态）

```
1) SMaterialEditor3DPreviewViewport : public SEditorViewport
   Editor/MaterialEditor/Private/SMaterialEditorViewport.h:36
   挂载：MaterialEditor.cpp:510 RegisterTabSpawner(PreviewTabId, ...)、:1677 SNew(...)、
        :746 SpawnToolkitTab(..., EToolkitTabSpot::Viewport)  → 停靠 Tab，非独立 exe
   （SEditorViewport.h:27-28  class SEditorViewport : public SCompoundWidget）

2) SEditorViewport 建普通 SViewport，未开 RenderDirectlyToWindow
   Editor/UnrealEd/Private/SEditorViewport.cpp:81   SAssignNew(ViewportWidget, SViewport)
   Editor/UnrealEd/Private/SEditorViewport.cpp:99   SceneViewport = FSceneViewport::Create(Client, ViewportWidget);
   Runtime/Slate/Public/Widgets/SViewport.h:37      , _RenderDirectlyToWindow(false)     ← 默认 false
   （SEditorViewport 的 SLATE_BEGIN_ARGS 只有 ViewportSize 一个参数，SEditorViewport.h:31-35
     ——根本没有 RenderDirectlyToWindow 槽位，编辑器视口恒走 B）

3) 于是走独立 RT
   Runtime/Engine/Private/Slate/SceneViewport.cpp:76
       , bUseSeparateRenderTarget( InViewportWidget.IsValid() ? !InViewportWidget->ShouldRenderDirectly() : true )
   Runtime/Engine/Private/Slate/SceneViewport.cpp:2399-2462
       FRHITextureCreateDesc::Create2D(TEXT("BufferedRT")) ... RenderTargetable | ShaderResource

4) Slate 当贴图合成
   Runtime/Slate/Private/Widgets/SViewport.cpp:164-169
       // Only draw a quad if not rendering directly to the backbuffer
       FSlateDrawElement::MakeViewport( OutDrawElements, LayerId, ... );
   SlateCore/Private/Rendering/ElementBatcher.cpp:2709-2739   把视口 RT 当普通纹理合批
```

**「UE 每个窗口一条 swapchain」是真的，但那是给 Slate 用的，不是给预览用的**：
`SlateRHIRenderer.h:140-141` `TMap<const SWindow*, FSlateViewportInfo*> WindowToViewportInfo;`；
`SlateRHIRenderer.cpp:315,328` `GetOSWindowHandle()` → `RHICreateViewport(...)`；`:742` `CreateViewport(SWindow)`；
帧尾逐窗口 draw + present（`:1486/:1500`）。

**全引擎只有两处传 `RenderDirectlyToWindow(true)`**：`GameEngine.cpp:215-222`（独立游戏全屏视口）、
`PlayLevel.cpp:3374-3382`（VR PIE 预览）——**均为「占满整窗口」的视口，且目标仍是该顶层窗口自己的 backbuffer**，
不存在「视口专属子窗口的 swapchain」。

**浮动 tab**：`FDockingDragOperation.cpp:335-363` `SNew(SWindow)` 新建顶层窗口（因而有新 swapchain），
但预览 widget 机制不变，里面仍是 RT 纹理。

### 3.5 Blender —— RTT + CPU 回读 ✅（对 CEE 最贴近的判例）

> **两稿在此处曾有表观冲突，已裁决**：opus 引 `:1204 shader_preview_render`，本线引 `:1281 RE_PreviewRender`——
> 复验证实 **`:1281` 就在 `:1204` 这个函数体内，二者是同一条路径**，非冲突（§9 附录 B 冲突 #1）。
> 但需分清 Blender 有**两条不同的预览路径**：

| 预览对象 | 路径 | 证据 |
|---|---|---|
| **材质 / 着色器 / 灯光 / 世界** | `preview_prepare_scene` 建场景 → `shader_preview_render` → **完整 RenderEngine 离屏渲染**（EEVEE/Cycles）内嵌 `preview.blend` | `render_preview.cc:503`（定义）/`:1235`（调用）→ `:1204`（函数）→ `:1281` `RE_PreviewRender(re, pr_main, sce)` → `pipeline.cc:2663,2684` `RE_engine_render` |
| 纹理（ID_TE） | 同函数内分支 `shader_preview_texture` | `render_preview.cc:1277-1279` |
| **物体 / 集合 / 场景** | `ED_view3d_draw_offscreen_imbuf_simple` → GPU 离屏 + `GPU_offscreen_read_color` → `icon_copy_rect` | `render_preview.cc:916,1034,1096` → `:933,1056,1120` `icon_copy_rect`（定义 `:1385`）；`view3d_draw.cc:2050,2066,2143,2146,2169` |

> ⚠️ **归类提醒**：`icon_copy_rect` 只被上表第三行那条 GPU 离屏路径调用（`:933/1056/1120`），
> **不在材质预览路径上**——材质那条走的是 `RE_ResultGet32`（`:1289`）。两者容易混淆，勿互引。

内嵌预览场景：`render_preview.cc:194` `load_main_from_memory(datatoc_preview_blend, ...)`
（来源 `release/datafiles/preview.blend`，声明 `ED_datafiles.h:17`）。

**结果去向与上屏**：
- Properties 大球留在 `RenderResult`，绘制时 `:698,709,718` `RE_AcquireResultImageViews` → `ED_draw_imbuf`。
- 图标预览 `:1289` `RE_ResultGet32(...)` 拷进 `PreviewImage` 的 **CPU uchar 数组**（`preview_image.cc:264-275`，`:1533` 绑定 `pr_rect`）。
- 上屏：`glutil.cc:68-118` `PixelBitmapDrawer::draw` —— **每次绘制** `GPU_texture_create_2d` 新建纹理、
  `GPU_texture_update` 上传、画三角扇、`GPU_texture_free` 销毁。
- **异步**：整个预览跑在 `WM_jobs_*` 后台 job（`render_preview.cc:2321,2328,2359,2406`）。

**Shader Editor 节点预览同源同路**：`node_shader_preview.cc:590,619,637` → `node_draw.cc:1656` 同样 `ED_draw_imbuf`。

**Blender 连主 UI 都是「每 region 一张 FBO → 合成进窗口」**：
`wm_draw.cc:722,752-753,759`（`GPU_viewport_create` / `GPU_offscreen_create`）、`:782,792` bind、
`:838,841` `GPU_viewport_draw_to_screen` / `GPU_offscreen_draw_to_screen`、`:1148` `wm_draw_region_blit`；
`gpu_viewport.cc:437-477` 实为**绑颜色纹理画四边形 batch**（注释写 "Blit"，实现非 `GPU_framebuffer_blit`）。

**窗口粒度**：`wm_window.cc:1035,1048` 每 `wmWindow` 一个 GHOST OS 窗口 + `GPU_context_create`；
**`createWindow` 全 `source/blender` 唯一调用点就是这里**，对话框也走它。
area/region 隔离靠 `wm_subwindow.cc:25-35` `GPU_viewport` + `GPU_scissor`，**不是原生子窗口**。
**未找到任何 UI 面板拥有独立原生子窗口的证据。**

**Blender 没有「材质编辑器窗口」**：节点编辑器是主窗口内的一个 SpaceType
（**订正**：终稿引的 `DNA_space_types.h:834` 在当前 checkout 未命中，实为 `DNA_space_enums.h:1173` `SPACE_NODE = 16`）；
Material Preview 只是 3D 视口的一个着色模式（`DNA_object_enums.h:41` `OB_MATERIAL = 4`）✅ 与终稿一致。

> **Blender 是对 CEE 最直接的判例**：材质预览球 =「离屏渲染 → 回读成 CPU 像素 → 当图片贴出来」，
> 与 CEE 计划的 `AcquirePreviewImage → QImage → QPainter::drawImage` **完全同构**。
> 差别只在 Blender 把它丢进 job 线程异步做，CEE 用「限频 + 三态哨兵」达成同一目的。

### 3.6 汇总表

| | UI 技术栈 | 材质预览落点 | 主 3D 视图落点 | 多窗口/多 swapchain 能力 | **该能力实际用途** | 预览借用了吗 |
|---|---|---|---|---|---|---|
| rbfx | ImGui（自绘） | RTT `Texture2D` | RTT `Texture2D` | 有（`CreateSecondarySwapChain`） | **ImGui 多视口浮出窗口**（默认 ON）；在 CEE 里被编译掉 | 否 |
| Godot | Control（自绘） | `SubViewport` 纹理 | `SubViewport` 纹理 | 有（每窗口 surface+swapchain） | 浮动 dock（`interface/multi_window/enable` 默认 true） | 否 |
| Unreal | Slate（自绘） | `BufferedRT` | `BufferedRT` | 有（每 `SWindow` 一条 RHI viewport） | tab 拖出成独立 `SWindow` | 否 |
| Blender | 自绘 GPU 模块 | 离屏 + **回读 ImBuf**（job 线程） | region FBO 合成 | 有（每窗口 GPUContext） | 多窗口工作区、对话框 | 否 |
| **O3DE** | **Qt（外来）** | **HWND + swapchain** | HWND + swapchain | 有（Atom RHI 按需建） | 多视口、XR 双眼、**交互式预览本身** | **是** |

**两种读法，缺一不可**：
1. **不是「全行业否决句柄」**，而是「只有 Qt 宿主才会遇到这个选择题，而唯一遇到的那家选了句柄」（§2）。
2. **但四家的多 swapchain 能力都在用**，只是被一致地限定在 UI/窗口合成层——**能力在用 ≠ 预览借用**。
   即便把面板浮出成独立 OS 窗口（rbfx/Godot/Unreal 都支持），**里面的预览球依然是一张贴图**。

---

## §4 缩略图：五家 **5/5** 全是 RTT + 回读（含 O3DE，无一例外）

与「主预览视口」分属两套机制，此结论比 §3 更整齐：

| 引擎 | 缩略图路径 | 证据 |
|---|---|---|
| **O3DE** | `SharedThumbnailRenderer` → `PreviewRenderer`，根 pass `ToolsPipelineRenderToTexture`（**descriptor 无窗口句柄**）→ `FrameCapture` + `AttachmentReadback` → `QPixmap` | `PreviewRenderer.cpp:62-66,80,222-232,244-257,271`；`SharedThumbnailRenderer.cpp:167-208` |
| Unreal | `UMaterialInstanceThumbnailRenderer::Draw(..., FRenderTarget*, FCanvas*, ...)` → `RenderThumbnail` + **`ReadbackThumbnail`** | `MaterialInstanceThumbnailRenderer.cpp:27,110-119`；`ObjectTools.cpp:5837-5848` |
| Godot | `EditorMaterialPreviewPlugin` 纯 RID 128×128 离屏 viewport，`request_and_wait` **等帧** → `texture_2d_get` 回读 → `convert(FORMAT_RGBA8)` | `editor_preview_plugins.cpp:339-357,363-372`；`editor_resource_preview.cpp:101-118,131-134` |
| rbfx | 场景页 PNG 预览 `texture->GetImage()` 回读 | `SceneViewTab.cpp:1085-1088` |
| Blender | 图标预览 `RE_ResultGet32` 回读进 `PreviewImage` CPU 数组；物体缩略图 `GPU_offscreen_read_color` | `render_preview.cc:1289,1533`；`view3d_draw.cc:2143,2146` |

**Godot 的「等帧回读」机制**（与终稿 §6.4.3 三态哨兵同形）：
线程模式 = `frame_pre_draw` 单发连接 + `VIEWPORT_UPDATE_ONCE` + `request_frame_drawn_callback` + 信号量等待；
非线程模式 = `viewport_set_update_mode(UPDATE_ONCE)` + `RS::draw(false)` 同步一帧。至少等 1~2 帧再回读。

**O3DE 的 `PreviewRenderer` 是 `AcquirePreviewImage` 的同宿主判例**：

```
PreviewRenderer.cpp:80      RenderPipeline::CreateRenderPipeline(pipelineDesc)          ← 无窗口
PreviewRenderer.cpp:244-246 RenderToTexturePass::ResizeOutput(size, size)
PreviewRenderer.cpp:249     m_renderPipeline->AddToRenderTickOnce();                    ← 按需渲一帧
PreviewRenderer.cpp:252-257 CapturePassAttachmentWithCallback(..., "Output", ...)
PreviewRenderer.cpp:222-232 [](const AttachmentReadback::ReadbackResult& result) {
                                ... QPixmap::fromImage(QImage(result.m_dataBuffer->data(), w, h,
                                                              QImage::Format_RGBA8888)); }
PreviewRenderer.cpp:271     m_renderPipeline->RemoveFromRenderTick();                   ← 渲完退出 tick
```

> `AddToRenderTickOnce` → `AttachmentReadback` → `QImage::Format_RGBA8888` → `RemoveFromRenderTick`
> —— 这就是 CEE 契约 `AcquirePreviewImage` + `PreviewResult{Unsupported,Unchanged,Updated}` 的**同宗判例，
> 且出自同一个 Qt 宿主**。终稿 §6.4.4「按需渲一帧、绝不空闲回读」的语义，在 O3DE 自家就有现成实现。

---

## §5 CEE 为什么给不出第二个句柄

### 5.1 第一层：这不是 Qt 的限制——CEE 已经在给，而且只能给一次

Qt 完全能交出原生句柄，**CEE 主视口就是纯句柄路径**：

- `EngineViewportWindow`（`QWindow` 子类，`setSurfaceType` 声明 GPU 表面 → Qt 不分配 `QBackingStore`、不建 GL context）
  → `EngineViewportWindow.cpp:65-94` 解析并缓存 `winId()`（Windows=HWND / Wayland=`wl_surface*` / X11=`xcb_window_t`）
  → `EngineViewport.cpp:32-38` `QWidget::createWindowContainer` 嵌回 widget 树。设计已落账 `Plan.md:105-111` §B2。
- 两个后端各自的入口：
  - rbfx：`RbfxBackend.cpp:383` `params[Urho3D::EP_EXTERNAL_WINDOW] = static_cast<void*>(nativeWindowHandle);`
  - Godot：`GodotBackend.cpp:463` `argStrings.push_back("--wid");`

**⇒ 任何以「Qt 拿不到句柄」为由的论证都是错的，不得作为驳回理由。**
真正的问题是这两个入口都是**主窗口语义的一次性入口**，而预览要的是第 2 个。

### 5.2 第二层：引擎侧硬阻断（**承重理由**）

#### 5.2.1 rbfx —— 三层阻断，层层独立

**第一层（最承重）：`Viewport` 的渲染目标类型系统里，压根没有「交换链」这个选项。**

```
Source/Urho3D/Graphics/RenderSurface.h:40   class URHO3D_API RenderSurface : public RefCounted
Source/Urho3D/Graphics/RenderSurface.h:43       RenderSurface(Texture* parentTexture, unsigned slice);   ← 唯一构造函数
Source/Urho3D/Graphics/RenderSurface.h:114      Texture* GetParentTexture() const { return parentTexture_; }
Source/Urho3D/Graphics/RenderSurface.h:143      const WeakPtr<Texture> parentTexture_;                  ← const，无其它来源
```

⇒ **rbfx 里一个 `Viewport` 只有两种归宿：主交换链（`renderTarget_ == nullptr`）或一张纹理。没有第三种。**

**第二层：`renderTarget_` 为空时，回退目标是硬编码的「那一条」主交换链。**

```
RenderPipeline/RenderBuffer.cpp:188   return renderTarget_  ? renderTarget_->GetView()
                                             : RenderTargetView::SwapChainColor(renderDevice_);
RenderPipeline/RenderBuffer.cpp:223   return depthStencil_  ? depthStencil_->GetView()
                                             : RenderTargetView::SwapChainDepthStencil(renderDevice_);
RenderAPI/RenderTargetView.cpp:71     return renderDevice_->GetSwapChain()->GetCurrentBackBufferRTV();
RenderAPI/RenderTargetView.cpp:73     return renderDevice_->GetSwapChain()->GetDepthBufferDSV();
RenderAPI/RenderTargetView.cpp:119    ... renderDevice_->GetSwapChain()->GetDesc().ColorBufferFormat;
RenderAPI/RenderTargetView.cpp:121    ... renderDevice_->GetSwapChain()->GetDesc().DepthBufferFormat;
RenderAPI/RenderDevice.h:137          Diligent::ISwapChain* GetSwapChain() { return swapChain_.RawPtr(); }   ← 无参
RenderAPI/RenderDevice.h:194,201      ea::shared_ptr<SDL_Window> window_;  RefCntAutoPtr<ISwapChain> swapChain_;  ← 各一个，非集合
```

补强：`Source/Urho3D/RenderPipeline/` 目录对 `ISwapChain`/`GetSwapChain` **零命中**——
渲染管线根本不持有 swapchain 概念，一切经 `RenderTargetView`，而后者写死主链。

**第三层：确实有 `CreateSecondarySwapChain`，但它不是给场景管线用的，且在 CEE 里被编译掉。**

```
RenderAPI/RenderDevice.h:82-84
    /// Create swap chain for secondary window. It is not supported for some platforms and backends.
    Diligent::RefCntAutoPtr<Diligent::ISwapChain> CreateSecondarySwapChain(SDL_Window* sdlWindow, bool hasDepthBuffer);
```

三个致命细节：
1. **参数是 `SDL_Window*`，不是 HWND**（`RenderDevice.cpp:1110-1114` 内部 `GetNativeWindow(sdlWindow, ...)`）——
   要用它，得先把 Qt 的 HWND 包成 `SDL_Window`，这本身就是给上游打补丁的前置。
2. **全仓唯一调用者是 ImGui 多视口**：`SystemUI/ImGuiDiligentRendererEx.cpp:338`
   `userData->swapChain_ = renderDevice_->CreateSecondarySwapChain(sdlWindow, false);`
   （`:333` `CreateSwapChainForViewport(ImGuiViewport*)`，`:311-318` 以其 backbuffer RTV 画 ImGui draw data，`:330` `Present(0)`）
   —— 它服务的是 **ImGui 的 UI 层，绕开 `RenderPipeline` 直画顶点**。返回的是裸 `ISwapChain`，
   **没有任何路径能把它变成 `RenderSurface` 交给 `Viewport`**（因第一层）。
3. **这条代码在 CEE 的构建里根本不存在**：`rbfx Source/Urho3D/CMakeLists.txt` 中 `SystemUI` 在
   `if (URHO3D_SYSTEMUI)` 门控内，而 CEE `External/CMakeLists.txt:264`
   `set(URHO3D_SYSTEMUI OFF CACHE BOOL "" FORCE)   # drops bundled ImGui/ImGuizmo`
   ⇒ **第三层阻断在 CEE 里是编译期的。**

> **结论：rbfx 走句柄 = 改 `RenderSurface` 的类型系统 + 改 `RenderTargetView` 四处硬编码 + 造 `SDL_Window` 包装。**
> 这不是「改 4-5 处」，这是**动渲染目标的抽象层**。

#### 5.2.2 Godot —— 一行硬编码，但那一行在 DisplayServer 里

多窗口多 swapchain 架构完备（每窗口一条：`display_server_windows.cpp:1930-1934` 建窗即 `screen_create(window_id)`
→ `rendering_device.cpp:5401-5416` `swap_chain_create(surface)`，`screen_swap_chains` 按 WindowID 1:1）。
阻断点极其具体：

```
platform/windows/display_server_windows.cpp:7181
    Error DisplayServerWindows::_create_window(WindowID p_window_id, WindowMode p_mode, uint32_t p_flags,
        const Rect2i &p_rect, bool p_exclusive, WindowID p_transient_parent,
        HWND p_parent_hwnd,                        ← 有这个参数！
        bool p_no_redirection_bitmap);

platform/windows/display_server_windows.cpp:1846  DisplayServerWindows::create_sub_window(...)
platform/windows/display_server_windows.cpp:1855
    Error err = _create_window(window_id, p_mode, p_flags, p_rect, p_exclusive, p_transient_parent,
                               nullptr,            ← 子窗口的 parent HWND 写死 nullptr
                               no_redirection_bitmap);

platform/windows/display_server_windows.cpp:8094 / :8291   （DisplayServer 构造，主窗口）
    _create_window(DisplayServerEnums::MAIN_WINDOW_ID, ..., parent_hwnd, ...);   ← 只有主窗口能收 --wid
```

⇒ **`--wid` 的宿主 HWND 只能喂给 `MAIN_WINDOW_ID`，CEE 的 `GodotBackend.cpp:463` 用掉的就是这唯一一次机会。**

叠加已知事实：libgodot **无 "render into external surface" API**，只能自建窗口（`Plan.md:124`），
主视口那块已经是靠 `SetParent` reparent 硬塞进来的。
要给预览开第二个表面 = 加公开入口 + 改 `DisplayServerWindows`，而 Godot 在 CEE 里是
**out-of-tree scons 独立构建**，私补丁最难维护。

#### 5.2.3 共同放大项与对照

**放大项**：两者都是**未 pin 的外部 checkout**（`External/CMakeLists.txt:298,334`）。
对未 pin 上游打私补丁，直接顶到 C5（接入成本）与「可插拔后端 = 消费 stock 引擎」的底线。

**对照——O3DE 为什么能**：因为 Atom 就是 O3DE 自己的 RHI，
`RenderPipeline::CreateRenderPipelineForWindow(desc, windowContext)`（`EntityPreviewViewportScene.cpp:107`）
是**一等公民 API**，`WindowContext` 就是「一个 native handle + 一条 swapchain」。它不需要 fork 任何人。

> **「给句柄」不是一个可以脱离引擎能力独立选择的架构风格，它是引擎 RHI 抽象层是否暴露「任意窗口」的直接后果。**

**O3DE 这个反例为什么不可移植到 CEE**——它依赖两个 CEE **都不满足**的前提：

| 前提 | O3DE | CEE |
|---|---|---|
| **面板形态** | 材质编辑器是**独立进程**，预览是 `centralWidget` 的**固定中央控件**（`MaterialEditorMainWindow.cpp:52-53`），不浮动不重停靠 | 2026-08-25 已改判为**主窗口内的 dock 面板**——正是 §5.3 airspace 最脆的场景 |
| **渲染后端** | **Atom**，per-widget swapchain 是一等公民（`CreateRenderPipelineForWindow` 按窗口句柄匹配） | **rbfx / Godot**，两者渲染目标抽象均收不了第 2 个表面（§5.2.1 / §5.2.2） |

补充：**O3DE 自己的缩略图照样是 RTT + 回读**（§4），说明即便在原生支持 per-widget swapchain 的栈上，
「离屏渲染 + 按需取回」依然是预览类需求的正常解。

### 5.3 第三层：就算引擎肯收，Qt 侧原生子窗口还有一层代价

三条，前两条有**源码痕迹**佐证（不是理论）：

1. **原生表面不能与 Qt 绘制混排（airspace）**。原生子窗口永远在 Qt 绘制之上，
   不参与 Qt 的裁剪/半透明/动画/z 序。**规避痕迹**：
   - O3DE 在原生视口上写 `setUpdatesEnabled(false)`（`RenderViewportWidget.cpp:39`）——放弃 Qt 绘制；
   - O3DE 把原生视口放 `centralWidget()`（`MaterialEditorMainWindow.cpp:52-53`）而**不放 dock**，
     dock 位只给纯 Qt 面板（`:26` Inspector / `:56` Viewport Settings）；
   - **CEE 自己已经吃过两次亏**：① `EngineViewport.cpp:41` `setAttribute(Qt::WA_DontCreateNativeAncestors)`，
     注释原文 "otherwise makes docking re-create HWNDs on every drag and stutters"；
     ② `Plan.md §B8 B3` 记着「`ViewportOverlayLabels` 透明 QWidget 叠 native 表面与"native 表面上不叠 Qt widget"
     约束冲突，临时脚手架，Qt 是否提升、文字是否可见未实机验证」。

   而材质预览面板**天生要和 Qt 混排**：ADS auto-hide 抽屉、浮动拖拽、tooltip、
   面板顶部的 `QComboBox` 工具条（终稿 §6.5）——每一样都会撞 airspace。

2. **成本不对称**。主视口 = 1 个 HWND + 1 条 swapchain，换来全屏交互；
   预览球 = 1 个 HWND + 1 条 swapchain（三重缓冲 VRAM）+ 一整套表面生命周期
   （懒建 / DPI 去抖 / 0×0 防御 / 销毁 latch，`Plan.md` §B2 那一整段工程要点要再来一遍），换来一个 256² 的球。

3. **CEE 现有单循环要改**。当前 `TickRender → backend->Tick → FrameMark`（`Plan.md` §B3）是「一帧一次 present」；
   第二个表面意味着第二次 present + 拍频。而 RTT 路径对主循环是**零改动**（终稿 §6.4.3）。

> 这三条本身**不足以**否决句柄方案（O3DE 就扛下了第 1、2 条）。真正否决它的是 §5.2。
> 写清楚是为了：万一将来 CEE 接了一个「能绑任意 swapchain」的后端（如 Filament，或有人给 rbfx/Godot 上了游），
> 这三条是届时**仍需付的账**，别以为 §5.2 解锁了就白送。

### 5.4 决策矩阵

| 判据 | 句柄方案 | RTT 方案 |
|---|---|---|
| rbfx 是否需改上游 | **是**，动 `RenderSurface`/`RenderTargetView` 抽象层 | 否，`SceneRendererToTexture` 现成（`Utility/`，无条件编译 ✅） |
| Godot 是否需改上游 | **是**，`display_server_windows.cpp:1855` + 公开入口 | 否，`SubViewport` + `UPDATE_ONCE` 现成 |
| `ISceneRenderer` / 主循环 / 四后端表面生命周期 | 全部要动 | **一行不动** |
| 与 Qt/ADS dock 混排 | airspace 冲突（§5.3） | 天然兼容（就是一张 `QImage`） |
| 稳态开销 | 第二条 swapchain 常驻 + 第二次 present | 稳态零调用零回读（脏位 + 限频） |
| 交互期开销 | 无回读 | 每 66~100 ms 一次全管线 stall（rbfx 同步）/ 零 stall（Godot 异步） |
| 上游判例 | O3DE ✅（自家 RHI）；rbfx/Godot/UE/Blender ❌（且四家为无效样本，§2） | rbfx ✅ / Godot ✅ / UE ✅ / Blender ✅（含回读）/ **O3DE 缩略图 ✅（同宿主）** |
| **结论** | **驳回**——不是因为业界不做，是因为两个后端收不了第 2 个表面 | **采纳** |

回读那一格的两条硬事实（✅ 与终稿一致）：

- rbfx 同步回读会 stall：`RenderAPI/RawTexture.h:157` 注释原文
  *"Read texture data from GPU. This operation is very slow and shouldn't be used in real time."*，
  实现 `RawTexture.cpp:1017` `immediateContext->WaitForIdle();`
- Godot 有异步回读：`rendering_device.h:470`
  `Error texture_get_data_async(RID p_texture, uint32_t p_layer, const Callable &p_callback);`
  （同步版 `:469` 注释自写 *"GPU textures will most likely force a flush"*；停顿点 `rendering_device.cpp:2729`
  `_flush_and_stall_for_all_frames()`）

⇒ 终稿 §6.4.4「10~15 Hz 限频 + ≤512² clamp + `isVisible()` 门」与 §6.4.3「Godot 走异步」**必须保留**，
它们不是保守设计，是 rbfx 那条同步路径的直接后果。

---

## §6 裁决：终稿结论维持

**`material_migration_final.md` §9.9「预览不开第二个 OS 表面」的驳回结论维持不变。**
§6.4 的「RTT + 按需回读 + 限频（10~15 Hz、≤512² clamp、`isVisible()` 门）」方案**不受影响**。

**但承重论据必须替换**：

| | 旧论据 | 新论据 |
|---|---|---|
| 性质 | 「上游反证：五引擎横评 0/5」——**经验归纳** | 「rbfx `RenderSurface` 只接受 `Texture*`；Godot 子窗口 parent 写死 nullptr」——**硬事实** |
| 可证伪性 | 一条反例即可推翻，**且已被 O3DE 推翻** | 需上游改抽象层才能推翻 |
| 地位 | 主论据 | **降为旁证**（且需附 §2 的样本有效性说明） |

---

## §7 对 `material_migration_final.md` 的订正清单（**已全部实施，落点见末列**）

| # | 位置 | 现状 | 订正 | 性质 |
|---|---|---|---|---|
| **C1** | §6.4.1 横评 O3DE 格 | 「预览是**主 swapchain 的 scissor 子区域**（`EntityPreviewViewportScene.h:60` `SwapChainPass`）」 | **是 `winId()` 取的独立 HWND 上的独立 swapchain**（`RenderViewportWidget.cpp:63`、`EntityPreviewViewportScene.cpp:107`）。`SwapChainPass` 恰恰证明**有**独立交换链，而非共享主链 | **事实错误，须改** |
| **C2** | §6.4.1 表头 / §9.9(a) | 「五引擎横评 **0/5** 用第二 swapchain」「全行业零判例」 | **4/5 RTT，1/5（O3DE）用独立 swapchain**；且另 4 家为**无效样本**（UI 自绘，不存在"外来窗口"这个选项，§2） | **论据错误，须换** |
| **C3** | §9.9(a) 承重理由 | 主论据 = 上游反证 + 全行业零判例 | 主论据改为 **rbfx `RenderSurface.h:43` 唯一构造只收 `Texture*` + `RenderTargetView.cpp:71,73,119,121` 四处硬编码主链 + `RenderPipeline/` 对 `ISwapChain` 零引用；Godot `display_server_windows.cpp:1855` parent 写死 nullptr**；「上游不这么做」降为旁证 | **结论不变，论据加固** |
| **C4** | §9.9 | 只有 (a) fork 成本 + (b) 非过度设计 | **补第三层理由**（§5.3 airspace / 成本不对称 / 第二 present 点，含 `EngineViewport.cpp:41` 与 `Plan.md §B8 B3` 两处自证）；**补一条已考虑并驳回的选项**：Diligent 是自有代码、开第二 swapchain 近乎零成本，但会使预览机制 backend-dependent（Diligent 走 A、rbfx/Godot 走 B），契约不统一、`AcquirePreviewImage` 哨兵形同虚设 ⇒ 驳回 | 加固 |
| **C5** | §6.4.1 「能力存在 ≠ 预览使用」 | 暗示多 swapchain 能力**闲置** | 改为「**能力在用**（Unreal 浮动 tab / Godot 浮动 dock 默认开 / rbfx ImGui 多视口默认开 / O3DE XR + 多视口），**但被一致限定在 UI/窗口合成层，3D 预览一律不借用**」——论据比"闲置"更强 | 加固 |
| **C6** | §6.4.2 R5 口径 | 「回读是 **Qt 宿主强加的 CEE 独有代价**」 | **过重，须松口**：Blender 材质球就是离屏渲染 + CPU 回读 + 每次绘制重上传（`render_preview.cc:1281,1289`；`glutil.cc:68-118`），是出货多年的 DCC 常规做法。CEE 路径非畸形；这反使 §6.4.4 限频显得**保守而非冒险** | **须松口** |
| **C7** | §7.2 rbfx 落地 | 引用 `SystemUI/MaterialInspectorWidget.cpp` 作预览场景配方 | **须补注**：该文件**不在 CEE 构建里**（`External/CMakeLists.txt:264` `URHO3D_SYSTEMUI OFF` × rbfx `CMakeLists.txt` `if (URHO3D_SYSTEMUI)`），只能**手抄配方**，不能 `#include`。`SceneRendererToTexture` 在 `Utility/`（无条件编译）**可用** ✅ | **落地风险，须补注** |
| **C8** | §6.4.4 | 「按需渲一帧、绝不空闲回读」 | 补**同宿主判例**：O3DE `PreviewRenderer` = `AddToRenderTickOnce`(`:249`) → `AttachmentReadback`(`:222`) → `QImage::Format_RGBA8888`(`:232`) → `RemoveFromRenderTick`(`:271`) | 加固，非必须 |
| **C9** | §6.4.1 Blender 引用 | `DNA_space_types.h:834` | 当前 checkout 实为 `DNA_space_enums.h:1173` `SPACE_NODE = 16` | ⚠️ 行号失效 |
| **C10** | §9.9(a) rbfx 引用 | 含 `RenderTargetView.cpp:137` | **删去 `137`**（本次 grep 未在该行命中 `GetSwapChain`），保留 `71,73,119,121` | ⚠️ 行号失效 |
| **C11** | §6.4.2 用词 | `TEXTURE_RENDERTARGET` | 当前 rbfx 已无此 token，改 `TextureFlag::BindRenderTarget`（语义相同） | ⚠️ API 过时 |
| **C12** | §11 裁决记录 | — | 补 2026-08-26 复核记录：指明「都用 RTT」被证伪、**驳回结论维持**、承重论据已替换、承重引用全部复验通过 | 记账 |

**复核后确认成立、不受影响的部分**：§6.4.2 的 rbfx/Godot RTT 对照表全部命中；§6.4.3 三态哨兵设计成立；
§6.4.4 限频三条规则成立**且必要**；§6.4.5 预览场景配方（Godot `sphere/box/quad` 在 `material_editor_plugin.cpp:343-352`）命中；
§6.1 dock 形态的业界证据全部命中（Godot `add_custom_control:470`；Unreal `RegisterTabSpawner(PreviewTabId)` `MaterialEditor.cpp:510`）。

---

## §8 顺带发现（对 P2/P3 落地有实际影响）

1. **rbfx 材质检视器不在 CEE 构建里**（= C7，**本次最有价值的落地发现**）。`SystemUI/` 整个目录被
   `URHO3D_SYSTEMUI=OFF` 编掉。P2 阶段抄 `MaterialInspectorWidget.cpp:197-215, 388-466` 的预览场景配方时，
   是**手抄代码**而非调用。**副作用是好的**：rbfx 侧的 ImGui 第二 swapchain 代码也不会进 CEE
   —— §5.2.1 第三层阻断在 CEE 里是**编译期**的。
2. **Blender 是 CEE 方案的最强判例，终稿没用足**。它是**唯一一家与 CEE 同形态**
   （RTT + CPU 回读 + 非交互式刷新）的判例，比 rbfx/Godot 的「引擎内自合成」更贴近。
   而且它给出了 CEE 可借鉴的缓解手段：**异步 job**（`WM_jobs_*`）。
   CEE 的对应物就是 Godot 的 `texture_get_data_async`（终稿 §6.4.3 已采纳）。
3. **O3DE `PreviewRenderer` 的复用价值**：它深绑 RPI（终稿 §9.8 已判定不引入，**正确**）。
   但它的**控制流形状**（一次性 tick + 回调式回读 + 直接产 `QImage`）可直接作为
   `AcquirePreviewImage` 三态哨兵的设计参照，写进契约注释里作为出处（= C8）。
4. **一个 v1.5 折中，本次不建议但记录在案**：若将来预览需要交互（orbit/dolly，终稿排到 v1.5），
   回读频率上去后 rbfx 那条同步 stall 会变痛。届时的正解**不是**「改走句柄」（§5.2 阻断仍在），
   而是终稿 §6.4.1 末尾已点到的方向 —— **共享 GPU 纹理**（D3D11/12 shared handle → Qt RHI 纹理）：
   零 CPU 回读、零引擎改动、契约形状不变（`AcquirePreviewImage` → `AcquirePreviewTexture`）。
   这与 v2「大尺寸纹理低成本进 Qt」是同一个缺口。**v1 不做，不进契约。**

---

## §9 附录

### 附录 A：承重引用复验记录（主线亲自 grep，不采信转述）

| 引用 | 出处 | 结果 |
|---|---|---|
| rbfx `RenderSurface` 唯一构造只接受 `Texture*` | `Graphics/RenderSurface.h:43` | ✅ 命中，且 `:143` `const WeakPtr<Texture> parentTexture_` 佐证无其它来源 |
| rbfx 回退主链 | `RenderPipeline/RenderBuffer.cpp:188, 223` | ✅ 两处均命中 |
| rbfx 管线只认主链 | `RenderAPI/RenderTargetView.cpp:71,73,119,121` | ✅ 四处全命中，且均为**无参** `GetSwapChain()` |
| rbfx `RenderPipeline/` 对 `ISwapChain` 零引用 | `Source/Urho3D/RenderPipeline/` | ✅ grep 零命中 |
| rbfx `CreateSecondarySwapChain` 收 `SDL_Window*` | `RenderAPI/RenderDevice.h:84` | ✅ 命中 |
| rbfx 该函数全仓唯一调用者 | `SystemUI/ImGuiDiligentRendererEx.cpp:338` | ✅ 全仓 grep 仅此一处 |
| rbfx `SystemUI` 被 CEE 编译掉 | `External/CMakeLists.txt:264` + rbfx `CMakeLists.txt` `if (URHO3D_SYSTEMUI)` | ✅ 命中；同时确认 `Utility` 在无条件编译列表内 |
| Godot `create_sub_window` parent 写死 nullptr | `display_server_windows.cpp:1855` | ✅ 命中；对照 `:8094/:8291` 主窗口传真 `parent_hwnd` |
| CEE 两后端句柄入口 | `RbfxBackend.cpp:383`、`GodotBackend.cpp:463` | ✅ 两处均命中 |
| CEE 主视口句柄路径 | `EngineViewportWindow.cpp:65-94`、`EngineViewport.cpp:32-41`、`DiligentBackend.cpp:62,105,185` | ✅ 命中 |
| Blender 材质预览路径归属 | `render_preview.cc:1204` 函数体内含 `:1281`/`:1289` | ✅ 命中（解决两稿表观冲突，见附录 B #1） |
| rbfx `RenderTargetView.cpp:137` | 终稿旧引用 | ⚠️ **未命中** `GetSwapChain`，建议删（= C10） |

### 附录 B：复核可信度与分歧裁决

**双线独立命中（可信度最高，两组复核各自 grep 得到同一结论）**：
O3DE `RenderViewportWidget.cpp:63` `winId()`、`EntityPreviewViewportScene`/`SwapChainPass`、预览在 `centralWidget` 非 dock；
rbfx `SceneRendererToTexture` + `ImageItem`、`RenderTargetView` 硬编码主链、`CreateSecondarySwapChain` 唯一消费者是 ImGui；
Godot `SubViewport` + `SubViewportContainer`、`create_sub_window` parent nullptr、主 3D 视口同构；
Unreal `SEditorViewport` → `BufferedRT` → `MakeViewport`、每 `SWindow` 一条 swapchain；
Blender 离屏 + 回读 + 每窗口 GPUContext、无面板级原生子窗口；O3DE `PreviewRenderer` 缩略图 RTT + `AttachmentReadback`。

**复核中出现的三处分歧及裁决**：

| # | 分歧 | 裁决 |
|---|---|---|
| 1 | Blender 材质预览的入口：一线引 `render_preview.cc:1204 shader_preview_render`，另一线引 `:1281 RE_PreviewRender` | **非冲突**——`:1281` 就在 `:1204` 函数体内，同一路径。但顺带纠出一处**归类错误**：`ED_view3d_draw_offscreen_imbuf_simple`（`:916/1034/1096`）与 `icon_copy_rect`（`:1385`）属**物体/集合/场景**预览，**不是材质**。§3.5 已按两条路径分表并加警示 |
| 2 | rbfx 阻断的承重点：「改 `RenderTargetView` 四处硬编码」vs「动 `RenderSurface` 抽象层」 | 采纳**动抽象层**：`RenderSurface.h:43` 唯一构造函数只收 `Texture*`，是比四处硬编码**更靠上**的阻断点（复验通过，附录 A）。四处硬编码降为第二层佐证 |
| 3 | 「0/5 用第二 swapchain」是否成立 | **命题成立，但单独表述会误导**：必须同时给出 §2 样本有效性（4 家是无效样本）与 §3.6「能力在用 ≠ 预览借用」。单说 0/5 会被 O3DE 一条反例推翻 |

### 附录 C：证据索引（可复现命令）

```powershell
# rbfx —— RTT 与三层阻断
Select-String -Path "I:\rbfx\Source\Urho3D\SystemUI\MaterialInspectorWidget.cpp"   -Pattern "SceneRendererToTexture|ImageItem|GetTexture"
Select-String -Path "I:\rbfx\Source\Urho3D\Utility\SceneRendererToTexture.cpp"     -Pattern "RenderSurface|SURFACE_|SetSize|BindRenderTarget"
Select-String -Path "I:\rbfx\Source\Urho3D\Graphics\RenderSurface.h"               -Pattern "RenderSurface\(|parentTexture"
Select-String -Path "I:\rbfx\Source\Urho3D\RenderPipeline\RenderBuffer.cpp"        -Pattern "SwapChainColor|SwapChainDepth"
Select-String -Path "I:\rbfx\Source\Urho3D\RenderAPI\RenderTargetView.cpp"         -Pattern "GetSwapChain"
Select-String -Path "I:\rbfx\Source\Urho3D\RenderAPI\RenderDevice.h"               -Pattern "SwapChain|Secondary"
Get-ChildItem "I:\rbfx\Source" -Recurse -Include *.cpp,*.h | Select-String -Pattern "CreateSecondarySwapChain"
Get-ChildItem "I:\rbfx\Source\Urho3D\RenderPipeline" -Recurse | Select-String -Pattern "ISwapChain|GetSwapChain"
Select-String -Path "I:\rbfx\Source\Urho3D\CMakeLists.txt"                         -Pattern "define_engine_source_files|URHO3D_SYSTEMUI"
Select-String -Path "I:\rbfx\Source\Urho3D\RenderAPI\RawTexture.h"                 -Pattern "very slow"

# Godot —— RTT 与阻断
Select-String -Path "I:\godot\editor\scene\material_editor_plugin.cpp"             -Pattern "SubViewport|add_custom_control"
Select-String -Path "I:\godot\scene\gui\subviewport_container.cpp"                 -Pattern "draw_texture_rect"
Select-String -Path "I:\godot\servers\rendering\renderer_viewport.cpp"             -Pattern "UPDATE_ONCE|UPDATE_DISABLED"
Select-String -Path "I:\godot\servers\rendering\rendering_device.h"                -Pattern "texture_get_data"
Select-String -Path "I:\godot\platform\windows\display_server_windows.cpp"         -Pattern "create_sub_window|_create_window|CreateWindowExW"
Select-String -Path "I:\godot\editor\inspector\editor_preview_plugins.cpp"         -Pattern "viewport_create|texture_2d_get|request_and_wait"

# Unreal —— RTT
Select-String -Path "I:\UnrealEngine\Engine\Source\Editor\MaterialEditor\Private\SMaterialEditorViewport.h" -Pattern "public SEditorViewport"
Select-String -Path "I:\UnrealEngine\Engine\Source\Editor\UnrealEd\Private\SEditorViewport.cpp"             -Pattern "SNew\( ?SViewport|FSceneViewport"
Select-String -Path "I:\UnrealEngine\Engine\Source\Runtime\Slate\Public\Widgets\SViewport.h"                -Pattern "RenderDirectlyToWindow"
Select-String -Path "I:\UnrealEngine\Engine\Source\Runtime\Engine\Private\Slate\SceneViewport.cpp"          -Pattern "bUseSeparateRenderTarget|BufferedRT"
Select-String -Path "I:\UnrealEngine\Engine\Source\Runtime\Slate\Private\Widgets\SViewport.cpp"             -Pattern "MakeViewport"
Select-String -Path "I:\UnrealEngine\Engine\Source\Runtime\SlateRHIRenderer\Private\SlateRHIRenderer.cpp"   -Pattern "GetOSWindowHandle|RHICreateViewport"

# Blender —— RTT + 回读（注意两条路径要分开验）
Select-String -Path "I:\blender\source\blender\editors\render\render_preview.cc"    -Pattern "shader_preview_render|RE_PreviewRender|RE_ResultGet32|ED_view3d_draw_offscreen_imbuf_simple|WM_jobs"
Select-String -Path "I:\blender\source\blender\editors\screen\glutil.cc"            -Pattern "GPU_texture_create_2d|GPU_texture_update|GPU_texture_free"
Select-String -Path "I:\blender\source\blender\editors\space_view3d\view3d_draw.cc" -Pattern "GPU_offscreen_create|GPU_offscreen_read_color"
Select-String -Path "I:\blender\source\blender\windowmanager\intern\wm_draw.cc"     -Pattern "GPU_offscreen|wm_draw_region_bind|wm_draw_region_blit"
Select-String -Path "I:\blender\source\blender\windowmanager\intern\wm_window.cc"   -Pattern "GPU_context_create|createWindow"
Get-ChildItem "I:\blender\source\blender\makesdna" -Include *.h -Recurse | Select-String -Pattern "SPACE_NODE = "

# O3DE —— 窗口句柄（关键反例）+ 缩略图 RTT 回读
Select-String -Path "G:\o3de\Gems\Atom\Tools\MaterialEditor\Code\Source\Window\MaterialEditorMainWindow.cpp"                      -Pattern "EntityPreviewViewportWidget|AddDockWidget|centralWidget"
Select-String -Path "G:\o3de\Gems\Atom\Tools\AtomToolsFramework\Code\Source\Viewport\RenderViewportWidget.cpp"                    -Pattern "winId|windowHandle|setUpdatesEnabled"
Select-String -Path "G:\o3de\Gems\Atom\Tools\AtomToolsFramework\Code\Source\EntityPreviewViewport\EntityPreviewViewportScene.cpp" -Pattern "WindowContext|CreateRenderPipelineForWindow"
Select-String -Path "G:\o3de\Gems\Atom\Tools\AtomToolsFramework\Code\Source\PreviewRenderer\PreviewRenderer.cpp"                  -Pattern "AttachmentReadback|QImage|AddToRenderTickOnce|RemoveFromRenderTick"
Select-String -Path "G:\o3de\Gems\Atom\RPI\Code\Source\RPI.Public\WindowContext.cpp"                                              -Pattern "SwapChainDescriptor|m_window|Init"

# CEE 自身 —— 已经在用的句柄路径 + 编译开关
Get-ChildItem "G:\o3de\Gems\CrossEngineEditor\Code\Source\Backends" -Recurse -Include *.cpp | Select-String -Pattern "EP_EXTERNAL_WINDOW|--wid"
Select-String -Path "G:\o3de\Gems\CrossEngineEditor\Code\Source\Viewport\EngineViewport.cpp"       -Pattern "createWindowContainer|WA_DontCreateNativeAncestors"
Select-String -Path "G:\o3de\Gems\CrossEngineEditor\External\CMakeLists.txt"                       -Pattern "URHO3D_SYSTEMUI"
```
