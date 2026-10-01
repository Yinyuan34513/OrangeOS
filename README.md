# Orange OS / 橙光 OS

> CHENG GUANG OS · NUBASY
> 橙光随行，流畅无界

一个 x86 引导扇区项目：从 512 字节的 MBR 开始，经过 VBE 图形模式和保护模式，
最终在 1024x768 的线性帧缓冲上把品牌画出来。没有内核、没有文件系统、没有
用户态——它是"只有 Logo 没有代码"的第一块代码。

QEMU 里的实际效果（`make shot` 生成 `build/screen.png`）：

- **Orange**（橙）+ **OS**（蓝），来自 DejaVu Sans Bold
- 蓝色弧线，二次贝塞尔 + 变宽竖线段
- 「橙光随行，流畅无界」，来自 Noto Sans CJK Bold
- `CHENG GUANG OS · NUBASY`

## 构建

```
make          # 生成 build/orangeos.img（1.44M 软盘镜像）
make shot     # 无头启动并截屏到 build/screen.png
make run      # 开个窗口直接看
make console  # 串口 shell 接到当前终端，可以直接敲命令
make test     # 自动回归：管道喂命令，校验响应，串口 I/O PASS / FAIL
make test-irq # 键盘中断回归：monitor sendkey 注入扫描码
```

依赖：`nasm`、`gcc-13`（支持 -m32）、`qemu-system-x86_64`、`qemu-img`、
`python3`、`ffmpeg`（用它的 drawtext 光栅化字体，所以需要带 freetype 的构建）、
`socat`。

写入 U 盘真机引导（小心覆盖数据）：

```
dd if=build/orangeos.img of=/dev/sdX bs=512 count=57
```

## 串口 shell

COM1（115200 8N1，轮询、无中断——IDT 还没建，开中断就是三重故障）上有一个
最小 shell：

```
Orange OS> help
commands:
  help     this list
  echo ..  print its arguments back
  sysinfo  framebuffer, CPU, build info
  logo     repaint the brand on the screen
  clear    clear the serial terminal
  poweroff QEMU ACPI poweroff
Orange OS> sysinfo
CPU vendor: AuthenticAMD
framebuffer: FD000000, 1024x768, 24bpp, pitch 3072
```

`sysinfo` 用 `cpuid` 读 CPU 厂商；`poweroff` 写 QEMU 的 PIIX4 ACPI
寄存器（端口 0x604）实现真关机。这是内核之外第一块真正的 I/O：guest 能收
输入、能解析、能回话。

### IDT 与键盘中断

`src/interrupts.c` 建了全 256 门 IDT（留空的门第一次被踩就是三重故障）、把
8259 重映射到 0x20 起（BIOS 留下的 0x08 正好是除零门）、只放行 IRQ0 时钟和
IRQ1 键盘，然后 `sti`——这是整个项目第一次开中断，从此 `hlt` 才是合法的
空闲指令。

CPU 异常不会默默 iret：0–31 每个向量一个独立处理函数，打印向量号后停机
（部分异常带 error code，iret 必错位，停机反而安全）。`sysinfo` 里的
`ticks`/`keys` 就是中断活着证据——ticks 在没人碰机器时也在涨。

输入做了统一：键盘走 IRQ1 → 扫描码表（set 1，含 shift/caps）→ 环形缓冲 →
`kgetc()`；串口仍是轮询。`getchar_any()` 先查键盘再查串口，两个口都能敲。

键盘路径的回归测试用 QEMU monitor 的 `sendkey` 注入 PS/2 扫描码：

```
make test-irq    # 敲入 echo KBDIRQ + sysinfo，校验 > KBDIRQ、ticks、keys
```

`> KBDIRQ` 这一行只有当 IRQ1 → 扫描码 → shift → 环形缓冲 → shell 整条链
都工作才可能存在。

### 内核堆

`src/heap.c`：在 PMM 之上做字节粒度分配。PMM 新增 `pmm_alloc_contiguous(n)`
（空闲链表按地址有序，连续 n 帧整段摘除），堆占用 2 MiB 连续区，经典 Knuth
边界标记：块头 {magic, size, next}、16 字节对齐、空闲链表按地址有序、
kmalloc first-fit + 尾部分割、kfree 插入后与物理前后块合并。

shell 新命令：`heap`（统计）与 `kmtest`（8 种尺寸的搅动测试：写入校验、
乱序释放、空闲字节必须精确复原）。`heap_init` 自带自检，开机即验证。

### 分页与物理内存分配器

`src/pmm.c` / `src/paging.c`，i386 两级分页（4K 页，未开 PSE）：

- **PMM**：stage 2 在实模式里用 `int 15h/E820` 抓内存地图存到 0x6000；
  `pmm_init()` 解析它，把所有 type=1、1 MiB 以上的帧串成**空闲链表**——
  链表指针就放在空闲帧自己的头 4 字节里，零元数据。分配弹表头，释放压回。
  初始化时自带 8 帧分配/释放自检（唯一性、对齐、计数恢复）。
- **分页**：CR3 指向 PMM 分出的页目录，两层映射——所有可用 RAM **恒等映射**
  （PG 翻转瞬间运行中的代码、栈、IDT 都不用搬家），加上帧缓冲的独立窗口
  `0xF0000000`（PCD 置位，扫描出表面不做 cache）。`sysinfo` 显示：
  ```
  memory: 3808 frames (14 MiB), free 3802
  paging: on, identity to 15 MiB, fb mapped at 0xF0000000
  ```
  屏幕此后通过页表翻译写帧缓冲——**画面正确本身就是分页工作的证明**。

顺序是刻意安排的：IDT 先建（分页出错时异常处理能打印向量号），PMM 再建
（页目录和页表都从它分配），最后才开 PG。

### 踩过的坑：BIOS 寄存器约定

e820 有三连坑，全靠串口诊断定位：**EBX 没清零**（残留 payload 缓冲区偏移，
BIOS 视为无效续读请求返回"不支持"）；**BP 被 BIOS 覆盖**（e820 只担保
EBX/ES:DI，计数器放寄存器就是灾难，改放内存）；**DI 重算时顺手把 BX 用成
草稿**（续读 ID 又没了）。结论：跨 BIOS 调用只依赖规范担保的寄存器，
其余一律走内存。

## 它是怎么工作的

```
boot/boot.asm      一切汇编：MBR + 实模式 stage2
src/main.c         一切 C：清屏、排版、blit、弧线
boot/payload.ld    把 c_start 钉在 0x10000 的链接脚本
tools/mkfont.py    文字 -> 1-bit 位图 -> C 数组
tools/pad.py       把镜像补齐到引导代码要求的扇区数
```

启动流程：

1. **Stage 1**（512 字节，`0x7C00`）：探测 BIOS 是否支持扩展读
   （`int 13h/42h`），然后从磁盘加载两块——stage 2 到 `0x7E00`，
   C payload 到 `0x1000:0x0000`。不支持扩展读就退回 CHS 逐扇区读。
2. **Stage 2**（仍是实模式）：`int 10h/4F00h` 拿 VBE 信息，`4F01h` 查询
   0x0118 号模式（1024x768，这块 BIOS 给的是 24bpp），`4F02h` 带着线性帧
   缓冲位切换过去。把帧缓冲地址、分辨率、pitch、bpp 填到 `0x7000` 的
   bootinfo，开 A20，进保护模式，`call 0x10000`。
3. **C payload**（32 位）：读 bootinfo，白底、blit 字标、画弧线、排标语，
   然后进入串口 shell（轮询 COM1，无中断）。

诊断全部走 COM1（115200 8N1），因为切到图形模式后文本屏就再也看不见了。
`-serial file:build/serial.log` 接住它们。

## 踩过的坑（留给下一个人）

- **CHS 加载的 LBA 偏移**：软盘几何是 18 扇区、2 磁头，0 基 LBA 换算成
  1 基扇区号。起始 LBA 写成 1 会把 MBR 自己当 stage 2 加载——于是
  0x7E00 处执行的是 MBR 的副本，打印完 "Orange OS" 再跳回自己，串口
  刷屏。另外 LBA 计数器不能用 `cx`，`lba_to_chs` 会把它覆盖成 CHS 值。
- **VBE 缓冲区在 es:di**：加载 payload 时把 `es` 设成了 `0x1000`，之后
  忘了改回来，BIOS 就把信息块写到 `0x10000` 偏移处去了，读回来全是零。
  "VBE2" 签名也不能省，否则 BIOS 按 1.x 布局应答，帧缓冲地址拿不到。
- **mode 0x118 是 24bpp 不是 32bpp**：pitch 是 3072（1024×3）而不是
  4096。按 4 字节/像素写的话第一行之后全部错位，所以绘制按
  `BitsPerPixel` 自适应 3 或 4 字节。
- **位图的行距是 stride 不是 w**：1bpp 位图每行补齐到整字节，blit 的位
  偏移是 `row * stride * 8 + col`。写成 `row * w + col` 会让每一行横向
  错开几个像素——整行斜切的"抽风字"就是这么来的。

## 已知限制

- 依赖 BIOS 的 VBE 支持，UEFI 机器不认（那是另一条路）。
- 软盘几何写死 18/2，别的介质要改 `lba_to_chs`。
- 字体位图在构建期生成、按构建机的字体渲染，跨机器结果可能有细微差别。
- 24bpp 下逐像素画屏约 79 万次函数调用，QEMU 里毫秒级，真机也够快；
  但没有做按行 memset 的优化。
