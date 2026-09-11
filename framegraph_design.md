# 基于 DiligentEngine 的现代 FrameGraph 方案（FG）

- 日期：2026-09-11
- 语言基线：**C++23**（concepts、`std::span`、designated initializers、`std::to_underlying`、`std::pmr`）
- 源码基线（本文所有 `file:line` 均在本地 checkout 亲核）：
  - Diligent：`I:\DiligentEngine`（DiligentCore master / v2.5.6 系）
  - filament：`I:\filament`（`filament/src/fg`）
  - O3DE Atom：`G:\o3de\Gems\Atom`

---

## 0. 摘要：最终形态一页纸

1. **API 取 filament**：`add_pass<Data>(name, setup, execute)`，setup 引用捕获、execute 值捕获；`Id<T>` = index(16)+version(16) 强类型句柄；write 产生新版本（SSA），旧句柄失效。
2. **调度取 filament**：黑盒引用计数剔除 + `stable_partition` 保序，**声明序即执行序，不做拓扑排序、不做 pass 重排**。
3. **状态/屏障取 O3DE 思想、按 Diligent 重写**：compile 期状态模拟，产出每 pass **一批** `StateTransitionDesc`，execute 期一次 `TransitionResourceStates`。FG 内所有命令用 `MODE_NONE`（Debug 用 `MODE_VERIFY`）。
4. **屏障判据压到一条规则**：`需要屏障 ⇔ 目标态 != 模拟态 || 上一次访问是写 || 首次使用`。`OldState` 恒为 `RESOURCE_STATE_UNKNOWN`（用引擎跟踪态），`Flags |= UPDATE_STATE`。UAV↔UAV 写、写后同态写全部被这一条覆盖。
5. **不做显存别名**。Diligent 只有 sparse 一种 `IDeviceMemory`，且 `BindSparseResourceMemory` **隐式 Flush + 要求 SPARSE_BINDING 队列 + 必须显式 fence/WaitForIdle 同步**（`DeviceContext.h:3797-3816`，`DeviceMemory.h:44-52`）——用它做逐帧瞬态别名是负收益。替代：**描述符键瞬态池（跨帧 + 帧内复用）**。
6. **瞬态池键直接用 `TextureDesc`/`BufferDesc`**：`operator==` 忽略 `Name`（`Texture.h:190-215`），零适配层。帧内 last-use 归还、后续 pass 可再借（单队列下 GPU 执行序即同步，O3DE 瞬态池同款论证）。
7. **光栅两模式，一套编译产物**：compile 始终产出 attachment 计划（clear / discardStart / discardEnd / 只读深度 / resolve）。
   - **Direct 模式（v1 默认）**：`SetRenderTargets(NONE)` + 先绑后清；`discardStart` 用屏障的 `DISCARD_CONTENT` 表达（D3D12→`DiscardResource`，VK→`oldLayout=UNDEFINED`，`CommandContext.cpp:228-260`、`DeviceContextVkImpl.cpp:3223`）。**对 PSO 零侵入**。
   - **RenderPass 模式（M2，opt-in）**：FG 建 `IRenderPass`/`IFramebuffer`（哈希缓存），拿到 `storeOp=DISCARD`、`MEMORYLESS`、pass 内 MSAA resolve。代价明写：PSO 必须 `pRenderPass != null`、`NumRenderTargets=0`、RTV 格式 `UNKNOWN`（`PipelineState.h:327-360`）。
8. **v1 单队列 / 单 immediate context / 单线程**，但显式屏障路线本身就是并行录制与异步队列的前提（自动转换模式**非线程安全**，`DeviceContext.h:2669-2687`）。接缝：`PassContext` 薄包装 + 每 pass 屏障批已成形。
9. **规模**：核心 `≈2000-2500` 行（含校验/graphviz），无第三方依赖，只依赖 Diligent Core 接口。
10. **不做清单**（§2.2）：显存别名、pass 重排、subpass 自动合并、子资源粒度状态、`.pass` 资产化、多设备、自定义资源类型泛型、PROTECTED 内存。

---

## 1. 事实基线（只列本方案承重的条目）

### 1.1 Diligent 提供什么

| 事实 | 出处 |
|---|---|
| `StateTransitionDesc`：`pResourceBefore/pResource/FirstMipLevel/MipLevelsCount/FirstArraySlice/ArraySliceCount/OldState/NewState/TransitionType/Flags`；**`OldState=UNKNOWN` ⇒ 用引擎内部跟踪态** | `DeviceContext.h:2304-2349` |
| `STATE_TRANSITION_FLAG_UPDATE_STATE`（引擎接管跟踪）/ `DISCARD_CONTENT`（尽量丢内容，避免 RT 解压与转 COMMON 停顿）/ `ALIASING`（**仅 sparse + aliasing flag 资源**） | `DeviceContext.h:2282-2298` |
| `TransitionResourceStates(count, descs)` 批量；old==new==`UNORDERED_ACCESS` ⇒ UAV 屏障（`TransitionType` 必须 IMMEDIATE） | `DeviceContext.h:3502-3533, 2353-2354` |
| 转换模式 `NONE / TRANSITION / VERIFY`；`TRANSITION` 会改内部状态、**非线程安全**；`VERIFY` 只读校验 | `DeviceContext.h:245-273, 2669-2687` |
| **状态转换在 render pass 内被禁止**（D3D12/VK 均 DEV_CHECK） | `DeviceContextVkImpl.cpp:585, 3247, 3507` |
| VK：`DISCARD_CONTENT` ⇒ `oldLayout = VK_IMAGE_LAYOUT_UNDEFINED`；写后总加屏障（`AfterWrite`） | `DeviceContextVkImpl.cpp:3218-3236` |
| D3D12：`DISCARD_CONTENT` ⇒ `DiscardResource`（要求处于 RT/DEPTH_WRITE 态）；纹理支持子资源范围屏障；`UPDATE_STATE` ⇒ `SetState(NewState)` | `CommandContext.cpp:228-260, 300-337, 396-424` |
| 引擎状态跟踪是**整资源**粒度：`ITexture::SetState/GetState` 单个 `RESOURCE_STATE` | `TextureBase.hpp:299-311, 417` |
| `TextureDesc::operator==` **忽略 Name**，含 `BindFlags/MiscFlags/ClearValue/ImmediateContextMask` ⇒ 天然池键 | `Texture.h:190-215` |
| VK 启用 `dynamic_rendering` 时不创建 `FramebufferCache/RenderPassCache`；`SetRenderTargets` 走 `vkCmdBeginRenderingKHR`，**附件集 hash 不变则不重启** | `RenderDeviceVkImpl.cpp:153-157`；`CommandBuffer.hpp:357-369` |
| 隐式路径附件写死 `loadOp=LOAD / storeOp=STORE`；`ClearRenderTarget` 在 rendering 未开始时改成 attachment clear 值（先绑后清即 CLEAR 路径） | `FramebufferCache.cpp:150-159`；`DeviceContextVkImpl.cpp:1280-1290, 1415-1418` |
| 显式 render pass：`RenderPassAttachmentDesc{LoadOp/StoreOp/StencilOps/InitialState/FinalState}`、`SubpassDesc`、`AttachmentReference`；D3D12 映射到 `ID3D12GraphicsCommandList4::BeginRenderPass`（含 ENDING_ACCESS_RESOLVE）；GL/D3D11 也实现了 Begin/EndRenderPass（模拟） | `RenderPass.h:44-208`；`DeviceContextD3D12Impl.cpp:1460-1634`；`DeviceContextGLImpl.cpp:658-686`；`DeviceContextD3D11Impl.cpp:1831-1858` |
| `MISC_TEXTURE_FLAG_MEMORYLESS`：只能作 framebuffer 内瞬态附件，load ∈ {CLEAR, DISCARD} 且 store = DISCARD | `Texture.h:60-65` |
| PSO 与 render pass 强耦合：`pRenderPass != null` ⇒ `NumRenderTargets=0`、RTV/DSV 格式必须 `UNKNOWN`、`SubpassIndex` 生效 | `PipelineState.h:327-360` |
| Swapchain `Present` 内部完成 PRESENT 转换（依赖引擎跟踪态）；主 swapchain 触发 `FinishFrame` | `SwapChainD3D12Impl.cpp:144-162`；`SwapChainVkImpl.cpp:685-686` |
| Sparse 是唯一的显存对象类型：`DEVICE_MEMORY_TYPE_{UNDEFINED,SPARSE}`；`BindSparseResourceMemory` **隐式 Flush**、需 SPARSE_BINDING 队列、需显式 fence/WaitForIdle | `DeviceMemory.h:44-52`；`DeviceContext.h:3797-3816` |
| `BeginDebugGroup/EndDebugGroup/InsertDebugLabel`；`GetStats()` 命令计数 | `DeviceContext.h:3710-3729, 3818-3822` |
| `GenerateMips`：D3D12 内部自管逐子资源转换，全 mip/slice 覆盖时 `SetState(SHADER_RESOURCE)`，否则恢复原态；VK 以进入态为基准 blit 并恢复，**要求进入态 != UNDEFINED** | `GenerateMips.cpp:141-153, 231-278`；`GenerateMipsVkHelper.cpp:53-100` |

**Diligent 没有**：帧图/pass 剔除/生命周期管理、瞬态资源池、非 sparse 的显存别名（placed resource）、跨 pass 屏障批调度。这四项正是本模块的增量。

**为什么不直接依赖自动转换模式（`MODE_TRANSITION`）**——四条硬理由：① 逐命令生效，无法成批，屏障数量与命令数同阶；② render pass 内禁止转换，一进 render pass 就失效；③ 无法表达 `DISCARD_CONTENT`，丢掉 discardStart 的全部收益；④ D3D12 自动路径只看 old/new 是否都是 UAV，对 UAV **读→读**也插 UAV 屏障（`CommandContext.cpp:396-398`），是过度同步。此外它非线程安全，堵死未来并行录制。

### 1.2 filament 给骨架

- `Builder`：`create/createSubresource/read/[[nodiscard]] write/sideEffect/declareRenderPass/sample/getDescriptor`；descriptor setup 期按值捕获，物化延迟到 execute（`fg/FrameGraph.h:100-235`）。
- `addPass<Data>(name, setup, execute)`，注释明确 "setup captures by reference / execute captures **must** be by copy"（`fg/FrameGraph.h:268-297`）。
- `compile()`：`cull()` → `stable_partition` 活跃前缀 → 逐 pass 注册资源得 first/last → `passNode->resolve()`（附件 discard 推导）→ 生成 `devirtualize/destroy` 列表 → `resolveResourceUsage()`（用量按位 OR）（`FrameGraph.cpp:141-249`）。
- `execute()`：first pass 前 devirtualize（**从 `TextureCache` 取**）、末 pass 后 destroy（**归还池**）（`FrameGraph.cpp:251-295`）。
- 附件推导：`discardEnd`=写后无活跃读者、`discardStart`=之前无活跃写者、`readOnlyDepthStencil`=本 pass 不写深度、clear 蕴含 discardStart、导入 RT 可 keepOverride 压制（`PassNode.cpp:104-250`）。
- `TextureCache`：完整 descriptor（不含 name）为键、年龄 GC（`TextureCache.h:129-171`）。**无堆别名**。
- fg 层**不发射任何屏障**——这一条在 Diligent 上不可照搬：Diligent 就是"后端"，不做启发式布局推导，屏障必须由 FG 自算。

### 1.3 O3DE 给三件精华

1. **编译期屏障 + 首用/末用 load/store 优化**：`OptimizeTransientLoadStoreActions` 把瞬态附件首用 Load→DontCare（除 Clear）、末用 Store→DontCare（`RHI/Code/Source/RHI/FrameGraphCompiler.cpp:494-540`）。
2. **瞬态池帧内复用语义**：Deactivate 不销毁、同帧后续 scope 可复用（`RHI/.../TransientAttachmentPool.h:30-37`）；激活/失活编成 `(scopeIndex, action, attachmentIndex)` 可排序命令流（`FrameGraphCompiler.cpp:561-606`）。
3. **队列中心图 + 跨队列冗余边裁剪**（`FrameGraphCompiler.cpp:143-257`：同队列先串成链，再对每个 consumer 的每个 producer 队列只保留"最晚 producer"，并丢弃"已有更晚 producer 同步了更早 consumer"的冗余边）——算法有价值，但 v1 单队列下整块塌缩为零，**留作未来参考，不实现**。

不吸收：RPI 的 `.pass` 资产链（152+ 资产 + 反射 + 热载 + 9 层间接）、多设备、`ExecuteGroup` 成本模型、拓扑排序、AliasedHeap 别名装箱（Diligent 无对应 API）。

---

## 2. 目标与非目标

### 2.1 目标（可验收）

1. **声明式**：一帧 = 一串 `add_pass`；read/write + Use 即依赖，无手写 barrier、无手写顺序。
2. **自动**：剔除、生命周期、池化物化、状态转换、RT 绑定与清屏、load/store 推导。
3. **高性能**：稳态零堆分配（帧 arena + 池命中）；compile O(V+E)；每 pass ≤1 次 `TransitionResourceStates`；执行期无分支决策（全是编译产物的线性回放）。
4. **可测**：编译器不 include Diligent 命令接口，产物为 POD 表 ⇒ 无 GPU 的黄金测试。
5. **可观测**：graphviz、每 pass debug group、`Stats` 与 `DeviceContextStats` 对账、Debug 期 `GetState()` 对账。

### 2.2 非目标（逐条理由，均有出处）

| # | 不做 | 理由 |
|---|---|---|
| N1 | 显存别名（placed / sparse aliasing） | Diligent 只有 sparse 一种显存对象（`DeviceMemory.h:44-52`）；`BindSparseResourceMemory` 隐式 Flush + 需 SPARSE_BINDING 队列 + 需显式 fence（`DeviceContext.h:3801-3814`）；`STATE_TRANSITION_FLAG_ALIASING` 明文只支持 sparse aliasing 资源（`DeviceContext.h:2296-2298`）⇒ 逐帧别名负收益。**接缝**：池的 `acquire/release` 是唯一改动点，若 Diligent 将来加 placed resource，替换池实现即可 |
| N2 | pass 重排 / 拓扑排序 | 写产生新版本、读引用既定版本 ⇒ 声明序天然是合法调度序（filament 生产实证）。乱序声明用校验报错替代 |
| N3 | subpass 自动合并 | 复杂度高、收益限 tiler。M2 的 RenderPass 模式已经把 `SubpassDesc` 的口留好，需要时做**显式** subpass（用户给 group id），不做自动分析 |
| N4 | 子资源（mip/slice）粒度状态跟踪 | 引擎跟踪是整资源粒度（`TextureBase.hpp:417`），`UPDATE_STATE` 与部分范围转换语义冲突。整资源粒度＝保守正确；mip 链场景用 `GenerateMips` helper 或拆资源 + copy |
| N5 | 异步队列 / 并行录制（v1） | 单图形队列覆盖桌面/编辑器全部需求；跨 context 的 SHADER_RESOURCE 转换还有"必须在 graphics context 上做"的约束（`DeviceContext.h:3524-3530`）。**接缝**：显式屏障 + 每 pass 批 + `PassContext` 包装 |
| N6 | `.pass` 资产化 / 数据驱动管线 | code-first；O3DE 的间接层是"美术改管线不改代码"的工具链需求，与调度内核无关 |
| N7 | 自定义资源类型泛型 | filament 留了 `ResourceAllocator<T>` 口但全仓只有 texture 一个实例——YAGNI 实证。封闭集合 = {Texture, Buffer}（+ 无数据依赖排序用的 Token） |
| N8 | 多设备 / PROTECTED 内存 / 跨帧编译缓存 | 无需求；逐帧重建同规模图已是 filament 生产事实（微秒级），缓存只带来失效复杂度 |
| N9 | 常量缓冲进图 | per-frame 上传用 `USAGE_DYNAMIC` + `MAP_DISCARD` ring，本就不需要状态跟踪与生命周期管理 |

---

## 3. 架构与帧流程

```
┌─ 声明（渲染线程，每帧）──────────────────────────────────────┐
│ fg.begin_frame();                                            │
│ auto bb = fg.import_backbuffer(swapChain);                    │
│ fg.add_pass<Data>("GBuffer", setup, exec);  × N               │
│ fg.present(bb);                                               │
├─ compile()（5 阶段，全 O(V+E)）─────────────────────────────┤
│ ① cull        引用计数迭代消减 + stable_partition 保序        │
│ ② lifetime    first/last + 全帧 Use 位 OR → BindFlags/视图集  │
│ ③ attachment  clear / discardStart / discardEnd / 只读深度     │
│ ④ barrier     状态模拟 → 每 pass vector<StateTransitionDesc>  │
│ ⑤ raster      RT/DSV 槽位表（Direct）或 RP/FB 键（RenderPass）│
├─ execute()（单 immediate context）──────────────────────────┤
│ 每 pass：DebugGroup → 物化(first-use) → 屏障批 →              │
│          [绑 RT + 清屏 | BeginRenderPass] →                   │
│          用户 lambda → [EndRenderPass] → 归还池(last-use)      │
├─ end_frame()：解绑 RT、池 GC、arena reset ────────────────────┤
└──────────────────────────────────────────────────────────────┘
外部：swapChain->Present() 由调用方发起（PRESENT 转换与 FinishFrame 由 Diligent 内部完成）
```

**架构决策**

| 决策 | 理由 |
|---|---|
| `FrameGraph` 持久对象 + `begin_frame()` reset | 容量复用 ⇒ 稳态零分配；比"栈对象 + 外部 arena"少一层基础设施 |
| 节点 SoA + `uint16` 索引 + arena thunk，无虚函数 | 剔除/生命周期是两趟连续遍历；200 pass 规模 compile 期望 30-80µs |
| 屏障在 compile 期模拟，不在执行期 diff `GetState()` | 可校验、可统计、可脱 GPU 单测；执行期是纯线性回放 |
| `OldState` 恒 `UNKNOWN` + `UPDATE_STATE` | 池化复用的纹理进入本帧时状态不可预知（不是 UNDEFINED），交给引擎跟踪最稳；`UPDATE_STATE` 保证 FG 之外的代码（ImGui、Present、拷贝）继续能用自动模式 |
| FG 内命令一律 `MODE_NONE`（Debug `MODE_VERIFY`） | 屏障批已保证态正确；`VERIFY` 是 Diligent 专为"检查不转换"提供的模式 |
| 状态权威在 FG，引擎跟踪作执行期校验 | Debug 期每 pass 后断言 `GetState() == simState`，一处断言抓所有模拟 bug |

---

## 4. 核心 API（C++23）

### 4.1 句柄与资源 trait

```cpp
namespace fg {

struct Handle {
    static constexpr uint16_t kInvalid = 0xFFFF;
    uint16_t index   = kInvalid;   // 资源槽位
    uint16_t version = 0;          // write 递增：捕获陈旧句柄
    constexpr bool valid() const noexcept { return index != kInvalid; }
    friend constexpr bool operator==(Handle, Handle) noexcept = default;
};

template <class R> struct Id : Handle {};   // 纯类型标记，编译期不可互换

struct Texture {
    struct Desc {
        uint32_t              width = 1, height = 1;
        uint16_t              arraySizeOrDepth = 1;
        uint8_t               mipLevels = 1, sampleCount = 1;
        RESOURCE_DIMENSION    dim    = RESOURCE_DIM_TEX_2D;
        TEXTURE_FORMAT        format = TEX_FORMAT_UNKNOWN;
        bool                  memoryless = false;   // 仅 RenderPass 模式下生效（M2）
        friend constexpr bool operator==(const Desc&, const Desc&) noexcept = default;
    };
    enum class Use : uint16_t {
        None = 0,
        Sampled          = 1 << 0,   // SRV
        ColorAttachment  = 1 << 1,
        DepthWrite       = 1 << 2,
        DepthRead        = 1 << 3,   // 只读 DSV
        Storage          = 1 << 4,   // UAV
        CopySrc          = 1 << 5,
        CopyDst          = 1 << 6,
        ResolveSrc       = 1 << 7,
        ResolveDst       = 1 << 8,
        ShadingRate      = 1 << 9,
        InputAttachment  = 1 << 10,  // 仅 RenderPass 模式
        GenerateMips     = 1 << 11,
    };
    static constexpr Use kDefaultRead  = Use::Sampled;
    static constexpr Use kDefaultWrite = Use::ColorAttachment;
};

struct Buffer {
    struct Desc {
        uint64_t size = 0;
        uint32_t elementStride = 0;
        friend constexpr bool operator==(const Desc&, const Desc&) noexcept = default;
    };
    enum class Use : uint16_t {
        None = 0,
        Vertex = 1 << 0, Index = 1 << 1, Constant = 1 << 2, IndirectArgs = 1 << 3,
        Sampled = 1 << 4 /*SRV*/, Storage = 1 << 5 /*UAV*/, CopySrc = 1 << 6, CopyDst = 1 << 7,
    };
    static constexpr Use kDefaultRead  = Use::Sampled;
    static constexpr Use kDefaultWrite = Use::Storage;
};
FG_FLAG_OPS(Texture::Use); FG_FLAG_OPS(Buffer::Use);   // constexpr | & ~ 等

template <class R>
concept Resource = requires {
    typename R::Desc; typename R::Use;
    { R::kDefaultRead } -> std::convertible_to<typename R::Use>;
};

using TextureId = Id<Texture>;
using BufferId  = Id<Buffer>;
using TokenId   = Id<struct Token>;   // 无数据依赖的强制排序（filament DummyLink 同款）
```

> 描述符**故意不含 usage/BindFlags**：BindFlags 由声明的 Use 位 OR 推出，单一事实源。

### 4.2 Builder（setup 期唯一入口）

```cpp
class Builder {
public:
    template <Resource R> Id<R> create(std::string_view name, typename R::Desc desc);
    TextureId create_texture(std::string_view n, Texture::Desc d) { return create<Texture>(n, d); }
    BufferId  create_buffer (std::string_view n, Buffer::Desc  d) { return create<Buffer>(n, d); }

    template <Resource R> Id<R> read (Id<R>, typename R::Use = R::kDefaultRead);
    template <Resource R> [[nodiscard]] Id<R> write(Id<R>, typename R::Use = R::kDefaultWrite);
    TextureId sample(TextureId t) { return read(t, Texture::Use::Sampled); }

    // 光栅声明：内部完成 write/read 并返回新版本句柄，杜绝"先 write 再 declare"错序
    struct Attachment {
        TextureId id;
        bool      clear = false;
        OptimizedClearValue clearValue{};
        TextureId resolveTarget{};        // 可选 MSAA resolve（RenderPass 模式内联，Direct 模式生成隐式 resolve pass）
        uint8_t   mip = 0; uint16_t slice = 0;
    };
    struct RasterDesc {
        std::span<const Attachment> colors;      // ≤ MaxRenderTargets(8)
        Attachment                  depth{};
        bool                        depthReadOnly = false;
    };
    class RasterRef {                            // 取回 write 后的新版本
    public:
        TextureId color(uint32_t slot) const; TextureId depth() const;
    };
    RasterRef declare_raster(const RasterDesc&);

    void side_effect();                          // 本 pass 为叶子，不可剔除
    template <Resource R> const typename R::Desc& desc(Id<R>) const;
};
```

### 4.3 FrameGraph

```cpp
class FrameGraph {
public:
    enum class RasterMode : uint8_t { Direct, RenderPass };   // §6
    struct Config {
        size_t     arenaBytes = 256 * 1024;
        uint16_t   maxPasses  = 256;
        RasterMode rasterMode = RasterMode::Direct;
    };
    FrameGraph(IRenderDevice*, IDeviceContext*, ResourcePool&, Config = {});

    void begin_frame();
    void end_frame();

    template <class Data, class Setup, class Exec>
        requires std::invocable<Setup, Builder&, Data&> &&
                 std::invocable<Exec, const PassResources&, const Data&>
    const Data& add_pass(std::string_view name, Setup&& setup, Exec&& exec);

    // 导入：所有权留在调用方。不需要 currentState —— 用引擎跟踪态（Debug 断言 GetState() != UNKNOWN）
    TextureId import(std::string_view name, ITexture*, Texture::Use allowed);
    BufferId  import(std::string_view name, IBuffer*,  Buffer::Use  allowed);
    TextureId import_backbuffer(ISwapChain*);              // 内部取 RTV->GetTexture()

    template <Resource R> void move(Id<R> from, Id<R> to);  // filament forwardResource
    void present(TextureId);                                // 叶子，防剔除；不发屏障
    TokenId create_token(std::string_view name);

    Blackboard& blackboard();

    bool compile();
    void execute();

    // helper（内部同走 add_pass）
    void add_copy_pass(TextureId src, TextureId dst);
    void add_copy_pass(BufferId src, BufferId dst);
    void add_resolve_pass(TextureId msaaSrc, TextureId dst);
    void add_generate_mips_pass(TextureId);

    struct Stats { uint32_t passes, culled, realized, poolHits, barriers, clears; };
    const Stats& stats() const noexcept;
    void export_graphviz(std::ostream&) const;              // Debug
};
```

**语义细则**

1. `write` 必须接收返回值（`[[nodiscard]]`）；旧句柄再用 ⇒ 版本校验错。
2. 同 pass 对同一资源 read + write 并存 ⇒ 错误（比 filament 只禁"写后读"更严；动态渲染下 SRV+RTV 同资源本就是 hazard）。
3. 每条边的 Use 必须映射到**单一** `RESOURCE_STATE`；同 pass 的多个纯读 Use 位或合并。
4. 写导入资源自动 `side_effect()`（filament 同款）；`present` 是天然叶子。
5. `import` **不要求调用方填 currentState**：引擎已跟踪整资源态，多传一个参数只会引入不一致风险；Debug 期断言 `GetState() != UNKNOWN`。
6. 名称用 `std::string_view`，要求指向字面量或生命周期长于本帧的存储。
7. setup 引用捕获、execute **值捕获**；`Data` 与 execute 闭包 `static_assert(std::is_trivially_destructible_v<...>)` —— arena 无析构遍历。

### 4.4 执行期访问

```cpp
class PassResources {
public:
    IDeviceContext* ctx() const noexcept;
    ITexture* texture(TextureId) const;  IBuffer* buffer(BufferId) const;
    ITextureView* srv(TextureId) const;  ITextureView* uav(TextureId, uint8_t mip = 0) const;
    ITextureView* rtv(TextureId) const;  ITextureView* dsv(TextureId, bool readOnly = false) const;
    IBufferView*  srv(BufferId) const;   IBufferView*  uav(BufferId) const;
    template <Resource R> const typename R::Desc& desc(Id<R>) const;
    IRenderPass*  render_pass() const noexcept;    // RenderPass 模式；Direct 模式返回 nullptr
    void detach(TextureId, RefCntAutoPtr<ITexture>& out);   // 导出，脱离池管理（历史帧纹理）
};
```
未声明访问 ⇒ 断言失败（filament `FrameGraphResources.cpp` 同款前置条件）。默认视图取 `GetDefaultView()`（引擎已缓存，`Texture.h:498-499`）；只读 DSV、单 mip UAV 等非默认视图缓存在池条目上。

---

## 5. compile()：五阶段

### 5.1 剔除（filament 算法）

每条边给 **from 节点** refCount+1；`side_effect` / 写导入 / `present` 置 TARGET 位（refCount 恒 ≥1）；从 refCount==0 的资源节点栈式传播：写者 pass refCount 归零则被剔除，其读入边再减。O(V+E)。`stable_partition` 把活跃 pass 前移，**保持声明序**。

### 5.2 生命周期与用量

- 逐活跃 pass 逐访问：资源 refCount++、`first = min`、`last = max`、`usageMask |= use`。
- `usageMask` → `BindFlags`（+ `MiscFlags`：`generateMips`/`memoryless`），是资源创建参数的唯一来源。
- 生成每 pass 的 `realize[] / release[]` 列表（filament devirtualize/destroy 同构）。
- 被剔除资源永不创建——这是延迟物化的全部意义。

### 5.3 附件计划（filament resolve + O3DE load/store 合并）

对每个光栅 pass 的每个附件：

| 项 | 规则 |
|---|---|
| `clear` | 用户请求；clear 蕴含 discardStart |
| `discardStart` | 该版本之前无活跃写者（首次写） |
| `discardEnd` | 本 pass 写它且之后无活跃读者；导入/present/detach 的资源禁止 |
| `depthReadOnly` | 本 pass 只 `DepthRead` |
| `resolve` | 声明了 `resolveTarget` 且 sampleCount>1 |

导入 RT 一律 `discardStart = discardEnd = false`（filament keepOverride 的简化版）。

### 5.4 屏障计划（本方案核心简化）

```cpp
// simState: 每资源的模拟态；wroteLast: 上一次访问是否为写
for (PassIndex p : activePasses) {
    for (const Access& a : accesses(p)) {
        const RESOURCE_STATE required = state_of(a.use);      // §5.6 纯函数
        Sim& s = sim[a.resource];
        const bool firstUse   = (s.uses++ == 0);
        const bool needBarrier = (s.state != required) || s.wroteLast || firstUse;
        if (needBarrier) {
            barriers[p].push_back(StateTransitionDesc{
                .pResource = nullptr /*execute 期填实体*/,
                .OldState  = RESOURCE_STATE_UNKNOWN,          // 用引擎跟踪态
                .NewState  = required,
                .Flags     = STATE_TRANSITION_FLAG_UPDATE_STATE |
                             (firstUse && is_full_overwrite(a) ? STATE_TRANSITION_FLAG_DISCARD_CONTENT
                                                               : STATE_TRANSITION_FLAG_NONE)});
        }
        s.state = required;
        s.wroteLast = is_write(a.use);
    }
}
```

要点与依据：

- **一条判据覆盖三类需求**：状态变化、写后同态访问（VK 的 `AfterWrite` 语义，`DeviceContextVkImpl.cpp:3219-3228`）、UAV↔UAV 写（`OldState=UNKNOWN` 解析出 UAV，old==new==UAV ⇒ D3D12/VK 自动发 UAV 屏障，`CommandContext.cpp:396-398`、`DeviceContext.h:3507-3509`）。UAV **读→读**不发屏障（优于自动模式的过度同步）。
- **首用必发屏障**：池化复用的纹理进入本帧时状态未知，不能假设 `UNDEFINED`。`OldState=UNKNOWN` 让引擎给旧态，`DISCARD_CONTENT` 在"首用即全覆盖写（附件写/UAV 写/CopyDst）"时打上 ⇒ D3D12 `DiscardResource`、VK `oldLayout=UNDEFINED`，等价 filament `discardStart` 与 O3DE `DontCare`，且**不依赖显式 render pass**。
- `is_full_overwrite`：`ColorAttachment`（且 `discardStart`）/ `DepthWrite`（且 `discardStart`）/ `Storage` 首写 / `CopyDst`。
- 屏障只在 render pass 之外发射（Diligent 硬约束）；Direct 模式下屏障总在 `SetRenderTargets` 之前，RenderPass 模式下在 `BeginRenderPass` 之前。
- 整资源粒度（N4）。`FirstMipLevel/ArraySliceCount` 字段留作未来升级口。
- split barrier（`STATE_TRANSITION_TYPE_BEGIN/END`）作为 M3 可选优化：同队列、生产者与消费者间隔 ≥2 个 pass 时拆分。注意 BEGIN 不允许带 `UPDATE_STATE`（`CommandContext.cpp:421-423`、`DeviceContextVkImpl.cpp:3528`）。

### 5.5 光栅计划

- Direct 模式：产出 `{RTV 槽位视图键, DSV 视图键, clear 列表}`。
- RenderPass 模式：产出 `RenderPassKey{逐附件 format/samples/loadOp/storeOp/stencilOps/InitialState/FinalState, subpass 布局, resolve 引用}` 与 `FramebufferKey{RenderPassKey, 视图指针集, 尺寸}`，execute 期查缓存（§6.2）。
- `FinalState` 前瞻：附件的下一活跃使用态若确定且紧邻，写入 `FinalState`，并从下一 pass 的屏障批中删除该资源条目（render pass 收尾免费转换）。前瞻失败（多消费者分叉/非紧邻）一律回落显式屏障——保守优先。

### 5.6 Use → RESOURCE_STATE / BIND_FLAGS（唯一事实源）

| Texture::Use | RESOURCE_STATE | BIND_FLAGS | 视图 |
|---|---|---|---|
| Sampled | `SHADER_RESOURCE` | `BIND_SHADER_RESOURCE` | 默认 SRV |
| ColorAttachment | `RENDER_TARGET` | `BIND_RENDER_TARGET` | 默认 RTV |
| DepthWrite | `DEPTH_WRITE` | `BIND_DEPTH_STENCIL` | 默认 DSV |
| DepthRead | `DEPTH_READ` | `BIND_DEPTH_STENCIL` | `READ_ONLY_DEPTH_STENCIL` 视图 |
| Storage | `UNORDERED_ACCESS` | `BIND_UNORDERED_ACCESS` | 默认/单 mip UAV |
| CopySrc / CopyDst | `COPY_SOURCE` / `COPY_DEST` | — | — |
| ResolveSrc / ResolveDst | `RESOLVE_SOURCE` / `RESOLVE_DEST` | （由全帧 OR 决定） | — |
| ShadingRate | `SHADING_RATE` | `BIND_SHADING_RATE` | — |
| InputAttachment | `INPUT_ATTACHMENT` | `BIND_INPUT_ATTACHMENT` | 默认 SRV |
| GenerateMips | `SHADER_RESOURCE` | `BIND_RENDER_TARGET` + `MISC_..._GENERATE_MIPS` | 全 mip SRV |

`GenerateMips` 的进入态与退出态都取 `SHADER_RESOURCE`：D3D12 helper 覆盖全 mip 时主动 `SetState(SHADER_RESOURCE)`，VK helper 以进入态为基准并恢复（要求非 UNDEFINED）——两后端由此确定一致（`GenerateMips.cpp:141-153, 231-278`、`GenerateMipsVkHelper.cpp:53-100`）。

Buffer：`Vertex/Index/Constant/IndirectArgs → VERTEX_BUFFER/INDEX_BUFFER/CONSTANT_BUFFER/INDIRECT_ARGUMENT`（**稳态集**：首用一次转换后不再变）；`Sampled/Storage → SHADER_RESOURCE/UNORDERED_ACCESS`；`CopySrc/CopyDst → COPY_SOURCE/COPY_DEST`。

---

## 6. execute() 与光栅两模式

### 6.1 Direct 模式（v1 默认）

```
per pass:
 1  ScopedDebugGroup(ctx, pass.name)
 2  realize(first-use)                                   // 池 acquire / CreateTexture + 视图
 3  if (!barriers.empty()) ctx->TransitionResourceStates(n, barriers)   // §5.4 产物
 4  if raster:
 5      ctx->SetRenderTargets(nColors, rtvs, dsv, MODE_NONE)            // 不变则引擎自动 no-op
 6      for cleared color: ctx->ClearRenderTarget(rtv, rgba, MODE_NONE)
 7      if clearDepth:     ctx->ClearDepthStencil(dsv, flags, d, s, MODE_NONE)
        // 先绑后清 ⇒ VK 走 attachment clear（DeviceContextVkImpl.cpp:1415-1418）
 8  user_exec(PassResources, Data)                        // 内部命令用 MODE_NONE / Debug MODE_VERIFY
 9  release(last-use)                                     // 归还池，不销毁
frame end: ctx->SetRenderTargets(0, nullptr, nullptr, MODE_NONE)
```

**刻意不在每 pass 末解绑 RT**：VK 的 `TransitionImageLayout` 自己会 `EndRenderScope()`（`CommandBuffer.cpp:151-160`），而连续同附件集的光栅 pass 不重启 rendering（`CommandBuffer.hpp:363-369`）是免费收益，解绑会毁掉它。视图强引用在 `end_frame` 统一释放。

**得失**：得到 clear/discardStart/自动屏障/池化，**对 PSO 零侵入**。失去 `storeOp=DISCARD` 与 `MEMORYLESS`（隐式路径写死 STORE，`FramebufferCache.cpp:150-159`）——桌面上这部分收益接近零，故 v1 接受。

### 6.2 RenderPass 模式（M2，opt-in）

```
 3' ctx->TransitionResourceStates(...)        // 仍在 render pass 外
 4' ctx->BeginRenderPass({rp, fb, clearCount, clears, MODE_NONE})
 8' user_exec(...)                            // 只发 draw；需 subpass 时 ctx->NextSubpass()
 9' ctx->EndRenderPass()                      // FinalState 在此生效
```

- `IRenderPass`/`IFramebuffer` 各一个哈希缓存（跨帧常驻，键集随图形状稳定）；池条目释放时按视图反查驱逐对应 framebuffer（Diligent 内部 `FramebufferCache` 同思路）。
- 收益：`loadOp=DISCARD`、`storeOp=DISCARD`、`MEMORYLESS`（`Texture.h:60-65` 的三条硬约束正好由 §5.3 的计划满足）、pass 内 MSAA resolve（D3D12 走 `ENDING_ACCESS_RESOLVE`，`DeviceContextD3D12Impl.cpp:1517-1533`）、显式 subpass。
- **必须明写的代价**：用于本模式的 PSO 必须以 `pRenderPass != null` 创建，且 `NumRenderTargets=0`、RTV/DSV 格式填 `UNKNOWN`（`PipelineState.h:327-360`）；VK 还要求 PSO 的 render pass 与激活 render pass 兼容（`DeviceContextVkImpl.cpp:862-876`）。这是整条 PSO 创建链的改造，不是 FG 内部细节。FG 因此提供
  ```cpp
  IRenderPass* FrameGraph::layout_render_pass(const RasterLayout&);  // 只含 format/samples/引用，load/store 取默认
  ```
  作为 PSO 创建用的**兼容模板**：VK 的 render pass 兼容性不看 load/store op 与 layout，因此模板与逐帧实际 render pass 兼容；D3D12 无此约束。
- 因此：**面向 tiler/移动端或需要 subpass 时再开**，桌面默认 Direct。两模式共用 §5.3 的同一份 attachment 计划，增量代码 ≈300 行，无重复逻辑。

### 6.3 Present

`present(bb)` 只加一个防剔除的叶子节点，**不发任何屏障**。调用方在 `execute()` 后调 `swapChain->Present()`：D3D12/VK 的 swapchain 各自用引擎跟踪态转到 PRESENT（`SwapChainD3D12Impl.cpp:148`、`SwapChainVkImpl.cpp:685-686`）——这正是 FG 必须带 `UPDATE_STATE` 的硬理由。

---

## 7. 资源池（唯一的复用机制）

```cpp
class ResourcePool {                 // 引擎级、跨帧存活
public:
    ITexture* acquire(const TextureDesc&, std::string_view dbgName);
    IBuffer*  acquire(const BufferDesc&,  std::string_view dbgName);
    void      release(IDeviceObject*);            // 归还，不销毁
    void      gc(uint64_t frame, uint32_t maxAge = 4);
    void      shutdown(IDeviceContext*);          // WaitForIdle 后清空
private:
    // 键 = TextureDesc / BufferDesc 本身（operator== 忽略 Name，Texture.h:190-215）
    std::unordered_map<TextureDesc, std::vector<Entry>, DescHash> textures_;
    std::unordered_map<BufferDesc,  std::vector<Entry>, DescHash> buffers_;
    // Entry 附带非默认视图小缓存（只读 DSV / 单 mip UAV / 全 mip SRV）
};
```

- **帧内复用**：pass N 归还、pass M>N 再借同一对象。单队列上 GPU 执行序即同步（O3DE `TransientAttachmentPool.h:30-37` 同款论证）。首用 `DISCARD_CONTENT` 保证不读到旧内容。
- **跨帧复用**：更晚提交的命令流重写该纹理，顺序天然正确；对象全程被池持有，无在飞销毁问题。
- **`ClearValue` 归一化**：`ClearValue` 在 `TextureDesc::operator==` 内（`Texture.h:213`），若直填用户 clear 色会炸键空间。策略：瞬态纹理统一用**格式规范值**（颜色 0、深度 1、模板 0）。非规范 clear 只失去 fast-clear，不影响正确性，文档化 + 提供开关。
- **memoryless 正常入池**：`MiscFlags` 已在键内，池化省的是创建开销，与内容无关。
- resize：新 desc 未命中 ⇒ 新建，旧键条目按 age GC 回收，无需任何显式失效。
- `gc` 用帧号 + `maxAge`；释放走 `GPUCompletionAwaitQueue` 或简单地"帧号 + N 帧延迟"。

---

## 8. 校验与调试

| 级别 | 规则 |
|---|---|
| 错 | 句柄 index/version 失配；未初始化句柄 |
| 错 | 同 pass 同资源 read+write 并存 |
| 错 | 同 pass 的 Use 集映射到多个 `RESOURCE_STATE` |
| 错 | 读取从未被写且非导入的资源 |
| 错 | 导入资源的 Use 超出 `allowed`；导入时 `GetState() == UNKNOWN` |
| 错 | `declare_raster`：colors>8、colors 与 depth 含同一纹理、附件尺寸/层数不一致 |
| 错 | 声明数超 `Config` 上限；arena 溢出 |
| 警 | pass 被剔除（计数 + 名字）；同帧同描述符多次物化（提示应复用句柄） |

- Debug 期每 pass 后断言 `ITexture::GetState() == simState`（一处断言抓全部模拟 bug）。
- `export_graphviz`：实线活跃 / 虚线剔除，边标 Use 与目标态，附件标 `C:/DS:/DE:`。
- 每 pass `BeginDebugGroup` ⇒ RenderDoc/PIX 时间线可读。
- `Stats` 与 `IDeviceContext::GetStats()`（`DeviceContext.h:3818-3822`）双口径对账。
- **降级开关**：`DisablePooling` / `DisableDiscard` / `DisableCulling` / `ForceVerifyStates` / `ForceDirectRaster` —— 能把 FG 退化成最朴素实现，是定位驱动层 bug 的关键手段。
- **脱 GPU 黄金测试**：编译器输入是声明流、输出是 POD 表（活跃 pass 序、生命周期区间、屏障序列、附件计划），用 mock 设备做黄金对比。

---

## 9. 性能预算

| 项 | 手段 | 预期 |
|---|---|---|
| compile | SoA + 索引 + 两趟遍历，无 map（除 blackboard/池） | 200 pass / 400 资源 ≈ 30-80µs |
| 帧内分配 | arena（默认 256KB）+ 池命中 | 稳态 0 malloc |
| 屏障调用 | 每 pass ≤1 次 `TransitionResourceStates` | 相邻同态零转换 |
| 资源创建 | 描述符键池，形状稳定 ⇒ 稳态命中率 ~100% | 稳态创建次数 ≈ 0 |
| 对比基线 | 与手写 `SetRenderTargets` 循环发出等价 GPU 命令；VK 附件集 hash 不变不重启 rendering | 无额外 GPU 开销 |

---

## 10. 里程碑

| M | 内容 | 判据 |
|---|---|---|
| **M0 骨架**（5-7 人日） | Handle/Id、arena、`add_pass` thunk、剔除+保序、生命周期+用量 OR、`ResourcePool`、屏障模拟+批、Direct 光栅（绑定/清屏）、import/present/move/token、debug group | 把 Tutorial13（shadow + 主 pass）改写为 FG，RenderDoc 逐 pass 附件/状态与手写版一致；Debug 期 `GetState()` 对账零失配 |
| **M1 完整**（2-3 人日） | 校验全集、graphviz、Stats 对账、copy/resolve/generate_mips helper、降级开关、mock 黄金测试 | 关 SSAO ⇒ 整链剔除、被剔除资源零创建；`MODE_VERIFY` 全帧零违规 |
| **M2 光栅进阶**（3-4 人日） | RenderPass 模式 + RP/FB 缓存 + load/store 全量 + `MEMORYLESS` + pass 内 MSAA resolve + `layout_render_pass` | tiler 上带宽下降可测；同键 RP/FB 只创建一次；Direct/RenderPass 两模式像素一致 |
| **M3 可选** | split barrier、GPU 计时（`QUERY_TYPE_DURATION`）、deferred context 并行录制 spike | 性能回归基线；并行录制像素一致 |
| **永不做** | sparse 别名（除非 Diligent 加 placed resource）、pass 重排、subpass 自动合并、`.pass` 资产化 | — |

**代码组织**（无第三方依赖）

```
fg/Handle.h            Handle / Id<> / Resource concept / Use 位与 FG_FLAG_OPS      ~120
fg/FrameGraph.h/.cpp   FrameGraph + Builder + add_pass thunk + execute 驱动         ~900
fg/Compile.cpp         剔除 / 生命周期 / 附件计划 / 屏障计划 / 光栅计划              ~600
fg/Resources.h/.cpp    VirtualResource + Use→State/BindFlags 映射表 + PassResources ~450
fg/ResourcePool.h/.cpp 描述符键池 + 视图缓存 + GC（M2 加 RP/FB 缓存）                ~350
fg/Blackboard.h        名字 → 句柄                                                  ~60
fg/Debug.cpp           graphviz / Stats / 校验消息                                  ~250
```
合计 **≈2.7k 行**（含 M2）。对照：filament fg ≈5k 行（无屏障、无 buffer、无池键 RP/FB 缓存），O3DE 仅 RHI 编译器即远超。

---

## 11. 用法示例（可直接作为集成测试蓝本）

```cpp
using namespace fg;

fg.begin_frame();
auto bb = fg.import_backbuffer(m_SwapChain);

struct GBufferData { TextureId albedo, normal, depth; };
const auto& gbuf = fg.add_pass<GBufferData>("GBuffer",
    [&](Builder& b, GBufferData& d) {
        const auto [w, h] = std::pair{b.desc(bb).width, b.desc(bb).height};
        auto albedo = b.create_texture("albedo", {.width = w, .height = h, .format = TEX_FORMAT_RGBA8_UNORM});
        auto normal = b.create_texture("normal", {.width = w, .height = h, .format = TEX_FORMAT_RGBA16_FLOAT});
        auto depth  = b.create_texture("depth",  {.width = w, .height = h, .format = TEX_FORMAT_D32_FLOAT});
        Builder::Attachment colors[]{{.id = albedo, .clear = true}, {.id = normal, .clear = true}};
        auto rt = b.declare_raster({.colors = colors, .depth = {.id = depth, .clear = true}});
        d = {rt.color(0), rt.color(1), rt.depth()};
        fg.blackboard()["gbuffer.depth"] = d.depth;
    },
    [=](const PassResources& r, const GBufferData& d) {
        DrawGBuffer(r.ctx());                       // 只发 draw，RT 已绑、已清、状态已就绪
    });

fg.add_pass<SsaoData>("SSAO",
    [&](Builder& b, SsaoData& d) {
        auto ao = b.create_texture("ao", {.width = w, .height = h, .format = TEX_FORMAT_R8_UNORM});
        d.ao    = b.write(ao, Texture::Use::Storage);
        b.sample(gbuf.normal);
        b.read(gbuf.depth, Texture::Use::Sampled);
        fg.blackboard()["ao"] = d.ao;
    },
    [=](const PassResources& r, const SsaoData& d) { DispatchSsao(r.ctx(), r.uav(d.ao)); });

fg.add_pass<LitData>("Lighting", /* read albedo/normal/ao/depth，写 hdr */ ...);
// AO 被禁用（无人读 "ao"）时 SSAO 整链自动剔除，ao 纹理零创建

fg.add_pass<PostData>("Tonemap",
    [&](Builder& b, PostData& d) {
        Builder::Attachment color{.id = bb};
        d.rt = b.declare_raster({.colors = {&color, 1}});   // 写导入资源 ⇒ 自动 side_effect
        b.sample(fg.blackboard().get<Texture>("sceneColor"));
    },
    [=](const PassResources& r, const PostData& d) { Tonemap(r.ctx()); });

fg.present(bb);
if (fg.compile()) fg.execute();
fg.end_frame();
m_SwapChain->Present();
```

---

## 12. 验收尺子

1. **正确性**：Tutorial13 改写版与手写版 RenderDoc 逐 pass 附件/状态一致；Debug 期 `GetState()` 对账全帧零失配。
2. **剔除**：关闭消费者 ⇒ graphviz 虚线 + `Stats.culled` 正确 + 被剔除资源零设备对象创建。
3. **屏障**：相邻同态零转换；UAV 写链有 UAV 屏障、UAV 读链无；首用 `DISCARD_CONTENT` 在 D3D12 层可见 `DiscardResource`。
4. **池化**：固定场景 1000 帧后 `ITexture/IBuffer` 存活数收敛为常数；resize 后旧键被 GC。
5. **零分配**：1000 帧稳态 CRT debug heap 无增长。
6. **性能**：100 pass 基准帧 compile+execute CPU < 50µs。
7. **可用性**：未读本文的开发者 30 分钟内写出正确的 bloom pass。
8. **M2 专项**：RenderPass 模式与 Direct 模式像素一致；tiler 上 `storeOp=DISCARD` + `MEMORYLESS` 带宽下降可测。
