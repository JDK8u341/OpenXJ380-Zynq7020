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

**M0–M4-11 全部完成并板上验证（板级自检 97 passed / 0 failed，八组破坏性 A/B 全部检出）。**
M4-11 的三步都结掉了：**11.1**（串口排他换成源 OS 的 yield 型互斥，D13）、
**11.2**（线程退出路径 `DEATH` + 让出 + `wfi`，D14）、**11.3**（`sched_park_self` 退场，并入 11.2）。

**按 §4.0 的决定：不回收。** 池 32 → **64** 槽（实测一次启动的**真实峰值 33**），
`kstack_peak_used` / `kstack_headroom` 是**永久容量判据**。

★ 11.2 期间炸出三个坑（51/52/53），其中两个是**先前就潜伏**的 ——
见计划 §0.5.5 与 README 的 M4-11.2 一节。**下一步待用户拍板**：
用户态归属哪个阶段（计划 §0.5.7 末尾）、心跳区扩容（计划 §0.5.8）。

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

> 状态列已更新到 **M4-11.1 之后**（§4.0 决定"不回收"改变了下面两行的处置）。

| 项 | 源 OS | 我们 | 处置 |
|---|---|---|---|
| 队列 | FIFO 名册 + 全表扫描 | ✅ 同（M4-8.4 对齐）| — |
| 每核队列/计数器/idle | ✅ | ✅（M4-10）| — |
| 选核 | 挑最短队列（严格小于；APP→CPU0）| ✅ 纯逻辑层 `sched_pick_cpu()`，宿主穷尽测 | — |
| **idle 的 level** | BSP = 0（漏赋值）、AP = 1 | ★ 两个都显式 = 1 ★ | **记偏离 D16**：与源 OS **意图**一致、与其 BSP **实际行为**不同。⚠ 这个偏离**有连锁后果**：见 D17 |
| 线程退出 | 两段式；第二段对 kernel_group 不可达 | ✅ **已同源**：`sched_thread_exit()` = `DEATH` → 让出 → `wfi`（`thread_finish()` 走它）| **D14 已结案（M4-11.2）**；`sched_park_self()` 已删（源 OS 没有它）|
| 释放栈/TCB | `kill_thread0`（唯一路径，被 `Cannot kill System process.` 挡住）| ❌ 无 | **D14：决定不做**（尊重源 OS）；代价是池只增不减 ⇒ 池 32→64 + 容量判据（实测峰值 33）|
| 回收者 | `reaper_thread`（只扫进程；且僵尸不在它的条件里，链本身是断的）| ❌ 无 | **不做**（§4.0）|
| 全局 `scheduler_lock` | 有（罩 add/remove）| ❌ 刻意没引（M4-10 时没有第二个用户）| **不做**：没有 remove 就没有第二个用户（D10/D11 的规矩）|
| mutex | yield 型 | ✅ **已有**（`src/mutex.c` 纯层 + `src/mutex_kern.c` 钩子），串口排他用的就是它 | **D13 已结案（M4-11.1，`25f46ad`）**；偏离 D17（等锁那一下睡一个 tick）|
| 可睡眠互斥/信号量 | ★ **不存在** ★ | 只有 `sched_sleep_ns`/`sched_wake_task` | **不发明**（从清单去掉）|
| 栈池 | 每线程 `alloc_frames(KERNEL_STACK_SIZE)`，进程拆卸时还 | 固定 32 槽，**只借不还** | 见 §4 |
| `sched_park_self` | ★ 源 OS **没有**这个函数 ★ | 自检探针的便利设施（6 处）| M4-11.3 退场 |

---

## 4. ★ 重新规划：为什么"task 系统"需要再想一遍 ★

### 4.0 ★★ 决定（2026-09-13 拍板，此后的实施以本节为准）★★

> **决定：不改源 OS 的行为。D13 转 yield 型互斥；D14 保持"只标记 DEATH、不释放"。**
> 也就是说：**不引入源 OS 没有的回收器**（原 §4.3 的方案 A 作废），照 §4.3 的方案 B 走，
> 但把它的代价（池只增不减）**变成可观测的判据**而不是"迟早会红"。

由此产生三条**必须一起做**的事（写在这里，免得实施时只看实现不看约束）：

| # | 事项 | 为什么 |
|---|---|---|
| 1 | **线程退出路径要真的存在**（`status = DEATH` → 让出 → `wfi`），但**不回收** | 源 OS 对内核线程的真实语义是"**可以标记为死，但永远不释放**"（PTASK §2.5/§9.3）。D14 现在的毛病不是"没回收"，而是**尾部那段终止循环不可达**（自检探针走的是 `sched_park_self()` = 永久 WAIT）—— 那不是源 OS 的语义 |
| 2 | **池容量必须按"不回收"来定**，并**每次启动都把用量打出来** | 源 OS 的内核栈来自页分配器（1 MiB/个，无上限），我们是固定 32 槽池（1 MiB/个）。在"不回收"前提下，**池上限 = 每次启动能创建的内核线程数上限**；启动自检本身已经用到 ~30/32 |
| 3 | 偏离清单要写成**用法差异**，不是"偷懒" | §4.1 那一条：源 OS 假设内核线程是永久基础设施；我们的自检探针是**临时**的。这个差异不会因为"不回收"而消失，只会变成一条**长期约束**：任何"每请求一个线程"的用法都得先解决容量 |

**§4.2 的五个问题仍然可以问作者，但它们不再阻塞实施** —— 答复只影响"偏离清单怎么写"
（"补完上游未接的一环" vs "我们把上游的'永久'假设用在临时线程上"），不决定做不做。

### 4.1 前提差异（这是全部问题的根）

| | 源 OS | 我们 |
|---|---|---|
| 内核线程的用途 | **永久基础设施**：idle、Process Reaper、驱动 worker、模块 worker（`netserver`）| **临时探针**：自检每个相都要造几个、跑完就该消失 |
| 线程数量 | 个位数、启动后基本不变 | 启动一轮就 30 个左右（**已经贴满 32 槽池**）|
| 因此"退出后不回收" | 几乎无感（就那么几个） | 会一直降到创建失败，而失败是静默的（返回 NULL）|

⇒ **我们的自检模型与源 OS 的线程模型不同**。源 OS 里"内核线程永久"是成立的假设；
我们这里**不成立**。按 §4.0 的决定，这个差异**不用"回收"来消除**，而是：
**限量 + 可观测**（线程总数有界、池用量每次启动都打出来、`kstack_headroom` 从
"D14 的临时哨兵"升级成**永久容量判据**）。

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
| "允许结束，回收是待办" | 偏离清单写"**我们知道上游这里没接完**（`pcb.cpp:457` 自述'要用'），但按 §4.0 的决定**不替它接**；代价是池只增不减，靠 §4.0 的容量判据兜住" |
| "内核线程本应永久" | 偏离清单写**用法差异那一条**：源 OS 假设内核线程永久，我们的自检探针是临时的 ⇒ 我们把"永久"的假设用在临时线程上，于是必须有容量上限与可视化 |

⚠ 两种回答都**不再改变实施内容**（§4.0）：不回收是决定，不是待定。

### 4.3 方案（★★ 已按 §4.0 的决定选 **B** ★★）

**方案 A（已作废）：两段式 + reaper 覆盖内核线程** —— 原推荐方案，需要引入
源 OS 没有的回收器、`remove_task`、全局 `scheduler_lock`，以及 §5 那一整张耦合项清单。
按 §4.0 的决定不再采用；方案本身留在这里，作为"将来容量真的不够时"的备选。

**方案 B（★ 采用 ★）：严格照源 OS 的既成事实（内核线程只标记不回收）**

1. 退出路径照源 OS 两段式的**第一段**：`status = DEATH` → `sched_yield()` → `for (;;) arch_wfi()`
   （`pcb.cpp:494-505` 的 `process_exit`）；**没有第二段** —— 这正是源 OS 的既成事实；
2. 于是**池容量就是硬约束**：`KSTACK_SLOTS` 由"每次启动的内核线程数上限"决定
   （启动自检已经用到约 30/32），用量与余量**每次启动都打出来**；
3. `sched_park_self()` 退场（源 OS 没有这个函数），`thread_finish()` 改走退出；
4. **判据（板的，★ 已全部落地 ★）**：
   - `kthread_exit_reached` —— 至少有一个线程真的走过退出路径（不是"编译过"而已）✓ `= 1`
   - `kthread_exit_death` —— 去**名册里数** `status == DEATH` 的线程（源 OS 的语义，
     **不是** WAIT）✓ `= 16`；这条同时是 §5 耦合项 3 的前提
   - `kthread_exit_refused` —— "有代码想停 idle" 的次数 ✓ `= 0`
   - `kthread_exit_lock_guard` —— §5 耦合项 4 那条绊线的**正向对照**（报告区间里
     kmain 正持着串口锁，问一句必须为真）✓ `= 1`；`kthread_exit_held_lock` ✓ `= 0`
   - `kstack_peak_used` / `kstack_headroom` —— 容量与余量，**永久判据** ✓ `20/33 of 64`
   - 破坏性 A/B（第八组）：**退一步用 `sched_park_self()`**（旧的 WAIT 挂起）⇒
     线程照样不动了，但名册里 `status` 是 WAIT 而不是 DEATH ⇒ `DEATH` 计数不涨 ✓ 检出
     （实测 `death 16→17` vs 对照组停在 17）。这条 A/B 证明的是
     "**退出语义真的接上了**"，而不是"线程恰好停住了"。

> （原方案 A 的细节 —— 垂死线程置 DEATH、回收者遍历两个核的调度队列挑
> "DEATH 且不在任何核 current 上"的目标、摘节点 + 还栈 + 还 TCB、补全局
> `scheduler_lock`、对照组窗口内暂停回收 —— 已随方案 A 一起作废，实施时
> 不需要它们；将来真要回收，再回 §5 的耦合项清单逐条处理。）

---

## 5. 耦合项清单（★ 方案 A 作废后，这张清单的适用面变了 ★）

⚠ **先说清现在的适用面**：§4.0 决定"不回收"之后，清单里的
**1 / 2 / 5 / 6 / 7 都不再需要处理**（它们全都是"线程真的会被释放"才成立的耦合），
**第 3 条降级成前提**（监视器名单里的线程不会变成"可运行却不动"的形态，
因为 DEATH 不再被当作可运行），**只有第 4 条要真的处理**（见下）。
留整张表是为了：将来若改主意做回收，这里是唯一的入口清单。

| # | 耦合项 | 为什么 | 现在 |
|---|---|---|---|
| 1 | 对照组的 `sched_ctx_snapshot_all/restore_all` | 它的**硬件性前提**是"窗口内没有线程被创建或销毁"（它按**遍历顺序**配对还原）。回收线程一上线就破坏该前提 ⇒ 会把 A 的 ctx 写进 B | 不适用（不回收）|
| 2 | 对照组清理里的 `ca->status = WAIT` | 探针若已跑完并被回收，这是 **use-after-free** | 不适用（不回收）|
| 3 | 饥饿监视器的观察名单 `g_starve_watch[]` | 它持 `tcb_t`；被监视线程若退出并被回收 ⇒ 悬空 | 降级为**前提**：退出 = DEATH，而 DEATH 不在可运行集合里 ⇒ 监视器跳过它（与现在跳过 WAIT 一样）。⚠ 仍要保证"**退出发生在撤名单之后**"这一顺序不变量 |
| 4 | ★ `mutex->owner` ★ | 源 OS 不注销 owner；引入"线程会退出"后，**持锁线程退出**第一次变得可达 | **要处理**：见 §6 的 11.1 —— 退出路径上带一条**绊线**（退出时若发现自己还持着串口锁就计数 + 报警），而不是发明源 OS 没有的"退出时注销 owner"机制 |
| 5 | 两个 idle | **永远不能回收** ⇒ 判据里要显式排除（`TASK_IDLE_LEVEL`） | 不适用（不回收）；但"**idle 不能退出**"这条照源 OS 保留（`kill_thread` 拒绝 level 1）|
| 6 | `percpu_t.cur_vfp_*` | 它总是指向"当前任务"，随 `sched_set_current` 更新 ⇒ 只要"不是任何核的 current"就安全 | 不适用（不回收）|
| 7 | JTAG 的 `walkq.tcl` | 名册会开始变短 —— 那是回收的证据 | 反过来变成判据：名册**只增不减**（不回收 ⇒ 名册就是"本次启动造过的所有线程"）|

---

## 6. 实施顺序（M4-11；★ 已按 §4.0 的决定修订 ★）

| 步 | 内容 | 判据 |
|---|---|---|
| ~~**11.1**~~ | ~~`mutex` + `console_excl` 换成它（D13 结案）~~ | **✅ 已完成（`25f46ad`）**。宿主：递归/EDEADLK/EPERM/EBUSY/EINVAL/让出次数逐值（`tests/test_arm32_mutex.py`，12 节）；板上 5 条判据全 PASS（`console_excl_wait=1`、`console_excl_sched_off=0ms`、`console_excl_alive=1`、两条绊线 0），饥饿监视器 `skipped` 1→**0**；第七组破坏性 A/B 检出（`+0ms/+19` vs `+1200ms/+0`）<br>★ 过程中新增**两个坑（48 全局计数 vs 真锁、49 照抄"让出"的前提）与一条偏离（D17 等锁睡一个 tick）**，还有一条判据方法论（坑 50：A/B 的"活着"观测量不能押在别人相位上）|
| ~~**11.2**~~ | ~~线程退出路径（★ 不回收 ★）+ `sched_park_self` 退场~~ | **✅ 已完成**。板上 97/0；六条判据全 PASS：`kthread_exit_reached=1`、`kthread_exit_death=16`（**去名册里数**出来的）、`kthread_exit_refused=0`、`kthread_exit_lock_guard=1`（绊线的**正向对照**）、`kthread_exit_held_lock=0`、`kstack_peak_used=20`（报告时刻）/ **33**（一次启动的真实峰值）；第八组破坏性 A/B 检出（`death 16→17` vs 对照组不涨）<br>★ 池 32 → **64**（`KSTACK_MAX_SLOTS` 的设计上限）：实测峰值 33 ⇒ 32 槽确实越界<br>⚠ 头两次上板都是"**报告全绿但整机在报告之后崩了**" ⇒ 坑 51（11.1 埋下的：把"修污染"的机制用到没有污染的场景）+ 坑 52/53（先前潜伏的每核指针哨兵 + 验证方法）|
| **耦合项** | **只剩 §5 的第 4 条**：`mutex->owner` —— 退出路径上带一条**绊线**（退出时若发现自己还持着串口锁就计数 + 报警）| 那条计数的判据恒为 0；它的**正向对照**在同一份报告里（故意触发一次让绊线响）|

**（原"做之前先做的两件事"已作废）**：§4.2 的五个问题仍然可以问作者，
但按 §4.0 的决定它们**不再阻塞实施** —— 答复只影响偏离清单怎么写。

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

---

# 第二部分：源 OS task 架构**全量扫描**（2026-09-13）

> **这一部分与 §2 的关系**：§2 是"结论摘要"，本部分是"**逐文件、逐行、逐调用点**的原始清单"。
> 目的是：M4-11 动手前，凡是"照源 OS"的地方都能**照行号复核**，凡是我们**不照**的地方都能说清代价。
>
> **基线复核（本次实测）**：`git ls-remote origin main` ⇒ `08e5c9c0b0de99b036242f2384706935e6d4a083`，
> 与本地 `origin/main` 一致；`08e5c9c` 的提交日期是 **2026-09-04**，到今天（2026-09-13）**没有前移**。
> （§2 里"2026-02 拉取"那句日期不准，以此处为准。）
>
> **本次亲手读过的源码**（不是 grep 抽样）：

| 文件 | 读法 | 行数（实测）|
|---|---|---|
| `kernel/task/pcb.cpp` | **全文** | 2733 |
| `kernel/task/scheduler.cpp` | **全文** | 594 |
| `kernel/task/mutex.cpp` / `reaper.cpp` / `ipc.cpp` / `poll.cpp` | **全文** | 189 / 66 / 51 / 52 |
| `kernel/lock_queue.cpp` | **全文** | 387 |
| `include/task/{pcb,scheduler,ipc,poll}.h`、`include/mutex.h`、`include/pipe.h`、`include/smp/smp.h` | **全文** | 252 / 16 / 51 / 69 / 29 / 35 / 64 |
| `kernel/main.cpp` 470-632、`kernel/smp/smp.cpp` 1-239 | 关键段 | — |
| `kernel/syscall/sys.cpp` 90-160 / 560-700 / 1320-1400 / 2505-2600 / 2790-2900 / 3056-3142 | 关键段 | — |
| `kernel/syscall/syscall.cpp` 150-249、`kernel/syscall/signal.cpp` 140-219、`kernel/user/user.cpp` 900-1045 | 关键段 | — |
| `kernel/intr/handler.S` 295-374、`driver/fs/vfs/pipefs.cpp` 1-200、`driver/fs/vfs/procfs.cpp` 210-340、`kernel/memory/{buddy,lazyalloc,page}.cpp` 相关段 | 关键段 | — |

> ⚠ **数行数的坑**：PowerShell 的 `Get-Content | Measure-Object -Line` 对 `pcb.cpp` 会数出 2460 行
> （少 273 行）。**只有 `read` 工具/wc 的数才对**（2733）。§2.1 的 2733 是对的。

---

## 8. 全量扫描：文件、结构、状态机

### 8.1 文件与入口符号（实测行号）

| 文件 | 行数 | 入口符号 → 行号 |
|---|---|---|
| `kernel/task/pcb.cpp` | 2733 | `alloc_pid:43`、`alloc_tid:48`、`found_pcb:187`、`kill_proc:244`、`kill_proc_deferred:292`、`kill_proc0:359`、`kill_thread:447`、`process_exit:494`、`switch_task_to_user_mode:725`、`jump_to_message:829`、`thread_clone:1138`、`process_execve:1296`、`free_fork_task:1845`、`process_fork:1906`、`create_kernel_thread:2177`、`create_user_thread:2265`、`create_message_thread:2404`、`create_process_group:2559`、`process_setup:2705` |
| `kernel/task/scheduler.cpp` | 594 | `get_current_task:36`、`change_proccess:95`、`is_task_schedulable:173`、`select_next_task_safe:316`、`timer_handle:381`、`scheduler_yield:460`、`scheduler_sleep_ns:468`、`scheduler_wake_task:495`、`add_task:526`、`remove_task:562` |
| `kernel/task/mutex.cpp` | 189 | `mutex_create:10`、`mutex_lock:23`、`mutex_trylock:67`、`mutex_unlock:107`、`mutex_destroy:142`、`mutex_is_locked:166`、`mutex_get_owner:179` |
| `kernel/task/reaper.cpp` | 66 | `process_is_current_on_any_cpu:15`、`find_reapable_child:29`、`reaper_thread:48`、`init_reaper:63` |
| `kernel/task/ipc.cpp` | 51 | `ipc_send:5`、`ipc_free_type:12`、`ipc_recv:24`、`ipc_recv_wait:34`、`ipc_recv_wait2:43` |
| `kernel/task/poll.cpp` | 52 | **纯工具函数，无 task 代码**（见 §9.4）|
| `kernel/lock_queue.cpp` | 387 | `queue_append_node:4`、`queue_remove_locked:26`、`queue_enqueue_ref:165`、`queue_remove_node:280`、`queue_dequeue:290` |

### 8.2 TCB 字段与**ABI 契约**（这是本次新发现的硬事实）

`struct thread_control_block` 的布局**被汇编引用**，不是纯 C 结构：

- `kernel/task/pcb.cpp:84-87` 两条 `static_assert`：`syscall_stack == 0xb48`、`syscall_user_rsp == 0xb50`；
- `kernel/intr/handler.S:32-33` 定义同名常量 `THREAD_SYSCALL_STACK = 0xb48` / `THREAD_SYSCALL_USER_RSP = 0xb50`；
  `:313-315` 系统调用入口 `movq %rsp, THREAD_SYSCALL_USER_RSP(%rax); movq THREAD_SYSCALL_STACK(%rax), %rsp`
  （即：**进入内核时切到 `task->syscall_stack`**），`:343`/`:350` 退出时再读回。
- ⇒ 推论：**内核线程没有 `syscall_stack`**（`create_kernel_thread` 不分配，见 §8.4），
  若它进系统调用，`%rsp` 会被载入 0 —— 源 OS 靠"内核线程从不 syscall"回避这一点。
- 按字段推算 `sizeof(struct thread_control_block) = 0xC20 = 3104 B`（锚点就是上面两条断言；
  `fpu_context_t` = 512 B，对齐 16，见 `include/cpu/fpu.h:5-8`）。

`struct process_control_block`（`include/task/pcb.h:85-141`）里与生命周期相关的是：
`status:95`、`thread_queue:93`、`child_pcb:106`、`parent_task:92`、`exit_code:107`、`task_level:113`、`vfork:104`。

### 8.3 `TaskStatus` 状态机：**谁能写 → 谁读 → 有没有唤醒路径**

| 状态 | 写入点（全树） | 读取/依赖点 | 唤醒路径 |
|---|---|---|---|
| `CREATE`(0) | `create_kernel_thread:2225`、`create_user_thread:2373`、`create_message_thread:2520` | `is_task_schedulable:177` 允许；`timer_handle:439` → `RUNNING`；`add_task:533-535` 改成 `START` | —（入队即转 START）|
| `RUNNING`(1) | `main.cpp:528`、`smp.cpp:160`、`process_setup:2713`（kernel_group）、`timer_handle:440`（换入）、`:450`（回滚）、`scheduler_sleep_ns:491`、`pipefs.cpp:65` | 计费与时间片 `:396/:402` | — |
| `WAIT`(2) | `scheduler_sleep_ns:486`、`pipefs.cpp:59` | `wake_sleeping_task:223-227`（**在扫描里顺手唤醒**）、`scheduler_wake_task:497` | ✅ 有（时间到 / `scheduler_wake_task`）|
| `DEATH`(3) | `kill_thread:456`、`kill_proc:287`、`kill_proc_deferred:310`、`kill_proc0:366`（整队置死） | `sys.cpp:109`、`signal.cpp:161`、`pcb.cpp:215`、`reaper.cpp:38`、`timer_handle:426`（寻父） | ❌ **永不回到 RUNNING**（没有清 DEATH 的代码）|
| `START`(4) | `add_task:534`、`wake_sleeping_task:225`、`scheduler_wake_task:506`、`timer_handle:437`（换出时 RUNNING→START）、`thread_clone:1180`、`process_fork:2056`、`create_process_group:2573` | 可调度集合 | — |
| `FUTEX`(5) | ★ **只有 `signal.cpp:198` 一处**（`SIG_DFL` 且内部决策是 STOP）★ | `is_task_schedulable:180/192` 读的是**进程**状态；线程状态只在可调度集合里被排除 | ❌ **全树没有把线程从 FUTEX 改回任何状态的地方** ⇒ 一旦置上就是永久不可调度 |
| `OUT`(6) | ★ **只有 `kill_thread0:464` 一处** ★ | `pcb.cpp:215`、`sys.cpp:109`、`signal.cpp:161` | ❌ 已是"已释放/待释放"|
| `ZOMBIE`(7) | `kill_proc:258`（is_zombie 分支）、`sys.cpp:1397`（wait4 收到 IPC 后补写） | `wait4_find_zombie_child:588`、`procfs.cpp:282`、`found_process_by_exe_path:215` | 由 `wait4` → `kill_proc(...,false)` 走 DEATH → 回收 |

> ★ **FUTEX 状态是坑**：`sys_(futex)`（`sys.cpp:3106-3131`）是**桩**——
> `FUTEX_WAIT` 只做"比对 val → `scheduler_yield()` → 直接返回 `ETIMEDOUT`"，`FUTEX_WAKE` 直接 `return 0`；
> **没有等待队列、没有 `tcb_t` 参与**。所以源 OS 里唯一把线程置 `FUTEX` 的地方是 SIGSTOP 语义，
> 而它**没有对应的 SIGCONT 唤醒实现**。⇒ 我们**不要**把 `FUTEX` 当成"可睡眠互斥的基础"照抄。

### 8.4 线程/进程的**五条创建路径**对照表

| | `create_kernel_thread:2177` | `create_user_thread:2265` | `create_message_thread:2404` | `thread_clone:1138`（CLONE_THREAD）| `process_fork:1906` |
|---|---|---|---|---|---|
| 内核栈 | ✅ 1 块 `:2207-2219` | ✅ 1 块 `:2298-2316` | ✅ 1 块 `:2439-2456` | ✅ 1 块 `:1184-1202` | ✅ 1 块 `:2057-2076` |
| 系统调用栈 | ❌ **不分配** | ✅ `:2318-2329` | ✅ `:2457-2467` | ✅ `:1192-1203` | ✅ `:2066-2077` |
| `task_level` | `TASK_KERNEL_LEVEL`（0）`:2202` | `APPLICATION`（2）`:2293` | `APPLICATION` `:2432` | `APPLICATION` `:1178` | `APPLICATION`（**给新 pcb** `:1955`，TCB `:2053`）|
| `context0.rip` | `_start` `:2217` | `switch_to_user_mode` `:2314` | `jump_to_message` `:2454` | `reg->rcx`（返回用户态的系统调用下一条）`:1208` | `init_fork_child_context:1857-1881` |
| 栈上压的返回地址 | ★ `process_exit` `:2237`（`PUSH_STACK` ×2 + 入口）★ | `switch_to_user_mode` `:2310-2312` | `jump_to_message` `:2450-2452` | — | — |
| `parent_group` | `pcb == NULL ? kernel_group : pcb` `:2186`/`:2205` | `pcb` `:2296` | `pcb` `:2435` | 复用父的 `process` `:1181` | 新建 `new_pcb` `:2097` |
| 入线程名册 | ✅ `target_group->thread_queue` `:2247` | ✅ `pcb->thread_queue` `:2381` | ✅ `:2538` | ✅ `:1275` | ✅ `:2124` |
| `tid` | `alloc_tid()` `:2256` | `alloc_tid()` `:2395` | `alloc_tid()` `:2551` | `alloc_tid()` `:1245` | ★ `tid = new_pcb->pid` `:2098`（**线程 ID 借进程 ID**，再 `reserve_tid_value`）|
| 初态 | `CREATE` `:2225` | `CREATE` `:2373` | `CREATE` `:2520` | `START` `:1180` | `START` `:2055` |
| `fs_base` | ★ `(uint64_t)new_task`（**TCB 自指针**）`:2241` ★ | 0 `:2377` | 继承创建者或自指针 `:2526-2534` | 继承/TLS `:1236` | 继承 `:2095` |
| 失败回滚 | 逐个 `free(new_task)` + `free_frames` | 同 | 同 | 完整（含 `vfs_close`/`free_frames`/`free`）| `free_partial_process` + `free_fork_task:1845` |
| 全树调用点 | `reaper.cpp:65`、`kmod/xhci/xhci.cpp:3107`、`kmod/xhci/usb_core.cpp:204`、`kmod/netserver/netserver.cpp:614`、`kmod/netserver/arch/sys_arch.cpp:256`（**全树共 5 处，全部传 `NULL` ⇒ 全挂 `kernel_group`**）| `user.cpp:1013`、`user.cpp:1043` | `kernel/syscall/message.cpp:94` | `sys_(clone):3061-3063`、`sys_(clone3):3066-3088` | `sys_(fork):3058`、`sys_(vfork):3135`、`syscall.cpp:409/410/644`、`thread_clone:1144`（无 CLONE_THREAD 时降级为 fork）|

> ⚠ **§2.5 的一处不精确**：§2 写"内核自带的线程全部传 `NULL`：`main.cpp` 的 idle、…"——
> **两个 idle 根本不是 `create_kernel_thread` 造的**，而是手写构造（`main.cpp:521-541`、`smp.cpp:152-172`）；
> 真正传 `NULL` 的是 §8.4 表里那 5 个调用点（§2 漏了 xhci 的两处）。

### 8.5 两个 idle 的**字段级**对照（Q0 的完整证据）

| 字段 | BSP idle `main.cpp:521-541` | AP idle `smp.cpp:152-172` |
|---|---|---|
| 分配 | `aligned_alloc(16, …)` + `memset(0)` `:522-523` | 自己那份 `alloc_zeroed_tcb:28-34` `:152` |
| `task_level` | ★ **从不赋值 ⇒ 0 = `TASK_KERNEL_LEVEL`** ★ | `TASK_IDLE_LEVEL`（1）`:154` |
| `tid` | `++now_tid`（首值 **0**）`:525` | `++now_tid`（1,2,…）`:155` |
| `parent_group` | `kernel_group` `:526` | `kernel_group` `:162` |
| `kernel_stack` | `get_rsp()`（**引导栈本体**）`:527` | `get_rsp()`（AP 的临时栈）`:156` |
| `status` | `RUNNING` `:528` | `RUNNING` `:160` |
| `context0.rsp` | `get_rsp()` `:533` | `get_rsp()` `:156` |
| `context0.rip` | ★ 从不赋值 ⇒ 0 ★ | ★ 从不赋值 ⇒ 0 ★ |
| `context0.rflags` | ★ 从不赋值 ⇒ 0 ★ | `get_rflags() \| 0x200` `:158` |
| `cpu_id` | ★ 从不赋值 ⇒ 0 ★ | `lapic_id()` `:159` |
| EEVDF 实体 | ★ 从不调用 `scheduler_init_task` ⇒ vruntime/deadline/slice 全 0 ★ | `scheduler_init_task(apu_idle)` `:167` |
| 进 `kernel_group->thread_queue` | ✅ `:532` | ★ **没有这一步** ★ |
| 进本核调度队列 | `queue_enqueue_ref(get_current_cpu()->…)` `:535` | `queue_enqueue_ref(info->…)` `:170` |
| FPU 现场 | `save_fpu_context:539` | ★ 没有 ★ |
| 循环体 | **没有循环**——它就是把 `KernelMain` 当时的现场存下来；`KernelMain` 尾部 `622-631` 是 `while(true){ if(!no_interrupt){enable_intr; enable_scheduler;} pause; }` | `while(true) pause` `:178-181` |

**由此得到三条比 §2.4 更硬的结论**：

1. BSP idle 缺的不止 `task_level` 一行：`task_level`/`cpu_id`/`context0.rflags`/`scheduler_init_task` **四处都没做**；
2. ★ **AP idle 没进 `kernel_group->thread_queue`** ★ —— 所以"内核线程名册"里**只有 BSP idle**，
   AP idle 只存在于调度队列与 `cpu->current_task` 里（`found_thread(kernel_group, tid)` 找不到它）；
3. `context0.rip == 0` 只维持到**它第一次被换出**（`change_proccess:140` 会把当时 rip 写回），
   之后 BSP idle 就是一个 `rip` 合法的**普通内核线程**，能被正常选回来跑 `main.cpp:622-631` 那个循环。
   ⇒ §2.4 里"被选中时白做一次派发"**只对第一次换出之前成立**，之后它是真能跑的。

### 8.6 一次 tick 的完整路径（`timer_handle` 逐步）

```
时钟中断 → save_registers (scheduler.cpp:42-93, cli + 保存 15 个寄存器 + ds/es)
  → timer_handle(reg)                        :381
      !is_scheduler → send_eoi; return       :384-387
      current == NULL → send_eoi; return     :389-393
      统计：system_cpu_total_ticks/busy_ticks、current->runtime_ticks   :395-399
      仅当 status==RUNNING && level!=IDLE：
          cpu->scheduler_ticks++；charge_current_eevdf_runtime(EV_TICK_NS)  :402-404
          scheduler_ticks < TIME_SLICE(=4) → 直接回去                        :405-408
      select_next_task → select_next_task_safe :413 / :316
          取本核队列；queue->lock；唤醒 current 自己 :329
          queue_average_vruntime(queue, current, now, &fallback, &idle)      :337/:256
               ★ 在扫描里顺手 wake_sleeping_task ★                            :267
               ★ idle 候选从"整表里 level==IDLE 的人"里取 ★                    :268-270
          best = fallback（若 vruntime <= 平均）                             :340-342
          否则二遍扫描挑 vruntime <= avg 且 deadline 最早的人                 :344-354
          best 为空 → fallback                                              :356
          兜底链 result = best ?: (current 可跑 ? current : idle)            :358
          记 eevdf_last_start / 补 slice/deadline                            :359/:304
      best == NULL || best == current → 回                                        :414-418
      记录 current->fs_base                                                      :421
      ★ 任务寻父：best 的组若"无父/父已 DEATH/OUT" ⇒ `parent_task = kernel_group` ★ :424-429
      disable_scheduler; current RUNNING→START; best START/CREATE→RUNNING        :433-441
      ★ best->context0.rip != 0 才 change_proccess；否则**撤销状态、不切换** ★    :444-452
      enable_scheduler; send_eoi                                                :454-457
```

关键点（M4-11 会踩到的）：
- **换出时把 `RUNNING` 改成 `START`**（`:437`）⇒ 任何"靠 status 判活"的外部观察者会看到 `START`；
- **`remove_task` 不清 `cpu->current_task`**（见 `:562-594`，只摘 `sched_node`）⇒ 释放 TCB 前必须自己确认"它不是任何核的 current"；
- `add_task:552` 调 `queue_average_vruntime` 时**只持全局 `scheduler_lock`，没有持目标队列的 `queue->lock`** ⇒ 与 `select_next_task_safe`（持队列锁）之间存在一个既有的良性竞态（照抄时要知道）。

### 8.7 锁清单与嵌套关系（做 §4.3 的"补 `scheduler_lock`"要看这个）

| 锁 | 定义 | 谁取 | 覆盖 |
|---|---|---|---|
| `scheduler_lock` | `scheduler.cpp:171` | 只有 `add_task:530`、`remove_task:564` | 全局，罩"挑核 + 入队/摘队" |
| 每核 `scheduler_queue->lock` | `queue_init` | `select_next_task_safe:325`、以及所有 `queue_*` 内部 | 每核调度队列 |
| `thread_queue->lock` | `queue_init` | `found_thread:230`、`invalidate_process_message_pipes:176`、`kill_proc:261`、`kill_proc_deferred:314`、`kill_proc0:362`、`signal_send_process:157`、`sys.cpp:104`、`sys.cpp` 多处 | 进程线程名册 |
| `create_thread_lock` | `pcb.cpp:68` | **四个创建者全取**：`:2183`/`:2271`/`:2412`/`:1168`/`:1940` | 串行化创建 |
| `execve_image_lock` / `user_stack_build_lock` | `pcb.cpp:69-70` | `process_execve:1386` / `switch_task_to_user_mode:745` | 影像替换 / 用户栈构建 |
| `zone->allocator.lock` | `buddy.cpp:373` | `free_frames:373` / `alloc_frames` | 页帧伙伴系统 |
| `pcb_group_queue->lock`、`file_open->lock`、`ipc_queue->lock`、`pipe->lock` | `queue_init` / `pipe.h:19` | 各处 | — |

**观察到的嵌套方向**：`create_thread_lock → scheduler_lock → scheduler_queue->lock`；
`thread_queue->lock → (若 kill_thread0 被启用) zone->allocator.lock + scheduler_lock + scheduler_queue->lock`；
**反向（`scheduler_lock → thread_queue->lock`）在全树不存在** —— 这解释了 §9.2 里为什么"解耦"要先动锁。

### 8.8 `tcb_t` 长期持有者**全量**审计（在 §2.7 的五类之外新增三类）

| # | 持有者（结构+字段）| 定义 | 写入 | 注销 | 失效后果 |
|---|---|---|---|---|---|
| 1 | 每核 `current_task` | `include/smp/smp.h:43` | `main.cpp:541`、`smp.cpp:172`、`timer_handle:447` | ★ **从不注销**（`remove_task` 不碰它）★ | 释放 TCB ⇒ 下一次 tick 读已释放内存 |
| 2 | 调度队列节点 `node->data` | `lock_queue.h` | `add_task:554`（`queue_enqueue_ref`）| ✅ `remove_task:572/588` | — |
| 3 | 进程线程名册 `pcb->thread_queue` | `pcb.h:93` | `:1275/:2124/:2247/:2381/:2538` | ✅ 进程拆卸 `kill_proc0:374-380` | — |
| 4 | `mutex_t.owner` | `include/mutex.h:17` | `mutex_lock:54`、`mutex_trylock:96` | ★ 只在 unlock 时清（`:130`）；**线程退出时不注销** ★ | 持锁线程退出 ⇒ 锁永久占用 + 悬空 owner |
| 5 | 管道等待名单 `blocking_read/blocking_write` | `include/pipe.h:21-22` | `pipefs.cpp:52`（`pipe_add_waiter`）| ✅ `pipe_wake_waiters:33-42`（唤醒时、以及最后一端 close 时 `:181/:189`）| 若线程在名单里被释放 ⇒ `scheduler_wake_task(node->thread)` 读已释放内存 |
| 6 | ★ **`fs_base` 自指针**（内核线程）★ | `pcb.cpp:2241` `new_task->fs_base = (uint64_t)new_task` | 同上 | 随线程；`change_proccess:97/119` 每次切换写回 MSR | 线程被释放而 MSR 仍指着它 ⇒ 下一次 `read_fsbase/write_fsbase` 用悬空值 |
| 7 | ★ **procfs 的多个渲染函数**（只读、**不持 `thread_queue->lock`**）★ | `procfs.cpp:220-241`、`261-270`、`300-322` | — | — | 与并发入队/退队竞态（本次新发现，属只读观察面）|
| 8 | `tcb->sched_node` 反向指针 | `pcb.h:176` | `add_task:554` | `remove_task:573/589` 置 NULL | — |

**结论**：真正"从不注销"的是 **①`current_task` 和 ④`mutex->owner`**；
⑤管道靠"先关 fd 再释放"的顺序兜住（`kill_proc0:370` 的 `close_process_file_table` 在 `:372-378` 的释放之前，
与 §2.7 的结论一致，本次逐行复核 ✅）；⑥是我们**新引入"线程会退出"之后才会变成问题**的第六类。

### 8.9 进程级生命周期（顺带扫清，M4-11 的 reaper 要对着它设计）

```
sys_exit:657-682        有兄弟线程 → kill_thread(自己) → open_interrupt; scheduler_yield; hlt
                        （★ 自己的 2 MiB 栈 + TCB 要等**整个进程**被拆时才还 —— 这就是 §2.5 的一般形式）
sys_exit_group:2532     直接 kill_proc(组, code, is_zombie=true)
kill_proc:244-290       kernel_group ⇒ 打印 "Cannot kill System process." 直接 return :247-251
   is_zombie=true  → close fd 表 :257 → status=ZOMBIE :258 → 逐个 kill_thread :265 → IPC_MSG_TYPE_EPID 通知父 :269-281
   is_zombie=false → 从父的 child_pcb 摘掉 :285-286 → status=DEATH :287 → kill_proc0 :288
kill_proc0:359-423      procfs_on_exit_task :361 → 全队置 DEATH :362-368 → close fd 表 :370
                        → do{ queue_dequeue; kill_thread0(t); free(t); } :372-378   ★ TCB 由这里 free ★
                        → queue_destroy(thread_queue) :380 → 从 pcb_group_queue 摘掉 :381
                        → lazy_free(pcb) :402（★ 名字骗人：它只清 `pcb->virt_queue` 里的惰性页记录，
                          `kernel/memory/lazyalloc.cpp:244-261`，**不释放 pcb**）→ 逐个 free 字段 → free(pcb) :420
wait4:1320 / wait4_reap_child:628  → kill_proc(child, exit_code, false) :653
reaper_thread:48-61     每轮 find_reapable_child(kernel_group) :57
                        （`status == DEATH` 且 `!process_is_current_on_any_cpu`，:38）→ kill_proc(target,0,false) :58
kill_proc_deferred:292-324  ★ **全树无调用者 = 死代码** ★（它才会把进程挂到 kernel_group->child_pcb 交给 reaper）
timer_handle:424-429     "任务寻父"：组无父/父已 DEATH|OUT ⇒ parent_task = kernel_group（孤儿的来源）
```

⇒ **reaper 的覆盖缺口（本次新发现）**：它只找 `status == DEATH`，而 `wait4` 之前的孩子是 `ZOMBIE`
（`:258`/`sys.cpp:1397`）⇒ **僵尸不在 reaper 的扫描条件里**；真正能让 reaper 干活的
`kill_proc_deferred` 又没人调用。所以"reaper 兜底回收进程"这条链在源 OS 里**是断的**，
这与 §2.5"内核线程永不回收"是同一类现象：**机制写好了，最后一根线没接**。

### 8.10 与 §2 的差异汇总（本次扫描更正/补充，按重要性排序）

| # | §2 原话 | 本次核对结果 | 影响 |
|---|---|---|---|
| 1 | `poll.cpp` "未读；注意它会持 `tcb_t`" | ★ **错**：它是纯工具（52 行，掩码转换 + select 位图），**不含任何 `tcb_t`**；真正的等待是 `sys.cpp` 的忙等循环 | 见 §9.4，M7 的移植预期要改 |
| 2 | "内核自带的线程全部传 NULL：main.cpp 的 idle…" | 两个 idle 不是 `create_kernel_thread` 造的；传 NULL 的是 5 个调用点（漏了 `kmod/xhci/xhci.cpp:3107`、`kmod/xhci/usb_core.cpp:204`）| 只是清单精度 |
| 3 | BSP idle"漏了一行赋值" | 实际漏了**四处**：`task_level` / `cpu_id` / `context0.rflags` / `scheduler_init_task` | §9.1 的 Q0 答复 |
| 4 | — | ★ **AP idle 没进 `kernel_group->thread_queue`** ★ | 新事实 |
| 5 | "两个 idle 的 rip 都保持 0 ⇒ 白做一次派发" | 只在**第一次换出之前**成立；之后 BSP idle 是合法候选、能跑回来 | 新事实 |
| 6 | `kill_thread0` = "488/490/491 三句" | 完整体是 `:462-491`：还置 `OUT`、清 child_tid、**释放 argv/cwd/用户栈映射**；TCB 是**调用者** `:377` 释放的 | 我们照抄时要照全 |
| 7 | — | ★ `kill_proc_deferred`（`:292-324`）是死代码 ★ | Q3/回收设计的重要旁证 |
| 8 | — | reaper 只认 `DEATH`、不认 `ZOMBIE` ⇒ 进程兜底回收链是断的 | 新事实 |
| 9 | — | `futex` 是桩；`FUTEX` 状态**无唤醒路径** | 别照抄 |
| 10 | — | TCB 布局是**汇编级 ABI**（`0xb48/0xb50`）| 我们 ARM 侧同理要锚住 |
| 11 | — | `kill_thread` 还有第 5 个调用点 `page.cpp:465`，但**实际不可达**（前面的分支 `:455-462` 必然终止） | 精度 |
| 12 | — | `lazy_free(pcb)` 名字骗人：只清 `virt_queue` | 精度 |
| 13 | §2.7 五类持有者 | 补三类：`fs_base` 自指针、procfs 无锁读、`sched_node` 反向指针 | §8.8 |
| 14 | — | 上游有些注释是**双重编码乱码**（`pcb.cpp:457`、`:1978`、`:2218-2219`、`reaper.cpp:13`）| 引用前先解码，见 §9.5 |
| 15 | — | ★ `sys_setpgid` 里 `process->pid = pgid;`（`sys.cpp:4208`）**直接覆盖 pid** ★ | 与 task 无关但同属"状态机被直接改写"的味道，记一笔 |
| 16 | — | `sys_wait4` 的回收路径（`sys.cpp:653`）**没有** reaper 那样的"不在任何核 current 上"检查（对比 `reaper.cpp:38`）| SMP 下的既有风险，M4-11 的 reaper 必须带这个条件 |

---

## 9. PTASK 里的问题：逐条作答（基于源码，不猜作者意图）

> 先把问题分成两类：
> - **源码可定论**：Q0、Q0b、Q1、Q3、§7 的三条未核实项；
> - **只能由作者回答**：Q2 的"当时卡在哪把锁"（但可以给出**三条代码级理由 + 一条 git 历史证据**，把范围锁死）。

### 9.1 Q0：BSP idle 的 `task_level` —— **是漏赋值，而且漏的不止一行**

**结论**：`kernel/main.cpp:521-541` 从头到尾**没有**给 `task_level` 赋值；TCB 来自 `aligned_alloc` + `memset(0)`（`:522-523`），
而 `TASK_KERNEL_LEVEL == 0`（`include/task/pcb.h:3`）⇒ 它的 `task_level` **就是 0**。
把 §2.4 的三条后果逐行复核后，**全部成立**，并补四条：

| # | 后果 | 证据 | 复核结果 |
|---|---|---|---|
| ① | `kill_thread` 不拒绝它 | `pcb.cpp:450` 只比 `TASK_IDLE_LEVEL` | ✅ |
| ② | `is_task_schedulable` 不排除它 ⇒ 是正常候选 | `scheduler.cpp:178` | ✅ **但有时限**：`change_proccess:140` 会在它**第一次被换出**时把当时的 rip 写回 TCB，此后它就是合法候选、**能被换回来真跑**（跑 `main.cpp:622-631`）。⇒ "白做一次派发"只对第一次换出之前成立 |
| ③ | 被 EEVDF 计费 | `:396`/`:402`/`charge_current_eevdf_runtime:245-254` 都只看 `task_level` | ✅ |
| ④ | ★ 新：**它的 EEVDF 实体从未初始化** ★ | AP 侧有 `scheduler_init_task`（`smp.cpp:167`），BSP 侧没有 | `eevdf_deadline == 0` ⇒ `deadline_before:237-243` 在第一轮比较里必然判它"最早"，**极容易被选成 fallback/best** |
| ⑤ | ★ 新：`cpu_id` 也没赋值（=0）★ | `smp.cpp:159` 有，`main.cpp` 没有 | `remove_task:566` 的快速路径按 `task->cpu_id` 找队列 |
| ⑥ | ★ 新：`context0.rflags` 也没赋值（=0）★ | `smp.cpp:158` 有 | 若它真被换回来，恢复的是 IF=0 的 rflags |
| ⑦ | ★ 新：它**在** `kernel_group->thread_queue` 里，而 AP idle **不在** ★ | `main.cpp:532` 有 `queue_enqueue`；`smp.cpp` 全文没有 | 名册里"内核线程"只有 BSP idle 一个 |

**判定**：代码形状上无法解释为"有意"——同一个不变量在 AP 侧显式写了（`smp.cpp:154`），BSP 侧却是
"memset 之后逐字段赋值、恰好漏掉 `task_level`"。⇒ **是漏赋值**。

### 9.2 Q0b：是不是"只有 idle 不可停、普通内核线程可以停" —— **是；但"可停"≠"会回收"**

- 判据确实只有一句：`pcb.cpp:450 if (task->task_level == TASK_IDLE_LEVEL)` ⇒ **只有 idle 被拒**；
  普通内核线程（level 0）可以被 `kill_thread` 置 `DEATH`（`:456`）——这与 `process_exit` 打印
  `"Kernel thread exit, Code: "`（`:498`）在语义上是自洽的。
- ⚠ 但必须补一句：**置 DEATH 之后没有任何释放路径**（见 §9.3）⇒ 源 OS 对内核线程的真实语义是
  "**可以标记为死，但永远不释放**"，而不是"可以结束"。
- 附带两个精度问题：那句 `write_serial_fmt("Cannot stop kernel thread.")`（`:452`）**没有换行符**；
  `kill_thread` 返回 `void`（`pcb.h:247`）⇒ **调用者无法得知自己被拒**。
  （`sys_exit:670-673` 正是靠"有兄弟线程就 kill_thread(自己)"来决定去向的，它拿不到反馈。）

### 9.3 Q1：挂在 `kernel_group` 上的内核线程，栈与 TCB 由谁释放 —— **没有谁（可达性证明）**

**三条边，每条都只有一个出口**：

```
kill_thread0(唯一调用者)  ⟵ pcb.cpp:376   （在 kill_proc0 的排空循环里）
kill_proc0 (唯一调用者)   ⟵ pcb.cpp:288   （在 kill_proc 的 is_zombie==false 分支里）
kill_proc  ∧  kernel_group ⟹ pcb.cpp:247-251 "Cannot kill System process." 直接 return
```

全树另外两处会释放 TCB/内核栈的代码，都够不着内核线程：

| 位置 | 作用域 | 为什么够不着 |
|---|---|---|
| `free_fork_task:1845-1855`（`free(task)` `:1854`）| **只用于 `process_fork` 的失败回滚**（调用点 `:2061/2071/2105/2114/2128/2141`）| 只在 fork 出错时跑 |
| `kill_proc0:372-378` 的 `free(thread)` `:377` | 进程拆卸 | 只对"能被 kill_proc 杀的进程"生效 |

**旁证（本次补全到 5 条）**：

1. `reaper_thread` 扫的是 `kernel_group->child_pcb`（**进程**）且只认 `status == DEATH`（`reaper.cpp:29-46`）；
2. ★ `kill_proc_deferred`（`pcb.cpp:292-324`）是**唯一**"把死进程挂到 `kernel_group->child_pcb` 交给 reaper"的机制，
   而它**全树无调用者 = 死代码** ★（`grep` 确认：只有 `pcb.h:246` 声明 + `pcb.cpp:292` 定义）；
3. `wait4` 之前的孩子是 `ZOMBIE`（`:258` / `sys.cpp:1397`）⇒ **僵尸不满足 reaper 的 `status == DEATH` 条件**；
4. 两个 idle 的 TCB 也**没有任何 free 路径**（`main.cpp:522`、`smp.cpp:152`）；
5. OOM/缺页那条路（`page.cpp:457`）对内核线程同样撞在 `:247-251` 上。

⇒ **措辞（与 §2.5 一致，补一句）**：
> 对挂在 `kernel_group` 上的线程，`kill_thread0()` 的那几行**没有任何可达路径**；
> 而且**不是"忘了写"，是"最后一根线没接"**——接线的零件（`kill_proc_deferred` + reaper）都在树里，只是没人调用。

**例外（照 §2.5，本次明确"TCB 由谁 free"）**：`create_kernel_thread(…, pcb != NULL)` 造的线程，
在那个**进程**被拆时由 `kill_proc0:372-378` 处理：`kill_thread0` 还栈（`:488-490`）+ 摘队（`:491`），
**再由调用者 `free(thread)`（`:377`）还 TCB**。
⚠ 全树 5 个 `create_kernel_thread` 调用点**全部传 `NULL`** ⇒ 这个例外在源 OS 里**从未被用到**。

### 9.4 Q3：反复 `create_kernel_thread` 会不会耗尽内核栈内存 —— **会，但源 OS 自己看不出来**

**每次调用的成本**（都是永久的）：

| 项 | 大小 | 出处 |
|---|---|---|
| 内核栈 | **1 MiB**（`CONFIG_KERNEL_TASK_STACK_SIZE = 1048576`）| `kernel/build_settings.h:64-65`、`include/proto.hpp:5-6,15`；`alloc_frames(KERNEL_STACK_SIZE/PAGE_SIZE)` `pcb.cpp:2207` |
| 系统调用栈 | **0**（内核线程不分配）| `create_kernel_thread` 全文没有第二次 `alloc_frames` |
| TCB | **3104 B**（`aligned_alloc(16,…)`）| `pcb.cpp:82-98`；`sizeof` 推算见 §8.2 |
| 合计 | **≈ 1.05 MiB / 个** | — |

**源 OS 实际用量**：5 个内核线程（reaper + netserver + lwip tcpip + xhci-svc + usb-hotplug）
+ 2 个 idle（不分配栈，用现成栈）⇒ **≈ 5 MiB**，在一台有 GB 级内存的机器上**无感**。

**有没有上限/计数**：没有。全树没有"线程数上限"或统计计数器；唯一的失败面是
`alloc_frames` 拿不到帧时打印 `"Failed to allocate memory!!!"`（`buddy.cpp:337`）并返回 0
⇒ `create_*` 返回 `-ENOMEM`/`NULL`（`pcb.cpp:2210-2213` 等）。

**reaper 会不会帮忙**：不会（只扫进程、只认 DEATH，见 §9.3）。

⇒ **回答**：会耗尽，但要在"模块反复加载/卸载"或"每次请求起一个 worker"这类用法下才会；
单次启动的那几个 worker 是纯泄漏但量级可忽略。
**要接的话**最省事的两条路（源树里都已有零件）：① 把 `kill_proc_deferred` 那套接上；
② 把 `find_reapable_child` 的覆盖面从 `child_pcb` 扩到 `thread_queue`（并加上"不在任何核 current 上"的条件）。

**对我们的意义（不变）**：我们是 32 固定槽 + 每轮自检约 30 个探针 ⇒
*"线程退出 + 回收"不是可选优化*（§4.1 的结论**不需要修改**，反而被这次扫描加强）。

### 9.5 Q2：那句 `// kill_thread0(task);` 为什么不能直接取消注释（**三条独立理由**）

> 先给历史证据：`git log -S "kill_thread0(task);" -- kernel/task/pcb.cpp` **只命中一条** `4224bec first commit`（2026-08-01，
> 仓库初始导入）。⇒ **这一行从进入仓库的第一天就是注释**，没有"曾经启用、后来关掉"的历史，
> 所以"当时卡在哪把锁"只能由作者回忆；但**卡住的原因**可以从代码里指出来。

**(a) 它会释放"自己正在用的那根栈"** —— `kill_thread` 的 5 个调用点里，**3 个是线程杀自己**：

| 调用点 | 谁 | 之后还继续跑吗 |
|---|---|---|
| `pcb.cpp:501`（`process_exit`）| 内核线程自己 | ✅ `:502-504` `open_interrupt; while(true) hlt;` —— **在已被 `free_frames` 的栈上** |
| `sys.cpp:672`（`sys_exit` 多线程分支）| 用户线程自己 | ✅ `:679-680` `scheduler_yield(); cpu_hlt;` —— **在已被 free 的 TCB 上**（`timer_handle` 会读 `current->status`）|
| `page.cpp:465` | 用户线程自己 | 实际**不可达**（前面的 `:455-462` 必然终止，见 §8.10 #11）|
| `pcb.cpp:265`（`kill_proc` 僵尸分支）| **别人**，但遍历的队列**包含当前线程自己**（`exit_group` 就走这条）| ✅ 同上 |
| `pcb.cpp:318`（`kill_proc_deferred`）| 同上（死代码）| — |

**(b) 释放之后 TCB 仍被"每核指针"引用** —— `remove_task`（`scheduler.cpp:562-594`）**只摘 `sched_node`，
不清 `cpu->current_task`**（`include/smp/smp.h:43`）。而 TCB 是**调用者**在 `kill_thread0` 之后才 `free`（`pcb.cpp:377`）。
⇒ 之后任意一次时钟 tick 的 `timer_handle:389/396` 都会读已释放内存。
源 OS 自己的正确答案就写在 reaper 里：**必须确认"它不是任何核的 current"**（`reaper.cpp:15-27`、`:38`）——
线程级回收要把这个条件照抄过去。

**(c) 锁嵌套与"边遍历边释放"** —— 两个"别人来杀"的调用点**都已经持有 `thread_queue->lock` 并用 `queue_foreach` 遍历**
（`:261-267`、`:314-320`）；`kill_thread0` 会在这把锁**之内**再去取
`zone->allocator.lock`（`buddy.cpp:373`，经 `free_frames:488-490`）、`scheduler_lock`（`scheduler.cpp:564`）、
以及队列锁（`:572`/`lock_queue.cpp:284`）。
全树**没有任何地方**建立 `thread_queue->lock → scheduler_lock` 这个方向（§8.7）⇒
这就是注释里"**要用的解耦，但可能要加锁**"的具体所指：**要么把释放挪出遍历（两段式），要么重新定义锁序**。
⇒ 结论：**取消注释不是"补一行"，而是"改结构"**——这正是 §4.3 方案 A（垂死线程只置 DEATH；第三方回收）在做的事。

**原注释的字节级还原**（顺手做掉）：文件里那串是 **UTF-8 里存的双重编码乱码**。
按 `读作 UTF-8 → 再编码回 GBK → 再按 UTF-8 解码` 逆变换可得
「**要用的解耦,但可能要加锁**」；其中逗号在乱码过程中被吞成 `?`（不可复原）。
⇒ §2.5 的转写「要用的解耦，但可能要加锁」**是对的**（只差一个全角/半角逗号）。

### 9.6 §7 的三条未核实项：结案

**(1) `kernel/task/poll.cpp`：本条结案，且 §2.1 的警告是错的。**

- 文件 **52 行**，全部是**纯工具函数**：`epoll_to_poll_comp:4`、`poll_to_epoll_comp:16`、
  `select_add:28`、`select_bitmap:42`、`select_bitmap_set:48` ⇒ **不含任何 `tcb_t`、不睡眠、不列表**。
- 调用者只有 `sys.cpp`：`:2578`（`poll_kernel_fds` 里转事件掩码）、`:3364/3371/3378/3423/3428/3433`（`select`）。
- **真正的等待**是两处**忙等循环**：
  - `sys_(poll)` → `poll_kernel_fds`（`sys.cpp:2544-2599`）：`do { 逐个 fd 轮询； open_interrupt; scheduler_yield(); pause; } while (未超时)`；
  - `sys_(epoll_wait)`（`sys.cpp:2807-2853`）：同样的 `do{…}`，watch 名单是 `epoll_file_t.watches`（`include/task/poll.h:60-62`），
    元素 `epoll_watch_t`（`:55-58`）只有 `fd + event`。
- ⇒ **修正**：要移植 poll/select/epoll，风险不是"它会持 `tcb_t`"，
  而是"**它是忙等：开着中断反复让出，既没有等待队列也不挂起**"（对 SMP 负载/时间片的观感会有影响）。

**(2) `process_fork` / `execve` 对 TCB 的所有权 —— 本次扫清。**

| 路径 | 进程组 | TCB | 两套栈 | 旧资源 |
|---|---|---|---|---|
| `thread_clone`（CLONE_THREAD）`pcb.cpp:1138-1292` | ❌ 不建，复用 `parent_task->parent_group:1181` | 新建 `:1170` | 新建 `:1184/:1192` | — |
| `process_fork` `:1906-2175` | ✅ 新建 `new_pcb:1942-1944`，入 `pcb_group_queue:2027` 与父 `child_pcb:2036` | 新建 `:2045` | 新建 `:2057/:2066` | 子用**新的** pagedir（`clone_page_directory:1969`；vfork 则共用）|
| `process_execve` `:1296-1707` | 复用 | ★ **复用当前 TCB**（`:1688-1702` 改它的 argv/main/user_stack/fs）★ | 不新建；用户栈重新 `page_reserve_user_range:1609` | 旧 pagedir 在 `:1666` 释放；旧 virt_queue/ipc_queue 在 `:1672/:1681` 清 |

⇒ 结论与 §2.8 一致：**源 OS 里 TCB 的存活期 = 进程组的存活期**（fork 新建、execve 不换、
线程先退只置 DEATH 不回收），不存在"TCB 必须活到 join"的约束。M7 的 fork/exec 移植可以照此简化。

**(3) `main` 有没有前移 —— 没有。**

- `git ls-remote origin main` ⇒ `08e5c9c…`（**2026-09-13 实测**）；该提交日期为 **2026-09-04**。
- ⚠ GitHub API 在本机取不到（`api.github.com` 解析到非公网地址）⇒
  §7 最后两条（"31 条议题 + 50 个 PR 里没有讨论线程生命周期"）**仍然无法复核**，强度保持原样。

### 9.7 仍然只能由作者回答的（3 条，可直接发 issue）

1. `pcb.cpp:457` 那句 `kill_thread0(task)`：**注释里说的"锁"是指哪一把**——
   `scheduler_lock`（`scheduler.cpp:171`）？`thread_queue->lock`？还是 `create_thread_lock`？
   还是当时想到的是"**不能在自己的栈上释放自己**"这个更根本的问题？（我们从代码里能指出的只有 §9.5 的三条）
2. `kill_proc_deferred`（`pcb.cpp:292-324`）**是不是**"内核线程/进程回收"的未完成件？打算由谁调用？
   （它 + reaper 正好能补上 §9.3 那条断链，但 reaper 判据只认 `DEATH`、不认 `ZOMBIE`。）
3. 内核线程的定位到底是"永久基础设施"还是"可以起停的工作线程"？
   —— 这决定我们偏离清单的写法（§4.2 的两种处置），**但我们这边照做回收这件事不变**（§4.1）。

### 9.8 这次扫描对 M4-11 的影响（结论：**方案 A 不变，但要补三件事**）

| # | 新增要求 | 依据 |
|---|---|---|
| 1 | 回收者必须**排除"任何核的 current"**（不只是"不在队列里/不是 DEATH"）| `remove_task` 不清 `current_task`（`scheduler.cpp:562-594`）；源 OS 自己就是这么防的（`reaper.cpp:38`）|
| 2 | 回收者要**同时认 `DEATH` 和"已被置 OUT/僵尸"**——否则会重演源 OS "reaper 永远挑不到人"的断链 | `reaper.cpp:38` vs `pcb.cpp:258`/`sys.cpp:1397` |
| 3 | 垂死线程**不能自己释放栈**：`thread_finish()` 只置 DEATH + 让出 + `wfi`，释放一律交给回收者 | §9.5(a)，源 OS `pcb.cpp:457` 的注释正是这个坑 |
| 4 | `mutex->owner` 的注销/拒绝要写成**不变量**（线程会退出后第一次可达）| §8.8 #4；`mutex_unlock:120-124` 只在 unlock 时清 |
| 5 | **不要引 `FUTEX` 状态**：源 OS 里它是 SIGSTOP 用的死胡同（无唤醒路径）| §8.3、`signal.cpp:198` |
| 6 | 我们的 `tcb` 若也有"内核线程没有 syscall 栈"这种差异，要在 M4-11 的退化清单里写明（x86 的 `handler.S:313-315` 是 ABI 契约）| §8.2（⚠ 我们 ARM 侧的对应实现本次**未逐行核对**）|

---

## 10. 全量扫描·第二批：启动/切换/每核结构的补充核对

> 第二批是沿"启动 → idle → 每核结构 → 上下文切换"这条线补的，**与 §8 不冲突，只做增补与更正**。
> 为确证结构体偏移，第二批用 clang 写了一个**一次性布局探针**（写在系统临时目录、不在仓库内，跑完删除），
> 拿到的偏移与 `handler.S` 里的硬编码常量**互相印证**，见 §10.4。

### 10.1 启动时序（KernelMain → 第一个用户线程）

| # | 事件 | 出处 |
|---|---|---|
| 1 | `process_setup()` ⇒ 建 `kernel_group` | `main.cpp:512` → `pcb.cpp:2705-2726`（`kernel_group = kernel_pcb;` `:2726`）|
| 2 | ★ BSP 把 `smp_scheduler_lock` 放行、`scheduler_is_ready++` ★ | `pcb.cpp:2728` / `:2729` |
| 3 | `init_smp(BootConfig.MADT)`：BSP + AP 各自的 `pcr_inf` 条目与 `scheduler_queue = queue_init()` | `main.cpp:519` → `smp.cpp:387-390`（BSP）/ `:421-423`（AP）|
| 4 | AP 入口是 ★ **`apu_entry`** ★（**全树没有 `cpu1_main` 这个符号**）| `smp.cpp:73-176` |
| 5 | AP：GDT/TSS/IDT/ltr → `init_lApic()` → 等 `smp_scheduler_lock` | `smp.cpp:105-150` |
| 6 | AP idle 构造 → 入本核队列 → 设 current → **`scheduler_is_ready++`** → `enable_intr()` | `smp.cpp:152-176`（`++` 在 `:174`）|
| 7 | BSP idle 构造 → 入 `kernel_group->thread_queue` 与本核队列 → 设 current | `main.cpp:521-541` |
| 8 | VFS/pipefs/procfs/syscall 初始化 → `init_reaper()` | `main.cpp:553-566` / `:587` |
| 9 | ★ **`while (true) { pause; if (scheduler_is_ready == xsi->cpu_count) break; }`** ★ | `main.cpp:581-585` |
| 10 | 第一个用户进程 → 用户线程 → `enable_scheduler(); open_interrupt; no_interrupt = false;` | `main.cpp:600-601` / `:610-612` |

**★ 关于"登记"的两处更正 ★**

1. `scheduler_is_ready` 是**整数计数器**（`smp.cpp:26 int scheduler_is_ready = 0;`），**不是位图**；比较用 `==`（`main.cpp:584`），分母 `xsi->cpu_count` 由 `smp.cpp:395/407/428/440` 自增（BSP 也占一格）。
2. ★ **BSP 的 `++` 与 AP 的 `++` 语义不对称** ★：BSP 的 +1 发生在 `process_setup()`（`pcb.cpp:2729`），**那时 BSP 的 `scheduler_queue` 还不存在**（要到 `smp.cpp:390` 才 `queue_init()`、`main.cpp:535` 才把 idle 入队）；AP 的 +1 才是"**idle 已注册**"。
   ⇒ §2.2 那句"BSP 等所有 AP 登记完 idle 才继续"**只对 AP 成立**；BSP 自己那一位在语义上是"进程子系统地基已铺好"。
   ⚠ 另一处易混：`start_ap()` 里等的 `*ready == 1`（`smp.cpp:237-246`，由跳板 `smp_trampo.S:114-115` 置位）只代表"**AP 进了 64 位跳板**"，**不代表 idle 已登记** —— 这就是 `main.cpp:581-585` 必须存在的理由。
   两个 `++` 都非原子，但时序上不重叠（`process_setup` 在 `init_smp` 之前）。

### 10.2 两个 idle 的**补充**（接 §8.5）

| 项 | BSP | AP |
|---|---|---|
| 是否登记进 `kernel_group->thread_queue` | ✅ `main.cpp:532` | ❌（`smp.cpp` 全文无此调用）|
| `fpu_context` | `save_fpu_context:539` | 只靠 memset 清零（**restore 的是全 0 FXSAVE 区**）|
| `set_kernel_stack` 的目标 | 全局 `tss0.rsp[0]`（`smp.cpp:289-293` 的 BSP 特判）| 本核 `info->tss0.rsp[0]`（`smp.cpp:313`）|
| `cpu_id` 的"单位" | 未赋值 → 0 | `lapic_id()`（★ 与 `add_task` 用的**下标**不同，见 §10.3）|
| 未赋值而保持 0 的字段 | `wakeup_time`/`user_info`/`main`/`syscall_stack`/`user_stack(_top)`/`eevdf_*`/`context0.{rip,rflags,cs,ss,ds,es}` | 同（除 `rflags`）|

### 10.3 ★ 新发现：三个"单位不一致 / 死字段 / 4× 记账偏差"

| # | 发现 | 证据 | 为什么要记 |
|---|---|---|---|
| 1 | ★ **`cpu_id` 有三套单位**：`add_task` 写的是**下标**（`scheduler.cpp:553 new_task->cpu_id = min_cpu_index;`）；`create_kernel_thread`/`create_user_thread` 写的是 **ACPI UID**（`pcb.cpp:2203`/`2294 = get_current_cpu()->processor_id`）；AP idle 写的是 **LAPIC ID**（`smp.cpp:159`）| 而 `remove_task` 按**下标**用（`scheduler.cpp:566-567 task->cpu_id < get_cpu_num()` + `get_cpu(task->cpu_id)`）| 只有 `add_task` 那一路是自洽的；另两路在"UID≠下标"的机器上会摘错队列（`remove_task` 有全核扫描兜底，所以是隐性 bug 而不是崩溃）——**我们 ARM 侧别抄这个字段语义，或者抄完要统一** |
| 2 | ★ **`iter_node` 是死字段** ★ | `include/smp/smp.h:45` 定义；全树**无任何写入** | §2.1 的每核结构表要标掉 |
| 3 | ★ **tick 记账粒度可能差 4×** ★ | 周期装载 `apic.cpp:164 calibrated_timer_initial = (lapic_timer * 1000) / 250;` ⇒ 推导周期 ≈ 4 ms；而 `scheduler.cpp:19 EEVDF_TICK_NS = 1000000ULL`（按 **1 ms/tick** 记账）、`TIME_SLICE = 4` tick | 若推导成立，实际时间片 ≈ 16 ms 而不是 4 ms（**推导，未实测**，列为 UNVERIFIED）。对我们的意义：**别把源 OS 的时间片数字当基准** |
| 4 | ★ **启动期所有线程都落在 CPU0** ★ | `add_task:540-549` 的挑核循环判据是**严格小于**，而各核初始队列长度都是 1（各自 idle）⇒ 平局给核号小的；`TASK_APPLICATION_LEVEL` 直接跳过整个扫描 | ⇒ reaper（level 0）与首个用户线程（level 2）都进 CPU0；AP 队列里永远只有它自己的 idle（`scheduler.cpp:178` 排除 level 1）⇒ **源 OS 的 AP 实际上是闲着**。这与我们 M4-10 实测到的"探针成批落到另一个核"是同一个规则的不同数据（我们的探针是 level 0 但**数量多**，队列长度会拉开 ⇒ 开始迁移） |
| 5 | `scheduler_tick()` 在 `include/task/scheduler.h:13` **声明但全树无定义** | — | 死声明 |
| 6 | 同特权级中断（ring0→ring0）硬件只压 RIP/CS/RFLAGS ⇒ `reg->rsp/ss` 读到的是栈上残留 | `include/task/pcb.h:38-44` 的 `// CPU自动压入` 段 | **只有 ring3→ring0 被抢占时**，`context0.rsp/ss` 才是用户栈 —— 这正是"用户线程被抢占能存对用户栈"的前提；我们 ARM 侧的等价假设要显式写出来 |

### 10.4 `context0.rip == 0` 分支的**副作用更正**（接 §8.6）

`scheduler.cpp:432-455` 原文里有一段顺序问题：

```cpp
436:        if (current->status == RUNNING) { current->status = START; }
439:        if (best->status == START || best->status == CREATE) { best->status = RUNNING; }
444:        if (best->context0.rip != 0) { … change_proccess … } 
448:        else { current->status = RUNNING; cpu->scheduler_ticks = 0; }   // ★ 只回滚 current
```

⇒ 当 `best` 是一个 `rip == 0` 的 idle 时：**`best->status` 被改成 `RUNNING` 后不会回滚**，
于是这个 idle 永久停在"**RUNNING 但从未运行**"上（每 tick 可再被选中、再被放弃）。
在 AP 上这恰好"自洽"（AP 只有自己的 idle，`is_current_task_runnable:187-197` **不检查 level**，
所以 AP 每次都能"继续跑自己"）；但对我们移植的启示是：
**"选中的目标上下文无效"这条分支必须成对地回滚双方状态**，否则会留下假的 RUNNING。

### 10.5 PROCESSOR_INFO 字段与 `handler.S` 常量的**交叉验证**（第二批实测）

| 字段 | 偏移（布局探针实测） | `handler.S` 常量 |
|---|---|---|
| `current_task`（`smp.h:43`）| **0x4c0** | `CPU_CURRENT_TASK = 0x4c0` ✔ |
| `syscall_user_rsp`（`:47`）| **0x4e0** | `CPU_SYSCALL_USER_RSP = 0x4e0` ✔ |
| `syscall_user_rax`（`:48`）| **0x4e8** | `CPU_SYSCALL_USER_RAX = 0x4e8` ✔ |

⇒ **与 §8.2 的 TCB 偏移一样，`PROCESSOR_INFO` 也是"汇编级 ABI"**：`sizeof(PROCESSOR_INFO) = 0x4F0`（1264 B），
`pcr_inf[256]` ≈ 316 KB，由 `smp.cpp:318 xsi = (…)malloc(sizeof(XSK_SMP_INFO));` 分配（**未 memset**，
只靠堆区初值清零），`pcr_inf[cpu_count..255]` 从不初始化。
⚠ BSP 条目里的 `gdt_entries_t`/`gdt_pointer`/`tss0`/`tss_stack` **是死字段**（BSP 用全局 `gdt.cpp:9-13` 那一套）。

---

## 11. 全量扫描·第三批：持有者审计与内存算术

> 第三批做两件事：① 把 §8.8 的持有者表**扩到全树**（含内核模块）；② 把"一个线程到底吃多少内存"**算到字节**。
> 其中 TCB/PCB 的尺寸是**用宿主 clang 直接编译真实头文件测出来的**（`pcb.cpp:84-87` 的两条 `static_assert`
> 在真实布局下成立 ⇒ 尺寸可信）。

### 11.1 尺寸（实测）

| 结构 | 尺寸 | 说明 |
|---|---|---|
| `struct thread_control_block` | **3104 B**（0xC20，align 16）| 组成：`context0` 176 + `fpu_context` 512 + `actions[64]` 2048（`sigaction_t` 32 B）+ 其余字段 |
| `struct process_control_block` | **400 B** | — |
| `KERNEL_STACK_SIZE` | **1 MiB** | `proto.hpp:5-7,15` = `build_settings.h:64-66` |
| `BIG_USER_STACK` | **16 MiB**（懒分配，按缺页 4 KiB 实扣）| `proto.hpp:8-9,16` |
| 引导期 `STACK_SIZE`（临时栈）| **256 KiB** | `build_settings.h:60-62,158`；`boot/bootx64.c:1218-1222` 预付 256 个 ⇒ **64 MiB 内核不可见** |
| `lock_node` | 32 B | `include/lock_queue.h:6-12` |

⇒ **内核线程一次 `create_kernel_thread` = 1 MiB（帧，buddy order 20，无内部碎片）+ 3104 B（堆）+ 2×32 B（两个入队节点）≈ 1 MiB + 3.17 KiB。**
用户线程（`create_user_thread`/`thread_clone`/`message_thread`）= **2 MiB + 3.17 KiB** + 16 MiB 懒预约。

**耗尽量级**（默认 `-m 8192`，`tools/ninja_build.py:373-374`）：8 GiB ≈ 2 097 152 帧，先扣掉
内核堆竞技场 256 MiB（`main.cpp:334-353`，**逐页 `alloc_frames` 且永不归还**）、`page_maps` 8 MiB、
双 bitmap 512 KiB、引导期预付 64 MiB、内核/驱动/FS 数十 MiB ⇒
**约 7 700–8 000 次 `create_kernel_thread` 后 `alloc_frames` 返回 0**（`pcb.cpp:2208-2214` 返回 `-ENOMEM`）。
堆侧不是瓶颈（256 MiB / 3.17 KiB ≈ 8 万）。**全树没有 `MAX_TASK`/`MAX_THREAD`/`MAX_PROC` 之类的上限常量。**

⚠ **`free_frames` 有两个静默早退**：`bitmap_get(&using_regions,…)==false` 或 `bitmap_get(&frame_allocator.bitmap,…)==false`
就 `return`（`buddy.cpp:354-358`）⇒ 参数算错时**不报错、也不释放**（我们移植时若沿用"栈顶 - 栈大小"反推，务必确认这一步）。
⚠ 同一函数还有一道**引用计数闸门**：先逐页 `address_unref`，再要求全部 `address_can_free` 才真正 `buddy_free_zone`
（`buddy.cpp:364-376`）⇒ **只要有一处漏配对 `page_ref`/`address_unref`，那一帧就永久回收不了**（源 OS 里页表共享会用到这个计数）。

### 11.2 ★ 新发现：`MemFree` 是假的 ★

`frame_allocator.usable_frames`（`include/mm/frame.h:11`）**只在 `alloc_frames_l` 里递减**（`frame.cpp:220`），
free 侧的两处 `usable_frames++` **都被注释掉了**（`frame.cpp:252`、`:285`），buddy 路径也不更新它
⇒ 它从开机起几乎不动。而它正是 `/proc/meminfo` 的 **MemFree/MemAvailable**（`procfs.cpp:139-140`）
与 `sysinfo.freeram`（`sys.cpp:4881-4882`）的数据源 ⇒ **看 MemFree 判断"有没有泄漏"是不可靠的**。
真正的空闲量是 `zone->free_pages`（`buddy.cpp:189/219`），**没有任何接口暴露**。
⇒ 对我们的意义：**板上判据必须是自有计数器**（`g_kstack.used_slots` / `kstack_headroom` / `sched_cpu_runq_len`
这一套是对的，继续用；别去参考源 OS 的 MemFree）。

### 11.3 `tcb_t` 持有者：**全树**清单（把 §8.8 从 8 类扩到 10+8 类）

**A. 直接持有 `tcb_t` 的（10 类）**

| 编号 | 持有者 | 写入 | 注销 | 释放 TCB 后 |
|---|---|---|---|---|
| A1 | 每核 `current_task`（`smp.h:43`）| `main.cpp:541`、`smp.cpp:169/172`、`scheduler.cpp:447` | ★ 只被覆盖，从不注销 ★ | 悬空（要"不在任何核 current 上"这一条守卫）|
| A2 | 调度队列节点 `lock_node.data`（`lock_queue.h:8`）| `lock_queue.cpp:82`（调用点 `scheduler.cpp:554`、`main.cpp:535`、`smp.cpp:170`）| ✅ `remove_task:572/588` | 安全（先摘后 free）|
| A3 | `tcb.sched_node`（`pcb.h:176`）| `scheduler.cpp:554` | ✅ `:573/589` 置 NULL | — |
| A4 | `pcb.thread_queue` 载荷（`pcb.h:93`）| `pcb.cpp:2247/2381/2538/2124/1275`、`main.cpp:532` | ✅ `kill_proc0:372-380` | 安全（先 dequeue 后 free）|
| A5 | 僵尸的 `thread_queue`（长期驻留）| `kill_proc(is_zombie=true)` `pcb.cpp:255-282` 只置 DEATH | 父 `wait4` ⇒ `kill_proc(...,false)`；**内核组没有 wait4 调用者** | ★ 父不 wait ⇒ TCB/2 MiB 栈/页目录**永久泄漏** ★ |
| A6 | `mutex_t.owner`（`mutex.h:17`）| `mutex.cpp:54/96`；清 `:130/156/15` | ★ **线程死亡时从不注销** ★ | 悬空 + 活锁。实例：`sys.cpp:1824 mm_op_lock`、`driver/fs/fatfs/fatfs.cpp:34 fatfs_operate_lock`、`driver/device.cpp:18 blk_cached_bounce_lock[256]`（都永不 destroy）|
| A7 | netserver `sys_mutex_t.owner`（`kmod/netserver/arch/sys_arch.h:10`）| `arch/sys_arch.cpp:140`（owner = `(void*)get_current_task()`）| 清 `:153/162` | 同 A6 ⇒ lwIP 侧死锁 |
| A8 | netserver `g_core_owner`（`arch/sys_arch.cpp:16`）| `:95` | 清 `:111` | 悬空 + 核锁死锁 |
| A9 | 管道 `blocking_read/blocking_write` 的 `task_block_list_t.thread`（`pipe.h:8/21-22`）| 唯一写 `pipefs.cpp:27` | ✅ 但**只能整条 drain**（`:35 head->next=NULL` + `:39 free(node)`），**没有"按线程撤单"的 API** | ★ 见 §11.4 的 P-1 ★ |
| A10 | 饥饿监视器 `starve_watch_t.task` | **只在 ARM 侧**（`arch/arm32/src/kmain.c:803/808/863`，清 `:867`）；x86_64 内核里**不存在** | — | 我们自己的东西（源 OS 没有）|

**B. 不直接持 `tcb_t`、但依赖其生存期（只列会出问题的）**

| 编号 | 对象 | 谁清 | 风险 |
|---|---|---|---|
| B1 | `proc_handle_t.task`（`include/procfs.h:12`）| **只被 `procfs_on_exit_task` 清**，而它**只被 `kill_proc0:361` 调用** | ★ `free_partial_process`（fork 失败路径）**不调用**它 ⇒ `/proc/<pid>/*` 的 handle 悬空（E2）★ |
| B2 | `pcb.procfs_node/proc_root` | `procfs.cpp:1345` | 常规退出安全（`:361` 先于 `:420`）|
| B3 | `pcb.parent_task`（`pcb.h:92`）| 写 `pcb.cpp:1995/2572` | ★ **父死时没有 reparent**（唯一做这事的 `kill_proc_deferred:303` 是死代码）⇒ 子进程 `parent_task` 悬空；解引用点 `scheduler.cpp:424-428`、`pcb.cpp:281`、`procfs.cpp:448/524`、`vfs.cpp:1255-1258`、`x3tp.cpp:36` ★ |
| B4 | `pcb.child_pcb` | `kill_proc0` **既不 destroy 也不给子进程改 parent_task** | 泄漏 + 子进程悬空（E1）|
| B6 | `driver/hda/hda.cpp:57 static pcb_t use_task;` | 从不注销；**写后从不读** | 悬空但当前无害 |
| B7/B8 | `pcb.notify_pcor_tid`、`pty.cpp:134 ctrl_pgid` | — | 存的是 **tid/pid 数值**，不是指针 ⇒ 只逻辑陈旧，安全 |

**点名但实测"不持 tcb"的**（顺带把 §2.1 的疑点全清掉）：
futex（无名单，`sys.cpp:3106-3131`）、epoll/poll（`poll.h:55-58` 只有 fd + event）、ipc（只持 `ipc_message_t`，内含 `int pid`）、
`PROCESSOR_INFO.iter_node`（**死字段**，全树零引用）。

### 11.4 ★ 管道那条 UAF（P-1）：§5 耦合项 #3 要升级 ★

`pipefs.cpp` 里 `blocking_read/blocking_write` 的**全部 10 个操作点**（逐行核过）：
`:12-19` 查重（只读）、`:21-31` 加入（**唯一写 tcb**）、`:33-42` drain（**唯一清空**）、`:58-64` 挂起自己；
触发点 `:92`（读成功唤醒写者）、`:100`（读者挂起）、`:117`（写成功唤醒读者）、`:121`（写者挂起）、
`:181`（最后一个 **writer** close ⇒ drain `blocking_read`）、`:189`（最后一个 **reader** close ⇒ drain `blocking_write`）、
`:194/:205-206`（两端都没了 ⇒ `free(pipe->buf)/free(pipe)`）。

> ★ **P-1（本次审计最明确的一处 use-after-free）**：
> 一个**读者**因为**自己进程被拆卸**而消失时，走的是 `:189`（drain `blocking_write`）——
> **它自己的 tcb 仍然留在 `blocking_read` 里**，紧接着 `pcb.cpp:377 free(thread)`。
> 此后对端（另一个进程）`write` ⇒ `pipefs.cpp:117` → `:38 scheduler_wake_task(已释放 tcb)`
> → `scheduler.cpp:495-508` 读 `status`/写 `wakeup_time`/`status`/`eevdf_*` = **对已释放内存的写**。
> **触发条件是"管道跨进程"**（对端 fd 不在被拆进程的文件表里，`pipe` 不会被 `:206` 一起带走）。
>
> 我们这边的对应结论（**必须记进 §5**）：**远端 OS 靠"先关 fd 再释放线程"的顺序只能覆盖"两端同进程"的情形**；
> 一旦我们的管道/等待名单也可能跨进程（或跨"生命周期不属于同一 owner"），
> **必须在"线程退出"时就把自己从所有等待名单里撤掉**，不能只依赖"关 fd 顺带 drain"。

**P-2（不涉 tcb，但同源）**：`pipe_wait_on` 返回后 `while(true)` 重新取 `pipe->lock`（`:84`/`:109`）；
若窗口内两端 fd 都关导致 `:206 free(pipe)` ⇒ 线程操作**已释放的 `pipe_info_t`**。

### 11.5 另外三条"顺序覆盖不到"的缺口（与 M4-11 的回收者直接相关）

| 编号 | 缺口 | 证据 |
|---|---|---|
| E1 | `kill_proc0` **不 reparent 子进程、不销毁 `child_pcb`** | `pcb.cpp:359-423` 全文没有 `child_pcb` 的 destroy；唯一 reparent 的 `kill_proc_deferred:303` 是死代码 |
| E2 | `free_partial_process`（`pcb.cpp:1821-1843`）**不调 `procfs_on_exit_task`**；而 `process_fork` 在 `:2136 procfs_on_new_task` 之后 `:2137 add_task` 失败 ⇒ `:2140` 走它 ⇒ `/proc/<pid>` handle 悬空 | 对照常规路径 `:361` 先撤 procfs 再 `:420 free(pcb)` |
| E3 | `kill_proc0:362-368` 只是各核"下次 tick 才会看到 DEATH"，随后 `:370` 关 fd 表、`:488 free_frames(kernel_stack)` ⇒ **跨核没有 stop-the-world**，在途 syscall 可能还在用那根内核栈 | 与 §9.5(b) 同源：**回收的前置条件只能靠"不在任何核 current 上"这类可见性判据**（reaper 的做法），不能靠"我已经置 DEATH 了" |

### 11.6 又有几条死代码 / 空壳（本次全树确认）

| 符号 | 位置 | 状态 |
|---|---|---|
| `kill_proc_deferred` | `pcb.cpp:292-324` | 无调用者（§9.3）|
| `ipc_free_type` | `ipc.cpp:12-22` | **无调用者** |
| `switch_to_kernel_stack` | `pcb.cpp:2697-2700` | **无调用者** |
| `scheduler_tick` | 声明 `include/task/scheduler.h:13` | **无定义** |
| `process_fork` 尾部 `:2164-2174` | — | **不可达**（在 `:2153 while(true)` 之后）|
| `create_user_thread_from_file` | `user.cpp:1034` | 无调用者（只有 `proto.hpp:243` 声明）|
| `installer_launch_app` | `kernel/installer_mode.cpp:141-157` | 无调用者 |
| `do_message` | `message.cpp:155` | 无调用者 |
| `XapiTaskInfo.thread_count` | `include/syscall/pxapi.h:273` | **从未被填充**；`do_xapi_GetTaskList` 只有声明（`pxapi.h:318`），无实现 |
| 真正被发送的 IPC 类型 | — | 只有 `IPC_MSG_TYPE_EPID`（`:269-281`）与 `IPC_MSG_TYPE_EXEC`（`:1660-1663`）；`NONE/EXIT/KEYBOARD/MOUSE/TIMER` 定义但无人发送 |
| `pcb.cpp:409 queue_destroy(ipc_queue)` | — | 只释放节点、**不释放 `ipc_message` 载荷**（对比 `execve` 路径 `:1674-1682` 逐个 free）⇒ 消息体泄漏 |
| IST / `tss_stack` | `idt.cpp:78 p_gate->ist = 0;`（"不使用IST"）；`smp.h:33 typedef uint8_t tss_stack_t[1024]` | ★ **IST 从没被使用**，`gdt.cpp:26` / `smp.cpp:139` 设的 `tss_stack`（1024 B/CPU）是**死代码** ★；中断/异常用的是该线程的 `kernel_stack`（TSS RSP0，`scheduler.cpp:100 set_kernel_stack`）|
| `KERNEL_TASK_STACK_SIZE`（`build_settings.h:159`）| — | **死宏**（真正生效的是 `proto.hpp:15` 的 `KERNEL_STACK_SIZE`）|
| TCB 的堆开销 | `liballoc-x86_64.a`（预编译，源码不在树内）| 每次分配的确切元数据开销 **UNVERIFIED**（量级 ≤ 几十 B，不影响"1 MiB/线程"）|

### 11.7 `execve` 的一个语义缺口（M7 前要知道）

`process_execve`（`pcb.cpp:1296-1707`）全文**没有遍历线程表** ⇒ **不杀同进程的其他线程**。
其他线程的 TCB/内核栈保留，但它们的 `user_stack` 仍指向**已被释放的旧地址空间**（`:1666 if (!was_vfork) free_page_directory(old_page_dir);`）。
Linux 会杀掉其他线程；源 OS 这里是个已知语义缺陷（我们 M7 若做多线程 + exec，必须自己定规则）。

### 11.8 运行时线程清单（默认构建，用于对照我们的"30 个探针"）

开机会常驻：`Process Reaper`×1（`reaper.cpp:65`）＋ `xhci-svc`×1（默认 BUILTIN_XHCI，`xhci.cpp:3107`）＋
`usb-hotplug`×1（`usb_core.cpp:204`）＋ `netserver`×1（`netserver.cpp:614`）＋ lwIP `tcpip` 线程（`sys_arch.cpp:256`）＋
idle×4（BSP 不占帧、AP×3 用引导栈）⇒ **帧消耗约 3–6 MiB**，与"个位数线程"一致。
（精确条数取决于 `/mod` 里的 `.sys` 与 USB 枚举结果 ⇒ **UNVERIFIED**。）

---

## 12. 一页纸：这次扫描**改变了什么 / 没改变什么**

**没有改变的（计划照走）**

1. §4.1 的判断：**"线程退出 + 回收"在我们这边是必需，不是可选**（32 槽 vs 每轮 ~30 个探针）——本次扫描反而加强了它（源 OS 每线程 1–2 MiB，耗尽量级 7 700+ 次，纯靠"用得少"才没暴露）。
2. §4.3 的方案 A（**垂死线程只置 DEATH + 让出 + `wfi`；回收者摘队 + 还栈 + 还 TCB**）。
3. §6 的三步顺序（11.1 mutex → 11.2 退出+回收 → 11.3 `sched_park_self` 退场）。

**改变 / 需要加进去的**

| # | 事项 | 依据 |
|---|---|---|
| 1 | 回收者的判据必须是"**`status` 已死/僵尸** **且** **不在任何核的 current 上**"，两条都要 | §9.3、§11.5 E3、`reaper.cpp:38` 正例 |
| 2 | **垂死线程必须把自己从所有等待名单里撤掉**（我们的管道/等待名单若可能跨 owner，源 OS 的"关 fd 顺带 drain"不够）| §11.4 P-1 |
| 3 | `mutex->owner` 的注销/拒绝要写成不变量；顺带注意源 OS 还有两处同类（netserver 侧）| §11.3 A6/A7/A8 |
| 4 | **不要引 `FUTEX` 状态**（源 OS 里它是 SIGSTOP 的死胡同，无唤醒路径）| §8.3、§9.6 |
| 5 | 时间片/记账的**数字不要照抄**（源 OS 的 tick 可能是 4 ms 而按 1 ms 记账）| §10.3 #3（推导，UNVERIFIED）|
| 6 | `cpu_id` 语义在源 OS 里有**三套单位**；我们若保留这个字段，必须只留一套 | §10.3 #1 |
| 7 | "选中的目标上下文无效"这类分支**必须成对回滚双方状态**（源 OS 只回滚了 current）| §10.4 |
| 8 | 板上判据继续用**自有计数器**，别信 MemFree（源 OS 的 MemFree 永远不减）| §11.2 |
| 9 | 若 M7 做"多线程 + execve"，语义要自己定（源 OS 不杀其他线程）| §11.7 |

**本次新增的 UNVERIFIED（别当已知）**

- tick 真实周期（4 ms 是推导）、PIT 是否也打 vector 32；
- AP idle 的 `fpu_context` 全 0 后 `restore_fpu_context` 是否总安全；
- `pcr_inf` 是否被 `kmod/**`、`driver/**` 写过（只在 `kernel/**` 内查过）；
- §11.3/§11.4 的那些悬空/UAF **都是代码路径级结论，没有跑 QEMU 复现**（本机也没有 `out/kernel.krl`）；
- `08e5c9c` 与我们 `kernel/**` 的"逐字节相同"是用 `git diff --stat 08e5c9c HEAD -- kernel include` 为空确认的
  —— 也就是说 **本文件所有行号对 `main@08e5c9c` 有效**，对我们分支的工作树同样有效。
