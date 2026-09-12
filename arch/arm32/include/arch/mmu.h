#pragma once

/*
 * ARMv7-A 短描述符(Short-descriptor)地址转换
 *
 * 这是移植计划 §2.3 里"单项风险最高"的那块:x86_64 侧的
 * include/mm/page.h 用的是 4 级页表 + PTE_* 位语义,
 * 那套词表在 ARMv7 上**没有对应位置**,必须整体重做。
 *
 * 与 x86_64 的根本差异:
 *
 *   |          | x86_64              | ARMv7-A 短描述符            |
 *   |----------|---------------------|-----------------------------|
 *   | 级数     | 4 (PML4/PDPT/PD/PT) | 2 (L1 / L2)                 |
 *   | 项大小   | 64 位               | **32 位**                   |
 *   | 基本粒度 | 4KB                 | L1 段 = 1MB,L2 小页 = 4KB   |
 *   | 一级表   | 512 项              | **4096 项**(4GB/1MB)        |
 *   | 权限     | U/S 位(单一)        | AP[2:0] 三级 + Domain 域    |
 *   | 内存类型 | PWT/PCD 两位        | TEX[2:0]+C+B+S 组合         |
 *   | 不可执行 | NX = bit63          | XN,**在 L1 和 L2 位置不同** |
 *
 * ⚠ 本文件只定义"描述符长什么样",不含任何 MMIO 或 CP15 操作 ——
 *   这样宿主 gcc 就能编译它,单测见 tests/test_arm32_mmu.py。
 *   CP15 原语在 arch/cpu.h,开 MMU 的流程在 src/mmu.c。
 *
 * 属性常量来源:AMD/Xilinx standalone BSP(MIT 许可)
 *   lib/bsp/standalone/src/arm/cortexa9/xil_mmu.h
 *   lib/bsp/standalone/src/arm/cortexa9/gcc/translation_table.S
 *   lib/bsp/standalone/src/arm/cortexa9/gcc/boot.S
 * 这些值在 Zynq-7000 上经过大量产品验证,直接沿用比自行推导更稳妥。
 * 每个常量都标注了它的位域解码,便于核对。
 */

#include <arch/types.h>

/* ------------------------------------------------------------------ */
/* 地址转换表几何                                                       */
/* ------------------------------------------------------------------ */

#define MMU_SECTION_SHIFT   20u
#define MMU_SECTION_SIZE    (1u << MMU_SECTION_SHIFT)   /* 1MB */
#define MMU_SECTION_MASK    0xFFF00000u                 /* 段描述符里的物理基址字段 */

/*
 * 一级表:4096 项 × 4 字节 = 16KB,覆盖整个 4GB 地址空间。
 * ⚠ 16KB 对齐不是"建议"而是硬性要求:TTBR0 低 14 位是属性字段,
 *   表基址必须 16KB 对齐,否则硬件取到的是错位的表。
 */
#define MMU_L1_ENTRY_COUNT  4096u
#define MMU_L1_TABLE_SIZE   (MMU_L1_ENTRY_COUNT * 4u)   /* 16KB */
#define MMU_L1_TABLE_ALIGN  MMU_L1_TABLE_SIZE
#define MMU_L1_INDEX_SHIFT  MMU_SECTION_SHIFT
#define MMU_L1_INDEX_MASK   0xFFFu

#define MMU_PAGE_SHIFT      12u
#define MMU_PAGE_SIZE       (1u << MMU_PAGE_SHIFT)      /* 4KB */
#define MMU_PAGE_MASK       0xFFFFF000u

/* 二级表:256 项 × 4 字节 = 1KB,覆盖一个 1MB 段 */
#define MMU_L2_ENTRY_COUNT  256u
#define MMU_L2_TABLE_SIZE   (MMU_L2_ENTRY_COUNT * 4u)   /* 1KB */
#define MMU_L2_TABLE_ALIGN  MMU_L2_TABLE_SIZE
#define MMU_L2_INDEX_SHIFT  MMU_PAGE_SHIFT
#define MMU_L2_INDEX_MASK   0xFFu

/* ------------------------------------------------------------------ */
/* 描述符类型                                                           */
/* ------------------------------------------------------------------ */

/*
 * ⚠ 一级和二级的位含义**不通用**,这是最容易按 x86 直觉写错的地方。
 *
 * 一级描述符(bits[1:0] 就是类型):
 *   0b00  fault          —— 访问即产生 translation fault
 *   0b01  page table     —— 指向二级表(表基址在 bits[31:10])
 *   0b10  section        —— 1MB 段(物理基址在 bits[31:20])
 *   0b11  reserved
 *
 * 二级描述符(bits[1:0] 不再单纯表示类型):
 *   0b00  fault
 *   0b01  large page     —— 64KB 大页
 *   0b1x  small page     —— 4KB 小页,**此时 bit0 是 XN 而不是类型位**
 *
 * 也就是说二级的"小页"是 0b10(XN=0)或 0b11(XN=1),
 * 不能像一级那样写 `(desc & 3) == 2` 就当作小页 —— 那会漏掉 0b11。
 */
#define MMU_DESC_TYPE_MASK      0x3u
#define MMU_L1_TYPE_FAULT       0x0u
#define MMU_L1_TYPE_PAGE_TABLE  0x1u
#define MMU_L1_TYPE_SECTION     0x2u
#define MMU_L1_TYPE_RESERVED    0x3u

#define MMU_L2_TYPE_FAULT       0x0u
#define MMU_L2_TYPE_LARGE_PAGE  0x1u

#define MMU_IS_L1_FAULT(desc)       (((desc) & MMU_DESC_TYPE_MASK) == MMU_L1_TYPE_FAULT)
#define MMU_IS_L1_PAGE_TABLE(desc)  (((desc) & MMU_DESC_TYPE_MASK) == MMU_L1_TYPE_PAGE_TABLE)
#define MMU_IS_L1_SECTION(desc)     (((desc) & MMU_DESC_TYPE_MASK) == MMU_L1_TYPE_SECTION)
#define MMU_IS_L2_FAULT(desc)       (((desc) & MMU_DESC_TYPE_MASK) == MMU_L2_TYPE_FAULT)
#define MMU_IS_L2_LARGE_PAGE(desc)  (((desc) & MMU_DESC_TYPE_MASK) == MMU_L2_TYPE_LARGE_PAGE)
/* 小页含 0b10 与 0b11 两种,所以判据是 bit1 而非等值比较 */
#define MMU_IS_L2_SMALL_PAGE(desc)  (((desc) & 0x2u) != 0u)

/* 二级表基址在 bits[31:10];一级页表描述符同样用它 */
#define MMU_L2_TABLE_MASK   0xFFFFFC00u

/* ------------------------------------------------------------------ */
/* 访问权限 AP[2:0] 与域 Domain                                         */
/* ------------------------------------------------------------------ */

/*
 * AP 与 x86 的 U/S 位完全不同:x86 只有"用户/超级visor"一个维度,
 * ARM 用 AP[2:0] 同时表达特权态和用户态的读写权限。
 *
 *   AP[2:0]  特权态(PL1)   用户态(PL0)
 *   0b000    无访问          无访问
 *   0b001    读写            无访问
 *   0b010    读写            只读
 *   0b011    读写            读写        <- "全权限",内核用这个
 *   0b101    只读            无访问
 *   0b110    只读            只读
 *   0b111    只读            只读
 *
 * ⚠ 权限是否生效取决于 DACR 里该域的模式:
 *   manager(0b11) 模式下 AP 被完全忽略,不会产生权限故障;
 *   client(0b01)  模式下 AP 才真正参与判定。
 *   开 MMU 的第一版会照 Xilinx 的做法把 DACR 设成全 manager,
 *   于是"任何权限错误都不会报错"—— 这是有意为之的降级,
 *   等有了用户态(M4)再切到 client 模式,见 src/mmu.c 的说明。
 */
#define MMU_AP_SHIFT_L1     10u
#define MMU_AP_SHIFT_L2     4u
/* AP[1:0] 的掩码。AP[2] 单独处理,因为它在一级/二级的位置也不同 */
#define MMU_AP_LOW_MASK     0x3u

#define MMU_AP_NONE         0x0u
#define MMU_AP_PL1_RW       0x1u
#define MMU_AP_PL1_RW_PL0_RO 0x2u
#define MMU_AP_FULL         0x3u  /* PL1 与 PL0 皆可读写 */
#define MMU_AP_PL1_RO       0x5u
#define MMU_AP_READ_ONLY    0x6u

/* AP[2]:一级在 bit15,二级在 bit9 —— 又一处一级/二级不一致 */
#define MMU_L1_AP2_BIT      (1u << 15)
#define MMU_L2_AP2_BIT      (1u << 9)

/*
 * Domain 只有一级描述符有(二级继承所在的一级项)。
 * 16 个域,每个域在 DACR 里占 2 位。
 */
#define MMU_L1_DOMAIN_SHIFT 5u
#define MMU_L1_DOMAIN_MASK  0xFu

#define MMU_DACR_CLIENT     0x1u  /* AP 生效 */
#define MMU_DACR_MANAGER    0x3u  /* AP 被忽略 */
#define MMU_DACR_NO_ACCESS  0x0u

/* 把 16 个域全部设成同一模式 */
#define MMU_DACR_ALL(mode)  ((u32)((mode) * 0x55555555u))

/* ------------------------------------------------------------------ */
/* 内存属性                                                             */
/* ------------------------------------------------------------------ */

/*
 * 一级**段**描述符的常用属性常量。
 *
 * 这些值包含:类型位(0b10)、AP=0b11、Domain=0b1111、TEX/C/B、S,
 * 但**不含物理基址** —— 基址由 MMU_SECTION_MASK 从地址里取,
 * 两者按位或起来才是完整的描述符(见 mmu_section_descriptor)。
 *
 * 位域解码(以 0x15DE6 为例):
 *   bit1:0   = 0b10        段
 *   bit2 (B) = 1
 *   bit3 (C) = 0
 *   bit4 (XN)= 0           可执行
 *   bit8:5   = 0b1111      Domain 15
 *   bit11:10 = 0b11        AP = 全权限
 *   bit14:12 = 0b101       TEX
 *   bit16 (S)= 1           共享
 * 注:TEX[2]=1 时 TEX[1] 与 C 相同、TEX[0] 与 B 相同,
 *     所以 0x15DE6 的 TEX=101 与 C=0,B=1 是自洽的。
 */
#define MMU_ATTR_NORMAL_WB      0x15DE6u  /* Normal,写回可缓存,共享(0x15DE6) */
#define MMU_ATTR_NORMAL_WT      0x16DEAu  /* Normal,写透可缓存,共享 */
#define MMU_ATTR_NORMAL_NC      0x11DE2u  /* Normal,不可缓存(TEX=001,C=0,B=0) */
#define MMU_ATTR_STRONG_ORDERED 0x00C02u  /* 强序(TEX=000,C=0,B=0),PL 默认 */
#define MMU_ATTR_DEVICE         0x00C06u  /* 设备内存(TEX=000,C=0,B=1),PS 外设 */
#define MMU_ATTR_RESERVED       0x00000u  /* 保留:访问即 translation fault */

/*
 * 位域掩码,用于在既有属性上叠加/清除单个特性。
 * ⚠ NON_SHAREABLE 是**掩码**不是值 —— Xilinx 原头文件把它和
 *   SHAREABLE 并列定义,很容易看成"一个可或进去的值",实则是取反用。
 */
#define MMU_ATTR_S_BIT          0x00010000u  /* bit16:共享 */
#define MMU_ATTR_SHAREABLE      MMU_ATTR_S_BIT
#define MMU_ATTR_NON_SHAREABLE  (~MMU_ATTR_S_BIT)

/*
 * XN(Execute Never)。Xilinx 用 (1<<4)|(1<<0) 一个常量同时覆盖一级和二级 ——
 * 因为一级段的 XN 在 bit4、二级小页的 XN 在 bit0。
 * 直接或进去即可,但要知道它对不同级别命中的是不同位。
 */
#define MMU_ATTR_XN             ((1u << 4) | (1u << 0))

/* ------------------------------------------------------------------ */
/* CP15 相关位定义                                                      */
/* ------------------------------------------------------------------ */

/*
 * SCTLR(System Control Register)常用位。
 * Xilinx boot.S 用的初值是 0b01000000000101 = 0x4005,
 * 即 M(MMU) + C(D-Cache) + RR(轮转替换),**不含 I(I-Cache)**。
 */
#define SCTLR_M   (1u << 0)   /* MMU 使能 */
#define SCTLR_A   (1u << 1)   /* 地址对齐检查 */
#define SCTLR_C   (1u << 2)   /* 数据缓存使能 */
#define SCTLR_I   (1u << 12)  /* 指令缓存使能 */
#define SCTLR_V   (1u << 13)  /* 异常向量基址:0=0x00000000,1=0xFFFF0000 */
#define SCTLR_RR  (1u << 14)  /* 缓存替换策略:1=轮转 */
#define SCTLR_Z   (1u << 11)  /* 分支预测使能 */
#define SCTLR_U   (1u << 22)  /* 不对齐访问使能(ARMv7 应置 1) */
#define SCTLR_XP  (1u << 23)  /* 异常向量:0=ARM 态 */
#define SCTLR_EE  (1u << 25)  /* 异常字节序:0=小端 */

/*
 * TTBR0 低位属性。Xilinx boot.S 的做法是把表基址与 0x5B 或起来
 * (原注释:Outer-cacheable, WB)。0x5B 全部落在低 14 位,
 * 也就是表基址字段之外,不会破坏基址本身。
 */
#define TTBR0_ATTR_XILINX  0x5Bu

/*
 * ACTLR(Auxiliary Control Register)位。
 * bit6 = SMP(参与 SCU 一致性),bit0 = 缓存/TLB 维护广播。
 * 单核阶段也要置位:SMP 位影响的是缓存一致性行为的定义,
 * 不置位会在后续加第二个核时出问题,而那时很难联想到这里。
 */
#define ACTLR_SMP           (1u << 6)
#define ACTLR_FW            (1u << 0)
#define ACTLR_XILINX_INIT   (ACTLR_SMP | ACTLR_FW)

/* ------------------------------------------------------------------ */
/* 纯函数:地址 -> 表索引                                               */
/* ------------------------------------------------------------------ */

/* 一级索引 = VA[31:20],即 1MB 段号 */
static inline u32 mmu_l1_index(u32 va)
{
    return (va >> MMU_L1_INDEX_SHIFT) & MMU_L1_INDEX_MASK;
}

/* 二级索引 = VA[19:12] */
static inline u32 mmu_l2_index(u32 va)
{
    return (va >> MMU_L2_INDEX_SHIFT) & MMU_L2_INDEX_MASK;
}

/* 段/页内偏移 */
static inline u32 mmu_section_offset(u32 va)
{
    return va & (MMU_SECTION_SIZE - 1u);
}

static inline u32 mmu_page_offset(u32 va)
{
    return va & (MMU_PAGE_SIZE - 1u);
}

/* 该地址所在段的起始地址 */
static inline u32 mmu_section_base(u32 addr)
{
    return addr & MMU_SECTION_MASK;
}

/*
 * 一级项覆盖的虚拟地址范围,用于诊断输出。
 * 返回该段最后一字节地址(含)。
 */
static inline u32 mmu_l1_index_end(u32 index)
{
    return (index << MMU_L1_INDEX_SHIFT) | (MMU_SECTION_SIZE - 1u);
}

/* ------------------------------------------------------------------ */
/* 纯函数:描述符编解码(实现在 src/mmu.c,便于宿主单测)              */
/* ------------------------------------------------------------------ */

/*
 * 按位域组合出一级段 / 二级小页的属性字。
 *
 * 存在的意义是把"一级和二级位位置不同"这件事收进一个地方:
 * 一级 AP[1:0] 在 bit11:10、TEX 在 bit14:12、AP[2] 在 bit15、S 在 bit16;
 * 二级 AP[1:0] 在 bit5:4、  TEX 在 bit8:6、 AP[2] 在 bit9、 S 在 bit10。
 * 把一级的属性字直接用到二级项上不会有任何编译或运行报错,
 * 只会让那一页的权限与缓存属性变得莫名其妙。
 *
 * @param ap3        AP[2:0],取 MMU_AP_* 之一
 * @param tex        TEX[2:0]
 * @param c          C 位(缓存)
 * @param b          B 位(缓冲)
 * @param domain     仅一级段有,0..15
 * @param shareable  是否共享
 * @param xn         是否禁止取指
 *
 * 单测用 Xilinx 的既有常量反推这些函数,确保位域理解正确。
 */
u32 mmu_l1_section_attr(u32 ap3, u32 tex, u32 c, u32 b, u32 domain, bool shareable, bool xn);
u32 mmu_l2_small_page_attr(u32 ap3, u32 tex, u32 c, u32 b, bool shareable, bool xn);

/*
 * 构造一级段描述符。
 * attr 取 MMU_ATTR_* 之一;pa 只保留 bits[31:20]。
 */
u32 mmu_section_descriptor(u32 pa, u32 attr);

/* 构造一级页表描述符(指向二级表)。domain 取 0..15 */
u32 mmu_page_table_descriptor(u32 l2_table_pa, u32 domain);

/* 构造二级小页描述符。attr 的位域按二级布局给出 */
u32 mmu_small_page_descriptor(u32 pa, u32 attr);

/* 从描述符里取出物理基址(按级别和类型判断字段宽度) */
u32 mmu_descriptor_pa(u32 desc, u32 level);

/*
 * 描述符类型的人可读文本,用于把页表打进串口/JTAG 时快速定位。
 * 传入 level(1 或 2)。返回静态字符串,不会为 NULL。
 */
const char *mmu_descriptor_kind_text(u32 desc, u32 level);

/*
 * 校验一级表是否符合硬件要求。
 * 检查项:表基址 16KB 对齐、非空指针。
 * 返回 0 表示通过,非 0 为错误码(见 mmu_l1_check_t)。
 */
typedef enum
{
    MMU_L1_CHECK_OK = 0,
    MMU_L1_CHECK_NULL = 1,
    MMU_L1_CHECK_MISALIGNED = 2,
} mmu_l1_check_t;

mmu_l1_check_t mmu_l1_table_check(const u32 *table);
