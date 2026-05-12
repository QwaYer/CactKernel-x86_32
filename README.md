# CactKernel / x86_32



Гибридное монолитное ядро для i686.  
Низкоуровневые интерфейсы на **C** и **NASM**; менеджер памяти, планировщик, часть синхронизации и **сетевой стек (smoltcp)** — на **Rust** (`cact_mm`, `sched`, `cact_net`).

---

## Stats


|                  |                                                               |
| ---------------- | ------------------------------------------------------------- |
| **Syscalls**     | 95 (`SYSCALL_COUNT` в `syscalls.h`)                           |
| **CPU ISRs**     | 32                                                            |
| **PMM диапазон** | 0 … `0xE000_0000` (до PCI/MMIO hole, до ~3.5 GiB RAM по mmap) |
| **MAX_FD**       | 256 на задачу                                                 |
| **xHCI driver**  | ~32 KiB                                                       |
| **MLFQ levels**  | 4                                                             |
| **ext4 (ядро)**  | ~40 KiB (read/write, inode ops)                               |


---

## Связанные репозитории и полная сборка


| Компонент                                                      | Роль                                                                                            |
| -------------------------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| **[CactLib-x86_32](https://github.com/QwaYer/CactLib-x86_32)** | `libc.a` / `libc.so` — номера `SYS_*` должны совпадать с `syscalls.h`                           |
| **LocalRepoCactOS** (`../LocalRepoCactOS`)                     | Драйверы `*.cctk`, ELF в `lib/bin/`, упаковка `**cctkfs.img`** (модуль GRUB `module2 … cctkfs`) |
| `**../build-cact-qemu.sh**`                                    | Драйверы → `cctkfs.img` → `./build_disk.sh` (пустой ext4 `nvme.img`) → `make` ядра и ISO        |


Типовой QEMU-цикл из корня воркспейса:

```sh
./build-cact-qemu.sh          # полный цикл
# или только ядро (ожидается уже собранный ../LocalRepoCactOS/cctkfs.img):
cd CactKernel-x86_32 && make
```

---

## Building

**Requirements:**


| Tool                              | Notes                                                                        |
| --------------------------------- | ---------------------------------------------------------------------------- |
| `gcc -m32`                        | Multilib — `gcc-multilib` (Debian/Ubuntu)                                    |
| `nasm`                            | Точка входа и обработчики прерываний                                         |
| `ld -m elf_i386`                  | GNU ld                                                                       |
| `cargo +nightly`                  | `rust_mm`, `sched`, `cact_net` (сборка с `-Z build-std`, target `i686-cact`) |
| `python3`                         | Упаковка `cctkfs.img` в LocalRepoCactOS                                      |
| `grub-mkrescue` / `xorriso`       | Сборка `build/cact.iso`                                                      |
| `qemu-img`, `mkfs.ext4`, `e2fsck` | Только для `./build_disk.sh` (образ диска для QEMU)                          |


```sh
# Ядро + ISO (подставляет cctkfs из ../LocalRepoCactOS при наличии)
make -j$(nproc)

# Только планировщик (Rust)
make sched

# Полная зачистка артефактов (включая Rust target)
make clean

# Пустой ext4-диск для ./run_qemu.sh (512 MiB по умолчанию)
./build_disk.sh
```

**Вывод в конце успешной сборки** (версия и хэш берутся из `VERSION` и `git`):

```
--------------------------------------------------
Cact kernel build complete!
  Version: 1.0.0
  Commit:  <short>
  Built:   <timestamp>
  Kernel:  build/kernel.bin
  Image:   build/cact.iso
--------------------------------------------------
```

**Версия в C** (из `Makefile`):

```makefile
VERSION_DEFS = -DCACT_VERSION=$(CACT_VERSION) \
               -DCACT_COMMIT_HASH=$(CACT_COMMIT) \
               -DCACT_BUILD_TIME="$(CACT_BUILD_TIME)"
```

**Линковка:** C-объекты + `libcact_mm.a` (PMM/VMM) + `libsched.a` (MLFQ) + `**libcact_net.a`** (smoltcp, DHCP, DNS, TCP/UDP сокеты).

---

## Structure

```
CactKernel-x86_32/
├── Cact/
│   ├── kernel/
│   │   ├── core/           kernel.c, multiboot2, syscalls/, interrupt, klib
│   │   ├── memory/         rust_mm/ (PMM, VMM, mmap, swap, slab, shm)
│   │   ├── proc/           task.c, task.asm, sched/ (Rust MLFQ)
│   │   ├── sync/           spinlock, semaphore, … (Rust + C glue)
│   │   ├── elf/            elf_loader, dynlink/
│   │   ├── gdt/ idt/
│   │   └── net/            arp, ethernet, ip, icmp, protocols/, socket/, rust_net/ (cact_net)
│   ├── drivers/
│   │   ├── block/          blkdev, pagecache
│   │   ├── input/          ps_2 keyboard + mouse
│   │   ├── network/        virtio_net (встроенный драйвер)
│   │   ├── pci/            enum, driver, loader, gdd, modblob (cctkfs staging)
│   │   ├── usb/            xHCI, HID, hub
│   │   └── video/          framebuffer, font, mtrr
│   ├── fs/
│   │   ├── vfs/            vfs, devfs, procfs, mntfs, etcfs, tmpfs, binfs, sbinfs, libfs, varfs
│   │   ├── ext4/
│   │   └── btrfs/ exFAT/ ramfs/   ← заглушки
│   └── pipe/
├── Makefile
├── VERSION                 # сейчас 1.0.0
├── linker.ld
├── grub.cfg                # multiboot2 + module2 cctkfs
├── grub.cfg.ramroot        # вариант с акцентом на cctkfs / без ext4
├── build_disk.sh           # build/nvme.img (пустой ext4)
└── run_qemu.sh             # QEMU; при отсутствии диска вызывает build_disk.sh
```

---

## Boot sequence

### Ранний `init()` (ещё без планировщика)


| #   | Шаг                        | Примечание                                                    |
| --- | -------------------------- | ------------------------------------------------------------- |
| 1   | **Multiboot2**             | mmap, framebuffer tag                                         |
| 2   | **cctkfs в .bss**          | `pci_modblob_load` — копия модуля GRUB до включения пейджинга |
| 3   | **fb_init**                | без FB — halt                                                 |
| 4   | **kernel_setup_hardware**  | см. ниже                                                      |
| 5   | **create_task(bootstrap)** | поток монтирования и `init`                                   |
| 6   | **sti**                    | boot-контекст становится idle (HLT)                           |


### `kernel_setup_hardware()` (до IRQ глобально у пользователя)


| #   | Компонент                                                    |
| --- | ------------------------------------------------------------ |
| 1   | GDT → PMM (mmap) → VMM → kmalloc heap → **paging on**        |
| 2   | slab, page fault handler                                     |
| 3   | PIC, IDT, **serial COM1**                                    |
| 4   | framebuffer, **MTRR WC**, shadow buffer                      |
| 5   | PS/2 kbd + mouse, опциональные предупреждения I/O / CMOS     |
| 6   | **PIT 100 Hz** (до PCI — GDD/таймер)                         |
| 7   | **blkdev_init** → PCI scan/enumerate → **usb_init (xHCI)**   |
| 8   | page cache, **swap_init**                                    |
| 9   | **vfs_init**, **net_init** (в т.ч. Rust `stack_init`, knetd) |
| 10  | **task_init**, **init_scheduler** (Rust MLFQ)                |


### Поток `kernel_bootstrap_main` (после первого тика планировщика)


| #   | Действие                                                                       |
| --- | ------------------------------------------------------------------------------ |
| 1   | `pci_driver_probe_deferred_all()`                                              |
| 2   | **mntfs_init** — монтирование ext4 (AHCI/NVMe, sema на IRQ)                    |
| 3   | **create_elf_task("bin/init")** — userland из **cctkfs** (overlay binfs/libfs) |


**Пример вывода (сокращённо):**

```
Cact Kernel 1.0.0
--------------------------
[VER] commit=…  built=…
Kernel is ready. Launching init…
```

---

## Memory map (rust_mm)

### Физическая память и куча


| Symbol           | Value                 | Notes                                                |
| ---------------- | --------------------- | ---------------------------------------------------- |
| `MEM_START`      | `0x00100000`          | Нижняя граница для PMM                               |
| `PCI_HOLE_START` | `0xE0000000`          | Верхняя граница RAM для PMM (начало PCI/MMIO hole)   |
| `MEM_SIZE`       | = `PCI_HOLE_START`    | Управляемый диапазон ~3584 MiB (страницы 0 … hole−1) |
| `TOTAL_PAGES`    | `MEM_SIZE / 4096`     |                                                      |
| `RESERVED_END`   | `0x02000000` (32 MiB) | Нижняя зона зарезервирована под ядро/таблицы         |
| `HEAP_START`     | `0x02000000`          |                                                      |
| `HEAP_SIZE`      | 16 MiB                | Окно kmalloc                                         |
| `HEAP_MAGIC`     | `0xDEADBEEF`          |                                                      |
| `SWAP_MAX_SLOTS` | 65536                 |                                                      |
| `SLAB_MIN/MAX`   | 8 … 2048 B            |                                                      |


### Виртуальное адресное пространство пользователя (ориентиры)

```
0xC0000000  ┌──────────────────────────┐  Ядро (ring 0)
0xBF000000  ├──────────────────────────┤  Нижняя граница пользовательского стека
            │       user stack ↓        │
0xBEFFF000  ├──────────────────────────┤  Страница с trampoline sigreturn (`int 0x80`)
0xB0000000  ├──────────────────────────┤  SHM верх (см. SHM_VA_LIMIT)
0xA0000000  ├──────────────────────────┤  SHM база (SHM_VA_BASE)
0x80000000  ├──────────────────────────┤  Верх user heap (USER_HEAP_LIMIT)
0x40000000  ├──────────────────────────┤  mmap / brk регион (MMAP_BASE … MMAP_LIMIT)
0x08048000  ├──────────────────────────┤  Типичный load ELF (PT_LOAD)
0x00000000  └──────────────────────────┘  NULL / guard
```

### Флаги страниц (выдержка из `rust_mm` FFI)


| Flag                        | Значение      | Назначение                             |
| --------------------------- | ------------- | -------------------------------------- |
| `PAGE_PRESENT`              | 0x001         | Страница отображена                    |
| `PAGE_RW`                   | 0x002         | Запись                                 |
| `PAGE_USER`                 | 0x004         | Доступ из ring 3                       |
| `PAGE_PWT` / `PAGE_SWAPPED` | 0x008         | PWT в PTE; при PRESENT=0 — маркер swap |
| `PAGE_PCD`                  | 0x010         | MMIO                                   |
| `PAGE_COW`                  | 0x200         | Copy-on-write                          |
| `PAGE_DEMAND`               | 0x400         | Demand fill                            |
| `PAGE_ZERO`                 | 0x800         | Zero-on-demand                         |
| `PDE_PRIVATE`               | 0x200 (в PDE) | Приватная таблица страниц процесса     |


---

## Scheduler (Rust MLFQ)


| Level | Name        | Quantum | Notes            |
| ----- | ----------- | ------- | ---------------- |
| 0     | Real-Time   | 5 ticks | Высший приоритет |
| 1     | Interactive | 1 tick  | Цель boost       |
| 2     | Normal      | 2 ticks | По умолчанию     |
| 3     | Background  | 4 ticks | CPU-bound        |


- **Boost** каждые 50 тиков: anti-starvation для уровней ≥ Normal  
- **Sleep / alarm**: очередь сна и таймеры на тике  
- **Reentrancy**: защита от вложенного `schedule`

**Состояния задачи:** `TASK_READY`, `TASK_RUNNING`, `TASK_SLEEPING`, `TASK_ZOMBIE`, `TASK_WAITING`

---

## Drivers


| Категория | Компонент                                                                | Примечание                                    |
| --------- | ------------------------------------------------------------------------ | --------------------------------------------- |
| Block     | AHCI, NVMe                                                               | В дереве ядра + опционально `.cctk` из cctkfs |
| Block     | blkdev, page cache                                                       |                                               |
| USB       | xHCI, HID, Hub                                                           |                                               |
| Input     | PS/2 keyboard / mouse                                                    |                                               |
| Video     | FB 32bpp, bitmap font, MTRR WC + shadow                                  |                                               |
| PCI       | Scan, enum, **GDD**, **modblob** (cctkfs), динамическая загрузка `.cctk` |                                               |
| Network   | **virtio-net**                                                           | В типовой QEMU-сборке                         |


Дополнительные PCI-драйверы (Marvell Yukon и др.) собираются в sibling-репозиториях `*-for-Cact` и попадают в `**cctkfs.img`** через `LocalRepoCactOS/build.sh`.

---

## Filesystems


| FS                        | Status | Notes                               |
| ------------------------- | ------ | ----------------------------------- |
| **ext4**                  | Active | R/W, inodes                         |
| **VFS**                   | Active | mount table, symlinks, ELOOP, права |
| **devfs**                 | Active |                                     |
| **procfs**                | Active | `/proc/cmd`, meminfo, …             |
| **mntfs**                 | Active | mount/umount, автомонт при загрузке |
| **etcfs**                 | Active | uid / passwd-подобные данные        |
| **tmpfs**                 | Active | RAM                                 |
| **binfs**                 | Active | `/bin` + overlay из **cctkfs**      |
| **sbinfs**                | Active | `/sbin` + cctkfs                    |
| **libfs**                 | Active | `/lib` + **libc.so** из cctkfs      |
| **varfs**                 | Active | `/var` layout                       |
| **pipes**                 | Active | `pipe`, fd table                    |
| **btrfs / exFAT / ramfs** | Stub   | Заглушки                            |


---

## Network stack

Классический C-путь (Ethernet/IP/ARP) дополняется **Rust `cact_net` (smoltcp)**: DHCPv4, ICMP ping, TCP/UDP для сисколлов, внутренний DNS-клиент.


| Слой                             | Содержание                                                                              |
| -------------------------------- | --------------------------------------------------------------------------------------- |
| **skb**                          | Буферы пакетов                                                                          |
| **Ethernet / ARP / IPv4 / ICMP** | C + вызовы в Rust где нужно                                                             |
| **TCP / UDP / сокеты**           | smoltcp + метаданные `tcp_socket_t` / VFS                                               |
| **knetd**                        | Опрос по семафору → `stack_poll()`                                                      |
| **DHCP**                         | smoltcp `dhcpv4::Socket`                                                                |
| **DNS**                          | `SYS_DNS_RESOLVE` / `dns_resolve()` — A-запись, UDP/53, сервер из DHCP или `netcfg_set` |


Ограничения: нет IPv6/TLS в ядре; в QEMU обычно **virtio-net**; число сокетов ограничено константами (`TCP_MAX_SOCKETS`, `KSOCK_MAX`, и т.д.).

---

## Kernel panic / ring 3 faults

Ring 0: полный дамп регистров и `hlt`.  
Ring 3: часть исключений мапится на сигналы (`SIGFPE`, `SIGSEGV`, остальное → `SIGKILL`) — см. код обработчика.

---

## System calls (95 total)

Источник правды: `Cact/kernel/core/syscalls/syscalls.h` (должен совпадать с `CactLib-x86_32/include/syscall.h`).


| Группа            | Сисколлы                                                                                                                                                                                                                                                     |
| ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **Debug**         | `print`                                                                                                                                                                                                                                                      |
| **Process**       | `getpid` `getppid` `fork` `exec` `exit` `waitpid` `sleep`                                                                                                                                                                                                    |
| **Session**       | `setsid` `setpgid` `getpgid` `getpgrp`                                                                                                                                                                                                                       |
| **Signals**       | `kill` `signal` `sigaction` `sigprocmask` `sigreturn` `sigpending` `sigsuspend` `alarm` `setitimer`                                                                                                                                                          |
| **FD / files**    | `open` `read` `write` `close` `lseek` `ioctl` `fcntl` `dup` `dup2` `pipe` `select` `poll`                                                                                                                                                                    |
| **File metadata** | `stat` `fstat` `access` `chmod` `chown` `umask` `truncate` `ftruncate` `sync` `fsync` `mknod`                                                                                                                                                                |
| **Paths**         | `create` `mkdir` `rmdir` `delete` `unlink` `rename` `link` `symlink` `readlink` `getdents` `chdir` `getcwd` `chroot`                                                                                                                                         |
| **System**        | `mount` `umount` `reboot` `uname`                                                                                                                                                                                                                            |
| **Memory**        | `brk` `mmap` `munmap` `mprotect`                                                                                                                                                                                                                             |
| **SHM**           | `shmget` `shmat` `shmdt` `shmctl`                                                                                                                                                                                                                            |
| **Time**          | `gettimeofday` `clock_gettime` `nanosleep`                                                                                                                                                                                                                   |
| **Users**         | `getuid` `getgid` `geteuid` `getegid` `setuid` `setgid`                                                                                                                                                                                                      |
| **Network**       | `socket` `bind` `connect` `listen` `accept` `send` `recv` `sendto` `recvfrom` `shutdown` `setsockopt` `getsockopt` `select` `poll` и номера `SYS_PING_ECHO` (ICMP echo), `SYS_NETCFG_SET` (IPv4/DHCP lease в стек), `SYS_DNS_RESOLVE` (`dns_resolve` в libc) |
| **Kmod**          | `module_load` `module_unload`                                                                                                                                                                                                                                |


---

## License

**GNU GPL v3.0** — см. `[LICENSE](LICENSE)`.

---

**Developer:** [QwaYer](https://github.com/QwaYer) · **libc:** [CactLib-x86_32](https://github.com/QwaYer/CactLib-x86_32) · **OS / userland:** [CactOS-x86_32](https://github.com/QwaYer/CactOS-x86_32)