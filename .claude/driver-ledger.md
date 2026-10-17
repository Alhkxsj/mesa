# GPU 驱动台账 (Driver Ledger)

> 本文件是**持久化事实源**，不随对话压缩而丢失。
> 由 SessionStart hook 自动注入回上下文（startup / resume / clear / **compact**）。
> 规则：**动手改驱动代码前先读本文件；发现新的寄存器/MMIO/中断/调试事实就立刻追加进来。**

## 维护协议

- **只追加，不改写历史条目**。发现旧条目有误 → 在下方「更正记录」写一条，不要静默修改。
- 每条事实写清 **来源**（文件:行号 / 命令输出 / 寄存器 dump）和 **确认状态**（`已验证` / `待验证` / `已推翻`）。
- 反直觉的坑（"明明看着对但就是不对"）优先记，这里比代码注释更抗压缩。

---

## 项目基线

> 来源：`mesa-mtk/CLAUDE.md`（2026-10-15 快照）。**本仓库代码比该快照更新，基线待核实。**

- 本 fork：在 Panfrost/PanVK 上增加 Arm 私有 `kbase` 内核驱动后端（`/dev/mali*`，r32p0–r44p0），替代上游 `panfrost`/`panthor` DRM 驱动。
- 目标设备：Google Pixel 7，Mali-G710，Valhall/CSF，arch ≥ 10。
- 构建关键选项：`-Dpanfrost-kmds=kbase,panthor`；每个启用的 KMD 定义 `HAVE_PAN_KMOD_<NAME>`，
  所有 kbase 专属代码必须留在 `#if defined(HAVE_PAN_KMOD_KBASE)` 后面。
- kmod 抽象层：`src/panfrost/lib/kmod/`（`pan_kmod.c` / `pan_kmod_backend.h` / `kbase_kmod.c`）。
- **待核实**：本仓库已出现 `kbase_csf.c`、`kbase_sync.c`、`kmod/tests/`、`kmod/virtio/`，快照文档未覆盖。

---

## 寄存器定义

> 格式：`名称 | 偏移/地址 | 宽度 | 读写属性 | 语义 | 来源 | 状态`

| 寄存器 | 偏移 | 宽度 | 属性 | 语义 | 来源 | 状态 |
|---|---|---|---|---|---|---|
| _(待填充)_ | | | | | | |

---

## MMIO 映射

> 格式：`区域 | 物理基址 | 大小 | 用途 | 证据来源 | 状态`

| 区域 | 基址 | 大小 | 用途 | 证据来源 | 状态 |
|---|---|---|---|---|---|
| _(待填充)_ | | | | | |

---

## 中断流程

> 格式：`IRQ/信号 | 触发条件 | 处理路径 | 同步原语 | 坑 | 状态`

| IRQ / 信号 | 触发条件 | 处理路径 | 同步原语 | 已知坑 | 状态 |
|---|---|---|---|---|---|
| _(待填充)_ | | | | | |

---

## 调试发现

> 只记"反直觉"和"踩过两次以上"的。按发现日期倒序追加。

| 日期 | 发现 | 根因 | 证据 | 状态 |
|---|---|---|---|---|
| 2026-10-16 | 真机 device 探针 `vkEnumeratePhysicalDevices` 回 `VK_SUCCESS` 但 **0 设备**，且 `version_calls=0`（**kbase ioctl 一次都没发**） | 探针 harness 的 open 白名单漏放行物理 `/dev/mali0` handoff；`panvk_kbase_open_device` 失败后 `panvk_kbase_enumerate_physical_device` 把 `VK_ERROR_INCOMPATIBLE_DRIVER` **静默映射**成 `VK_SUCCESS` + 0 设备 | `m5a-20261016-194448/live-device-2.log`（`count=0 blocked=3 version_calls=0`）；`panvk_kbase_device.c:69`、`panvk_physical_device.c:541` | 已验证（已修，`live-device-3.log` 通过） |
| 2026-10-16 | **"枚举回 0 设备"不是内核/kbase ABI 问题**，别往驱动方向查 | 判据是 `version_calls`：为 0 且无 `DEVICE_VERSION` 行 ⇒ 失败在 **open/包装层**，在首次 ioctl 之前 | 同上 | 已验证 |
| 2026-10-16 | 探针 device 模式的 reopen 白名单与 `handed_off` 标志**耦合**，只修 open 不够 | `handed_off` 是 ioctl wrapper 的前置条件（`probe.active && probe.handed_off && arg`），reopen 分支不置它则后续 ioctl 全被拒 | `panvk_kbase_enum_probe.c` ioctl wrapper；`live-device-2.log` | 已验证（已修） |
| 2026-10-16 | `manifest.json` 被误判"三处哈希全过期"，实为归档约定误读 | `manifest.json` 记**阶段开始前**（`original/`），`current-manifest.json` 记**阶段结束后**（`current/`）；误判者拿阶段前清单对阶段后工作树 | m4g 全部六条哈希逐一核对：`manifest.json`↔`original/` 全符，`current-manifest.json`↔`current/` 全符 | 已验证（见 `OPEN-ITEMS.md` 更正记录 C1） |
| 2026-10-17 | **ICD 级探针的 mock 会用 `blocked()` 静默吸收驱动多发的 ioctl**，于是想当然以为"fixture 通过 ⇒ 代码正确" | 走 `blocked()` 的分支直接回 `-1/EPERM`，驱动侧观察不到差异。M4g 的 `alias-invalid-*` fixture 对着**会发 `MEM_FREE` 的旧代码**照样通过 | 旧代码下 `panvk_kbase_alias_probe_alias_invalid_cookie` 等全部 OK（97/97 里含它们）；真正的 oracle 是 `kbase_kmod_test.c` 的 `__wrap_*` 计数器 —— 回退修复后 `kbase_kmod_test.c:861` abort | 已验证 |
| 2026-10-17 | **ICD 内部符号探针读不到**（`-fvisibility=hidden`） | `nm -D libvulkan_panfrost.so` 只导出 `vk_icdGetInstanceProcAddr`/`vk_icdGetPhysicalDeviceProcAddr`/`vk_icdNegotiateLoaderICDInterfaceVersion` 三个符号；探针是独立进程，只经 syscall 观测。驱动内部状态（如 `alias_cleanup_uncertain`）无法直接读取，必须落到**可观测的 syscall 等价物**（munmap 返回值） | `nm -D --defined-only` 输出 3 行；`panvk_kbase_enum_probe.c` 用 `dlsym(icd,"vk_icdGetInstanceProcAddr")` 起手 | 已验证 |
| 2026-10-17 | **探针把失败的 unmap 记成成功** | `panvk_kbase_device_probe.inc` 的 `device_munmap` 修前**无条件** `unmaps++` / `entry->live=false`，失败的 munmap 也被计为已释放 → 恰好掩盖 cross-4GiB 泄漏 | M4g 负例 `cross-window`（`cpu_boundary_fixture=1`）只验窗口判定，走不到 unmap 失败分支 | 已验证（已修：仅成功后记账，失败打印 `DEVICE_UNMAP_FAILED` 并置 `cleanup_uncertain`） |
| 2026-10-17 | **给探针加"CPU 仿真"分支时，台账记账（`device_note`）必须放在 `probe.live` 早返回之前** —— 否则 live 路径永远不计该请求 | M5b-1 给 `panvk_kbase_device_probe.inc` 的 `CS_GET_GLB_IFACE` 加 CPU 仿真时，把 `probe.live ? host_ioctl(...)` 放在 `device_note(DEV_CSIF)` 前面返回 ⇒ live 不计 DEV_CSIF ⇒ `device_check_result` 的 `DEV_CSIF >= 1` 失败 ⇒ live `--live-device` 打 `csif=0 accepted=0`。**CPU 完全跑不出这个症状**（CPU 走的恰好是会记账的仿真分支） | `m5b1-.../logs/live-device-precheck-regression-csif0.log`（`csif=0 accepted=0`，exit=1）对比 `logs/live-device-baseline.log`（修后 `csif=2 accepted=1`，exit=0，与 M5a 基线一致） | 已验证（已修） |
| 2026-10-17 | **kbase ioctl union 的 `in` 与 `out` 在偏移 0 处重叠**（`cs_tiler_heap_init`、`mem_alias`、`cs_queue_bind` 都是）—— 写 `out` 会摧毁 `in` | union 不是 struct；诊断 printf 若要打印输入，**必须先把输入抓进局部变量**再写 out。本阶段被这个坑骗出 `nents=256 chunk=3411968` 这种垃圾值 | `mali_kbase_ioctl.h` 的 union 定义；本阶段 `DEVICE_ALIAS` / `DEVICE_HEAP_INIT` 诊断修复前后 | 已验证（诊断假象，非驱动 bug） |
| 2026-10-17 | **空 PRIMARY 命令缓冲在 kbase 上不 vacuous**：`EndCommandBuffer` 的空子队列快捷路径**显式排除** COMPUTE（`i != PANVK_SUBQUEUE_COMPUTE`）⇒ COMPUTE 永远走 `finish_cs` ⇒ 无条件发 `cs_wait_slots` + CS-error save + `cs_flush_caches` + `cs_wait_slot` ⇒ `cs_root_chunk_size != 0` ⇒ 真发 kick | 且只有 COMPUTE 有 chunk ⇒ `graphics` 保持 false ⇒ 不需要 tiler heap rearm、不撞 `graphics_ready`。注意 `cs_is_valid(b)` 只是 `!b->invalid`，对零命令缓冲**恒真** —— 真正的判据是 `cs_root_chunk_size`，不是那个 `cs_is_valid` 门 | `panvk_vX_cmd_buffer.c:271-283`；真机 `live-submit.log` 的 `CS_QUEUE_KICK ring_va=0x5ffffc1000 index=4 ret=0`；回退该条件 ⇒ `panvk_kbase_icd_test.c:924` abort 且 `--cpu-submit` 转 `kick_delta=0` | 已验证 |
| 2026-10-17 | 两个生产 hazard：非法 alias 输出**任意 `MEM_FREE`**；cross-4GiB **unmap 失败静默返回坏 VA** | 前者绑定条件是**来源（provenance）**，未通过校验的输出无法确立 cookie 归属 ⇒ 该分支**永不该 FREE**；后者 `alias->gpu_va` 本身就是坏 VA，且唯一调用点只看 NULL ⇒ 只能**回滚+上报**（不填 alias、不取 BO 引用） | `kbase_kmod.c:762`（`!valid`）/ `:785`（munmap 失败）；调用点 `panvk_kbase_compute_queue.inc:159` | 已验证（已修，`h1-*`/`h2-*` 检查点，回退即 abort） |

| 2026-10-18 | `perf_create` 是 `ENOTSUP` 空桩，但 `perf_enable/dump/destroy` **未设置**，而 `pan_kmod_perf_enable/dump/destroy()` **无条件解引用** `dev->ops->perf_*` | 一旦有人只把 `perf_create` 实现成返回非 NULL，`pan_kmod_perf_enable()` 立即空指针调用崩溃。当前不可达仅因为 `perf_create` 返回 NULL —— **不是**因为 vtable 完整 | `pan_kmod.h:1030` `:1043` `:1049`（三处均无 null 检查，对比 `:724` `:781` `:817` 的可选 ops 有检查）；`kbase_kmod.c` vtable 缺这 3 项 | 已验证（静态）；**改 perf 前必须先补齐这 3 个 op 或加门** |
| 2026-10-18 | 厂商 kbase **没有 dma-buf exporter** ⇒ `bo_export` 在本内核上**不可能**实现（不是没做，是做不了） | 全 vendor 树 `grep dma_buf_export` 只命中 `drivers/base/arm/dma_buf_test_exporter/`（ARM 测试模块），`mali_kbase` 零命中；`kbase_mem_import` 只处理 **import** 方向（UMM type 2 / USER_BUFFER type 3），无对称 export 路径 | `mt6991-kernel-src/mali-r49p1/drivers/gpu/arm/midgard/mali_kbase_mem_linux.c:2103`；全树 grep | 已验证（静态 grep，2026-10-18） |
| 2026-10-18 | `query_timestamp` 即使接 `GET_CPU_GPU_TIMEINFO`(nr 50) 也**缺 cycles→ns 的换算来源** | vendor uapi 只有 `BASE_TIMEINFO_{MONOTONIC,TIMESTAMP,CYCLE_COUNTER}_FLAG`，**没有** timestamp frequency 这个 gpuprop 键；`grep -rn "TIMESTAMP_FREQUENCY\|TIMESTAMP_PERIOD"` 在 midgard 全树零命中。panthor 的 frequency 来自 panthor 自己的 props，kbase 无对应物 | 同上 grep；`pan_kmod.h:256-259` 需要 `timestamp_frequency` 才能算 `timestamp_cycles_to_ns_factor` | 待验证（需 RE 或固定假设，别直接抄 panthor） |

| 2026-10-18 | **`kbase_mem_import` 有两条返回形状**，只处理 cookie 分支会漏掉一半：`(*flags & (SAME_VA\|NEED_MMAP)) != 0` ⇒ 返回 cookie（需 mmap 才进 GPU VA 空间）；**否则内核自己 `kbase_add_va_region`/`kbase_gpu_mmap` 并返回真实 GPU VA** | 反编译 `kbase_mem_import`：分支判据是 `(*param_7 & 0x6000) == 0` ⇒ `add_va_region`/`gpu_mmap`，返回 `reg->start_pfn << 12`；`else` 走 `pending_regions` cookie 路径，返回 `slot*0x1000 + 0x40000`。64 位客户端因 `kbase_mem_from_umm` 强制 NEED_MMAP **总是** cookie 分支，但内核 ABI 两条都存在 | `mali_kbase_mt6991_r49.ko` @ `kbase_mem_import`（REA/Ghidra 伪代码，发行版二进制）；回调 oracle：只按 NEED_MMAP 判定 ⇒ `kbase_kmod_test` 在 `direct != NULL` 处 abort | 已验证（已修，两条分支都实现） |
| 2026-10-18 | imported UMM 的 **CPU 映射必须 `PROT_NONE`** —— 这是 mmap 一致性检查的**唯一可通过形状** | `kbase_context_mmap` 的检查是 `(reg CPU_RD \|\| !VM_READ) && (reg CPU_WR \|\| !VM_WRITE)`。`PROT_NONE` 两个条件恒真；`PROT_READ` 要求 import flags 带 `CPU_RD`，而那样也毫无用处 —— fault handler 对 `IMPORTED_UMM && free_on_close` 直接 SIGBUS | 反编译 `kbase_context_mmap` @ `0x1509d0` 的 `LAB_00150b38`（`inconsistent VM flags`）；`kbase_cpu_vm_fault` @ `0x15032c` 首分支 `plVar9[10]==1 && *(uVar10+0x2c)!=0` ⇒ warn 后 `iVar3=2` = VM_FAULT_SIGBUS | 已验证（静态，发行版 `.ko`） |
| 2026-10-18 | 发行版 `.ko` **确认无 dma-buf exporter**，但**有完整 importer** | `nm` 里 `dma_buf_get/attach/detach/map_attachment_unlocked/put` 全是 `U`（外部引用），无 `dma_buf_export`；同时 `kbase_api_mem_import`、`kbase_mem_import`、`kbase_mem_umm_map(_attachment)` 都存在。⇒ `bo_export` 保持 `ENOTSUP` 是**内核能力边界**，不是欠账 | `nm mali_kbase_mt6991_r49.ko`（10 个 dma_buf 符号，全 `U`）；`kbase_mem_import` 的 callees 含 `dma_buf_get`/`dma_buf_attach`/`kbase_mem_umm_map_attachment` | 已验证（发行版二进制，非仅源码） |

> M5a / M5b-1 真机实测硬件事实（设备身份、ioctl 观测、映射生命周期、用户提交的 kick）见
> `/home/zl/github/mesa-port-backups/RE-NOTES.md` §8 与 §10；本台账不重复抄录。

---

## 更正记录

| 日期 | 原条目 | 更正为 | 依据 |
|---|---|---|---|
| _(暂无)_ | | | |
