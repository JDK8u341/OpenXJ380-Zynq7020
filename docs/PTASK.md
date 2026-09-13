# PTASK —— Task 系统（调度 / 线程 / 生命周期）的发现与重新规划

> **这份文件的用途**：把"源 OS 的 task 子系统到底长什么样"与"我们 ARM 侧照它做到哪一步"
> 一次性写清，供**压缩上下文后**直接续接。凡是我**亲自在源树里核过**的结论都带
> `文件:行号`，可以照行号复核；凡是**没核实**的都显式标了"未核实"。
>
> 相关文档：
> - `docs/ZYNQ7020_PORT_PLAN.md` §0.5.7 = 阶段续接点（本文件是它的"task 专题分册"）
> - `arch/arm32/README.md` §11 = ARM 侧每个模块的设计/踩坑记录
>
> 基线：源 OS 仓库 `https://github.com/xingji-studio/OpenXJ380`，`main` = **`08e5c9c`**
> （= 我们这条分支的基点；2026-02 拉取后 `main` 仍停在这里）。
> ⚠ 本地 `kernel/**`、`include/**` 与上游 `08e5c9c` **一字未改**（`git diff HEAD -- kernel include` 为空），
> 所以本文件引用的行号就是上游的行号。

---

## 1. 一句话现状

**M0–M4-10 已完成并板上验证（自检 86 passed / 0 failed，七组破坏性 A/B 中的六组已建）。**
**M4-11（mutex + 线程退出/回收）尚未开始 —— 而调研发现它依赖一个更基本的问题：**
**task 系统的"线程生命周期"在源 OS 里是"没接完"的状态，我们需要先决定照到什么程度。**

---

## 2. 源 OS 的 task 子系统：模块与语义（已核实）

### 2.1 模块分布

| 模块 | 位置 | 行数 | 职责 |
|---|---|---|---|
| 调度器 | `kernel/task/scheduler.cpp` | 594 | 选取（全表扫描）+ `timer_handle` + `add_task`/`remove_task` + sleep/wake |
| PCB/TCB | `kernel/task/pcb.cpp` | 2733 | 线程/进程生命周期：`create_kernel_thread:2177`、`kill_thread:447`、`kill_thread0:462`、`process_exit:494`、`kill_proc:244`、`kill_proc0:359` |
| 回收者 | `kernel/task/reaper.cpp` | 66 | 常驻内核线程，巡检 `kernel_group->child_pcb`（**进程**）|
| 互斥锁 | `kernel/task/mutex.cpp` | 189 | ★ **yield 型**互斥（见 2.6）|
| 进程 IPC | `kernel/task/ipc.cpp` | 51 | 进程消息队列；等待用 `do { sleep_ns(1ms); recv(); } while` **轮询** |
| poll | `kernel/task/poll.cpp` | — | 未读（M7 相关；若要移植，注意它会持 `tcb_t`）|
| 每核结构 | `include/smp/smp.h:35-49` | — | `PROCESSOR_INFO{ current_task, scheduler_queue, scheduler_ticks, iter_node }` |
| 等级枚举 | `include/task/pcb.h`（宏，非 enum）| — | ★ `TASK_KERNEL_LEVEL 0` / `TASK_IDLE_LEVEL 1` / `TASK_APPLICATION_LEVEL 2` ★ |

### 2.2 调度语义（M4-8.4 / M4-9 / M4-10 期间逐条核过）

| 事实 | 出处 |
|---|---|
| 常量：`TIME_SLICE=4`（tick）、`EEVDF_BASE_SLICE_NS`、`WAKEUP_CREDIT`、`SLEEPER_CREDIT`、`DEFAULT_WEIGHT=1024` | `scheduler.cpp:17-24` |
| `vruntime_delta()` 是**恒等式**（乘除同一个常数）⇒ 实际没有权重 | `scheduler.cpp:232` |
| 队列 `lock_queue` 是**插入序 FIFO，不排序** | `kernel/lock_queue.cpp:100`（`queue_append_node`）|
| 选取 = **全表扫描 + avg_vruntime 闸门 + fallback + idle 兜底** | `scheduler.cpp:316-362` |
| **唤醒发生在扫描里**（`queue_average_vruntime` → `wake_sleeping_task`）| `scheduler.cpp:256-287` |
| `is_task_schedulable` 排除：NULL / current / 非 RUNNING,START,CREATE / **IDLE** / `parent_group==NULL` / 组状态 | `scheduler.cpp:173-185` |
| 兜底链：`result = best ? best : (current 可运行 ? current : idle)` | `scheduler.cpp:358` |
| 计费只对 `status==RUNNING && task_level != IDLE` | `scheduler.cpp:396-411` |
| `scheduler_yield()` = `scheduler_ticks = TIME_SLICE; int $32` | `scheduler.cpp:460-465` |
| `add_task`：**全局** `scheduler_lock` → 挑"队列最短的核"（**严格小于** ⇒ 平局给核号小的；`APP` 级**跳过整个扫描**）→ `init_task_eevdf_entity(base - WAKEUP_CREDIT)` | `scheduler.cpp:526-560` |
| `remove_task`：按 `task->cpu_id` 摘节点（失败则全核扫一遍），持**全局** `scheduler_lock` | `scheduler.cpp:562-594` |
| BSP 等所有 AP 登记完 idle 才继续：`while (scheduler_is_ready == xsi->cpu_count);` | `main.cpp:581-585` |
| 源 OS **没有**周期性负载均衡/迁移（`balance|migrat|load_avg|steal` 零命中）| 全树 |

### 2.3 队列是"**全部线程的名册**"，不是"就绪链表"

睡眠（WAIT）、挂起、甚至 `current` 都**留在队列里**，靠 `is_task_schedulable` 排除。
⇒ 直接后果（M4-10 实测踩到）：**"挑队列最短的核"比的是名册长度**，于是一个核名册长、
新线程就**成批**落到另一个核 —— 这不是 bug，是这套规则 + 名册模型的必然结果。

### 2.4 ★ idle：两个核的 idle **行为不一样**（源 OS 的一处漏赋值）★

| | 构造 | `task_level` 实际值 | 后果 |
|---|---|---|---|
| **BSP idle** | `main.cpp:520-541`：`aligned_alloc` + `memset(0)`，逐个字段赋值，**从不赋 `task_level`** | ★ **0 = `TASK_KERNEL_LEVEL`** ★ | ① `kill_thread` 不拒绝它；② `is_task_schedulable` **不排除它** ⇒ 它是**正常候选**，而它的 `context0.rip` 是 0（只赋了 `rsp`）⇒ 被选中时 `timer_handle` 走"不切换"那一支（**白做一次派发**）；③ 被 EEVDF **计费** |
| **AP idle** | `smp.cpp:152-181`：`alloc_zeroed_tcb()` 后**显式** `task_level = TASK_IDLE_LEVEL` | **1** | 被排除在候选之外；只做最后兜底；不计费 |

- 两个 idle 的 `context0.rip` **都保持 0**（= "上下文无效"，第一次被切走时由 `change_proccess` 填上）；
- AP idle 的循环体就是 `cpu1_main` 里的 `while (true) pause`（`smp.cpp:178-181`）；
- **idle 会被切回来**：兜底链（`scheduler.cpp:358`）就是它的入口 —— 计划里曾写"只被切走、
  不被切回"，**那句是错的**（M4-10 调研纠正，板上 `back=1` 验证）。

### 2.5 ★★ 线程退出与资源释放：源 OS 是**两段式，而且第二段对内核线程不可达** ★★

**第一段（垂死线程自己）**：

```
内核线程入口 return
   → process_exit()                      pcb.cpp:494-505   ← create_kernel_thread 压在
                                                              每根线程栈上的返回地址（pcb.cpp:2237）
        write_serial_string("Kernel thread exit, Code: ")   pcb.cpp:498
        kill_thread(get_current_task())                     pcb.cpp:501
        open_interrupt; while (true) hlt;                   pcb.cpp:502-504
   → kill_thread(task)                   pcb.cpp:447-458
        if (task_level == TASK_IDLE_LEVEL) { "Cannot stop kernel thread."; return; }   :450-454
        task->status = DEATH;                                                          :456
        // kill_thread0(task);   ← ★ 注释掉的 ★（注释自述「要用的解耦，但可能要加锁」）   :457
```

⚠ 补一个精度:`process_exit()` 也被**用户线程的建栈失败路径**直接调用
（`pcb.cpp:732` / `751` / `758`,都在 `switch_task_to_user_mode` 里）——
也就是说它是"**这个线程到此为止**"的通用入口,不只服务内核线程的 return。

**第二段（释放，由**别人**做）**：

```
kill_thread0(task)                       pcb.cpp:462-492
    free_frames(task->kernel_stack …)     :488
    free_frames(task->syscall_stack …)    :490
    remove_task(task)                     :491
  ↑ 全树唯一调用者：kill_proc0()          pcb.cpp:376
  ↑ 全树唯一调用者：kill_proc()           pcb.cpp:288
  ↑ ★ kill_proc() 对 kernel_group 直接 return ★  pcb.cpp:247-251（"Cannot kill System process."）
```

**而内核线程的 `parent_group` 就是 `kernel_group`**：
`create_kernel_thread(…, pcb)` 的 `pcb == NULL ⇒ target_group = kernel_group`（`pcb.cpp:2186`），
随后 `new_task->parent_group = target_group`（`:2205`）、`queue_enqueue(target_group->thread_queue, new_task)`（`:2247`）。
内核自带的线程全部传 `NULL`：`main.cpp` 的 idle、`reaper.cpp:65`、`kmod/netserver/netserver.cpp:614`、
`kmod/netserver/arch/sys_arch.cpp:256`。

⇒ **结论（这就是"内核线程永不回收"的体现）**：

> 对挂在 `kernel_group` 上的线程，`kill_thread0()` 的 488/490/491 三句
> **没有任何可达路径**：唯一的调用链被 `kill_proc()` 的 `Cannot kill System process.` 挡死。

**旁证三条**：

| 旁证 | 出处 |
|---|---|
| 回收者 `reaper_thread()` 遍历的是 **`kernel_group->child_pcb`（进程）**，不是 `thread_queue`（线程）| `reaper.cpp:35`、`:48-61` |
| 内核线程挂在 `kernel_group->thread_queue` 上 | `create_kernel_thread` `pcb.cpp:2247` |
| 连 OOM/缺页那条路也走不通：`kill_proc(current_task->parent_group, …)` 对内核线程同样被拒；紧接着那段只处理 `TASK_APPLICATION_LEVEL` | `page.cpp:457`、`:463-469` |

**例外（必须写清，否则结论过宽）**：如果内核线程是**挂在真实进程上**造的
（`create_kernel_thread(…, pcb != NULL)`），那么**那个进程被拆时**会走
`kill_proc0` → `pcb.cpp:363-378`（逐个 `kill_thread0(thread)` 再 `free(thread)`）⇒ **这类线程会被释放**。
⇒ 准确措辞：**"挂在 `kernel_group` 上的线程永不回收"**，不是"所有内核线程"。

### 2.6 ★ mutex：源 OS 是 **yield 型**，不是"可睡眠"型 ★

`mutex.cpp:21-22` 的作者原注释：

> 当前调度器里的 WAIT 语义并不适合通用互斥，这里采用 **yield 型互斥**：
> 获取失败时主动让出时间片，但不把线程切到 WAIT，避免线程在未持锁时继续执行。

实现 = `spin_lock(状态)` 判一下 → 拿不到就 `scheduler_yield(); cpu_relax();` 重来。
其余语义（都要照抄）：递归计数 / 非递归自锁 ⇒ `-EDEADLK` / 解锁非持有者 ⇒ `-EPERM` /
销毁加锁中的锁 ⇒ `-EBUSY` / 已销毁 ⇒ `-EINVAL` / `trylock` 被占 ⇒ `-EBUSY`。
⚠ `mutex_t.wait_queue` 字段**从头到尾没人用**（只在 create/destroy 里建/销毁）——
是"本来打算做可睡眠、后来改成 yield"的化石。

### 2.7 `tcb_t` 的五类长期持有者（释放 TCB 必须面对的兼容面）

| 持有者 | 位置 | 何时注销 |
|---|---|---|
| 每核 `current_task` | `include/smp/smp.h:43` | 切走时 |
| 调度队列节点 `lock_node->data` | `scheduler.cpp` | `remove_task()` |
| 进程线程名册 `pcb->thread_queue` | `pcb.cpp:2247` | 进程拆卸 |
| **`mutex_t.owner`** | `include/mutex.h:17` | ★ **从不注销** ★ |
| ★ **管道等待名单** `pipe_info_t.blocking_read/blocking_write` ★ | `include/pipe.h:6-22`，用法 `driver/fs/vfs/pipefs.cpp:12-66` | 管道被唤醒/关闭时 |

**源 OS 靠两件事让"不注销"也能成立**：

1. **延迟释放**（TCB 活到进程拆卸）；
2. ★ **顺序** ★ `kill_proc0` **先** `close_process_file_table(pcb)`（`:370` ⇒ 关 fd ⇒ 管道 close ⇒
   `pipe_wake_waiters` 清空那些外部名单）**再** `kill_thread0` + `free(thread)`（`:372-378`）。

⇒ 也就是说：**"更晚释放"= 用"释放时没人在意它"换掉了"在每个持有者那里写注销"**。
这是源 OS 这套写法真正的代价与收益。

### 2.8 用户可见语义（M7 相关，先记下）

- `alloc_tid()` 是**单调计数器**（`pcb.cpp:48-50`），**没有 free、不复用** ⇒ 不存在"tid 复用导致别名"；
- XAPI 里**没有 `pthread_join`**（全树零命中）⇒ 少一条"TCB 必须活到 join"的理由；
- `wait4` 是**进程级**（`sys.cpp:579-588`）⇒ 线程 TCB 的存活期与它无关。

---

## 3. 我们 ARM 侧的现状（与源 OS 的对照）

| 项 | 源 OS | 我们（M4-10 末）| 处置 |
|---|---|---|---|
| 队列 | FIFO 名册 + 全表扫描 | ✅ 同（M4-8.4 对齐）| — |
| 每核队列/计数器/idle | ✅ | ✅（M4-10）| — |
| 选核 | 挑最短队列（严格小于；APP→CPU0）| ✅ 纯逻辑层 `sched_pick_cpu()`，宿主穷尽测 | — |
| **idle 的 level** | BSP = 0（漏赋值）、AP = 1 | ★ 两个都显式 = 1 ★ | **记偏离**：与源 OS **意图**一致、与其 BSP **实际行为**不同 |
| 线程退出 | 两段式；第二段对 kernel_group 不可达 | ❌ 只有 `sched_park_self()`（永久 WAIT）| **D14，M4-11** |
| 释放栈/TCB | `kill_thread0`（唯一路径，被 `Cannot kill System process.` 挡住）| ❌ 无 | **D14，M4-11** |
| 回收者 | `reaper_thread`（只扫进程）| ❌ 无 | M4-11 |
| 全局 `scheduler_lock` | 有（罩 add/remove）| ❌ 刻意没引（M4-10 时没有第二个用户）| M4-11 有 remove 时补 |
| mutex | yield 型 | ❌ 无（串口排他用"关调度"）| **D13，M4-11.1** |
| 可睡眠互斥/信号量 | ★ **不存在** ★ | 只有 `sched_sleep_ns`/`sched_wake_task` | **不发明**（从清单去掉）|
| 栈池 | 每线程 `alloc_frames(KERNEL_STACK_SIZE)`，进程拆卸时还 | 固定 32 槽，**只借不还** | 见 §4 |
| `sched_park_self` | ★ 源 OS **没有**这个函数 ★ | 自检探针的便利设施（6 处）| M4-11.3 退场 |

---

## 4. ★ 重新规划：为什么"task 系统"需要再想一遍 ★

### 4.1 前提差异（这是全部问题的根）

| | 源 OS | 我们 |
|---|---|---|
| 内核线程的用途 | **永久基础设施**：idle、Process Reaper、驱动 worker、模块 worker（`netserver`）| **临时探针**：自检每个相都要造几个、跑完就该消失 |
| 线程数量 | 个位数、启动后基本不变 | 启动一轮就 30 个左右（**已经贴满 32 槽池**）|
| 因此"退出后不回收" | 几乎无感（就那么几个） | **致命**：`kstack_headroom` 会一直降到创建失败，而失败是静默的（返回 NULL）|

⇒ **我们的自检模型与源 OS 的线程模型不同**。源 OS 里"内核线程永久"是成立的假设；
我们这里**不成立** —— 所以"线程退出 + 回收"在我们这边**不是可选优化，而是必需**。
这一条比"照不照源 OS"更根本：**它是我们的用法差异，必须记在偏离清单最上面。**

### 4.2 待作者确认的问题（已整理成可直接发 issue 的正文）

> **Q1** 挂在 `kernel_group` 上的内核线程，退出（`process_exit` → `kill_thread` → `DEATH`）之后，
> 它的**内核栈与 TCB 由谁释放**？还是有意不释放？
> 证据链：`kill_thread0` 只被 `kill_proc0` 调（`pcb.cpp:376`）→ `kill_proc0` 只被 `kill_proc` 调（`:288`）
> → `kill_proc` 对 `kernel_group` 直接 return（`:247-251`）；而内核线程的 `parent_group` 就是
> `kernel_group`（`:2186`/`:2205`/`:2247`）。
>
> **Q2** `pcb.cpp:457` 那句被注释掉的 `kill_thread0(task)`，注释写「要用的解耦，但可能要加锁」——
> 当时卡在哪把锁（`scheduler_lock` / `thread_queue->lock` / `create_thread_lock`）？
>
> **Q3** 反复 `create_kernel_thread`（或模块反复起 worker）会不会耗尽内核栈内存？
> 有没有计划把回收接进 `reaper_thread`，让它也扫 `thread_queue`（而不只是 `child_pcb`）？
>
> **Q0**（BSP idle 漏赋值）`kernel/main.cpp` 造 BSP idle 时（约 521-539 行）没有给
> `idle_thread->task_level` 赋值，它保持 memset 的 0 = `TASK_KERNEL_LEVEL`；而 AP idle
> （`smp.cpp:154`）设的是 `TASK_IDLE_LEVEL`。于是 BSP idle ①不会被 `kill_thread` 拒绝
> ②`is_task_schedulable` 不排除它，它会作为候选被选中，而 `context0.rip == 0`，
> `timer_handle` 会因此放弃这次切换 ③还会被 EEVDF 计费。**这是漏了一行赋值吗？**
>
> **Q0b** 顺便确认意图：`kill_thread` 那句 `"Cannot stop kernel thread."` 只对
> `TASK_IDLE_LEVEL` 生效 —— 也就是**只有 idle 不可停**、普通内核线程（level 0）可以停，对吗？
> （这样 `process_exit` 的 `"Kernel thread exit"` 日志才说得通。）

**两种回答各自的处置**：

| 作者回答 | 处置 |
|---|---|
| "允许结束，回收是待办" | M4-11.2 按两段式补回收，偏离清单写"**补完上游未接的一环**（`pcb.cpp:457` 自述'要用'）"|
| "内核线程本应永久" | 我们**仍然需要**回收（§4.1 的用法差异），但偏离清单要写成**更重的两条**：①"源 OS 假设内核线程永久，我们的自检探针是临时的" ②"因此我们引入了源 OS 没有的『临时内核线程』概念与回收路径" |

### 4.3 方案（推荐 A）

**方案 A（推荐）：两段式 + reaper 覆盖内核线程**

1. 垂死线程（照源 OS）：`status = DEATH` → `sched_yield()` → `for(;;) arch_wfi()`
   （**它在自己的栈上，不能自己释放栈** —— 这一点源 OS 与我们都一样）；
2. 回收者（照源 OS 的结构，**覆盖面扩大**）：遍历**两个核的调度队列**（因为 D5 还没有进程组），
   挑 `status == DEATH` **且不在任何核 `current_task` 上**的线程 ⇒ `remove_task`（摘节点）
   + `kstack_free`（还栈）+ `heap_free`（还 TCB）；
   ⚠ 那两条可见性条件顺带关掉竞态：选取时 `is_task_schedulable` 已排除 DEATH
   ⇒ "被选中"与"被释放"不会重叠；
3. 补上源 OS 的**全局 `scheduler_lock`**（`add` 与 `remove` 到这里才真的并发）；
4. 对照组窗口内**暂停回收**（见 §5 耦合项）。

**方案 B（不推荐）：严格照源 OS 的既成事实**（内核线程只标记不回收）
⇒ 必须把 `KSTACK_SLOTS` 从 32 扩到 ~64，并接受"每轮自检永久吃掉槽"、`kstack_headroom` 迟早变红。

**判据（板的）**：
- `used_slots` **回落**（`g_kstack.used_slots` 在启动末尾小于峰值）；
- 名册长度不再单调增长（`sched_cpu_runq_len(0/1)` 峰值后下降）；
- `kstack_headroom` **变大**（当前 >= 4 是"贴边"，回收后应当明显富余）；
- 第七组破坏性 A/B：**关掉回收** ⇒ 线程照常退出（DEATH）但**栈槽不回落**、名册只增不减。

---

## 5. 耦合项清单（做 11.2 时**必须**一起处理，否则会把已验证的东西弄坏）

| # | 耦合项 | 为什么 |
|---|---|---|
| 1 | 对照组的 `sched_ctx_snapshot_all/restore_all` | 它的**硬件性前提**是"窗口内没有线程被创建或销毁"（它按**遍历顺序**配对还原）。回收线程一上线就破坏该前提 ⇒ 会把 A 的 ctx 写进 B |
| 2 | 对照组清理里的 `ca->status = WAIT` | 探针若已跑完并被回收，这是 **use-after-free** |
| 3 | 饥饿监视器的观察名单 `g_starve_watch[]` | 它持 `tcb_t`；被监视线程若退出并被回收 ⇒ 悬空。必须"**先撤名单再允许回收**"（当前相 5 收尾是"置 WAIT + 撤名单"，顺序已经对；但要写成不变量）|
| 4 | `mutex->owner` | 源 OS 不注销 owner；我们引入"线程会退出"后，**持锁线程退出**第一次变得可达 ⇒ 要么在退出时拒绝/报警，要么记退化 |
| 5 | 两个 idle | **永远不能回收**（源 OS 也拒绝停 idle）⇒ 判据里要显式排除（`TASK_IDLE_LEVEL`）。⚠ 我们的两个 idle 都是 level 1 ✅（源 OS 的 BSP idle 是 0，不能照抄那个值）|
| 6 | `percpu_t.cur_vfp_*` | 它总是指向"当前任务"，随 `sched_set_current` 更新 ⇒ 只要"不是任何核的 current"就安全（不需要额外条件）|
| 7 | JTAG 的 `walkq.tcl` | 名册会开始变短 —— 这是**好事**（能直接看到回收是否发生）|

---

## 6. 已确定的实施顺序（M4-11 分三步）

| 步 | 内容 | 判据 |
|---|---|---|
| **11.1** | `mutex`（源 OS yield 语义）：纯状态机放纯逻辑层 + 两个钩子（取当前任务 / 让出），宿主穷尽测；**`console_excl` 从"关调度"换成它（D13 结案）** | 宿主：递归/EDEADLK/EPERM/EBUSY/EINVAL 逐条；板上：`sched_off_total_ns` 几乎不再增长 ⇒ 饥饿监视器的 `skipped` 掉到 ~0 |
| **11.2** | **线程退出 + 回收**（方案 A）+ `remove_task` + 全局 `scheduler_lock`；`thread_finish()` 改走退出（**D14 结案**）| 见 §4.3 的四条判据 + 第七组 A/B |
| **11.3** | `sched_park_self` 退场（源 OS 没有这个函数）、D13/D14 结案文档、偏离清单更新 | — |

**做之前先做的两件事**：
1. 把 §4.2 的问题发给作者（等他回答决定偏离清单怎么写）；
2. 无论作者怎么答，§4.1 的"用法差异"都是既定事实 ⇒ 回收照做。

---

## 7. 这份文件里**没有**核实的东西（别当成已知）

- `kernel/task/poll.cpp` 的细节（它是否持 `tcb_t`、睡眠语义）—— **未读**；
- `process_fork`/`execve` 路径对 TCB 的所有权转移（M7 才用得上）；
- 兄弟仓库 `xingji-studio/OpenXSKernel` 已确认**不是**这套 task 代码的来源（只有 10 个提交、不含 task 目录）；
- 议题/PR 层面：31 条议题 + 50 个 PR 我都列过标题，**没有一条**讨论线程生命周期；
  最接近的 #68（内核代码审查）只列了 P1-P5/F1-F5，**没有**这一项。
  ⚠ 但我**没能逐条读所有评论**（GitHub API 未认证有速率限制）⇒ "没人讨论过"这句的强度是"我查过的范围内没有"。
- ⚠ 中文议题搜索会**把词拆开**匹配（我搜「内核线程」时命中的 5 条经逐条阅读确认是假阳性）——
  不要因为"搜索有命中"就以为有人讨论过。
