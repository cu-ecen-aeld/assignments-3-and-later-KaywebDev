# Faulty Driver Kernel Oops

## Reproduction

In the running QEMU image, write the requested string to the faulty character device:

```sh
echo "hello_world" > /dev/faulty
```

The command triggers a kernel Oops in the driver's write handler. The Oops terminates the writing process; it does not panic the kernel, so the system remains available for collecting the log with `dmesg`.

## Captured Oops

```text
Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
Mem abort info:
  ESR = 0x0000000096000045
  EC = 0x25: DABT (current EL), IL = 32 bits
  SET = 0, FnV = 0
  EA = 0, S1PTW = 0
  FSC = 0x05: level 1 translation fault
Data abort info:
  ISV = 0, ISS = 0x00000045
  CM = 0, WnR = 1
user pgtable: 4k pages, 39-bit VAs, pgdp=0000000041bf4000
[0000000000000000] pgd=0000000000000000, p4d=0000000000000000, pud=0000000000000000
Internal error: Oops: 0000000096000045 [#1] SMP
Modules linked in: hello(O) faulty(O) scull(O)
CPU: 0 PID: 138 Comm: sh Tainted: G           O       6.1.44 #1
Hardware name: linux,dummy-virt (DT)
pstate: 80000005 (Nzcv daif -PAN -UAO -TCO -DIT -SSBS BTYPE=--)
pc : faulty_write+0x10/0x20 [faulty]
lr : vfs_write+0xc8/0x390
sp : ffffffc008de3d20
x29: ffffffc008de3d80 x28: ffffff8001aa6a00 x27: 0000000000000000
x26: 0000000000000000 x25: 0000000000000000 x24: 0000000000000000
x23: 000000000000000c x22: 000000000000000c x21: ffffffc008de3dc0
x20: 000000558314e530 x19: ffffff8001aede00 x18: 0000000000000000
x17: 0000000000000000 x16: 0000000000000000 x15: 0000000000000000
x14: 0000000000000000 x13: 0000000000000000 x12: 0000000000000000
x11: 0000000000000000 x10: 0000000000000000 x9 : 0000000000000000
x8 : 0000000000000000 x7 : 0000000000000000 x6 : 0000000000000000
x5 : 0000000000000001 x4 : ffffffc000787000 x3 : ffffffc008de3dc0
x2 : 000000000000000c x1 : 0000000000000000 x0 : 0000000000000000
Call trace:
 faulty_write+0x10/0x20 [faulty]
 ksys_write+0x74/0x110
 __arm64_sys_write+0x1c/0x30
 invoke_syscall+0x54/0x130
 el0_svc_common.constprop.0+0x44/0xf0
 do_el0_svc+0x2c/0xc0
 el0_svc+0x2c/0x90
 el0t_64_sync_handler+0xf4/0x120
 el0t_64_sync+0x18c/0x190
Code: d2800001 d2800000 d503233f d50323bf (b900003f)
---[ end trace 0000000000000000 ]---
```

## Analysis

The Oops is an ARM64 data abort caused by a write to virtual address zero. `WnR = 1` identifies the fault as a write, and `FSC = 0x05` reports a level-1 translation fault: address zero has no valid mapping in this process's page tables. The program counter is inside `faulty_write`, confirming that the driver write callback caused the fault.

The `faulty_write` implementation deliberately dereferences a null pointer:

```c
*(int *)0 = 0;
```

Writing `hello_world` invokes that callback through the character-device file operations. The kernel attempts to store zero through the null pointer, faults at address zero, records the Oops, and terminates the calling `sh` process. The call trace runs from `faulty_write` through `ksys_write` and the ARM64 system-call entry path. `Tainted: G           O` indicates that an out-of-tree module was loaded; the listed modules identify `faulty` as loaded when the fault occurred.
