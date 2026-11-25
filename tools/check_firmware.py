"""检查链接后的 RAM 预算、关键 RF/中断入口位置与启动 CSR 权限。"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--prefix", default="riscv-none-embed-")
    parser.add_argument("--role", choices=("sensor", "gateway"))
    args = parser.parse_args()
    readelf = args.toolchain / "bin" / f"{args.prefix}readelf"
    output = subprocess.check_output([str(readelf), "-sW", str(args.elf)], text=True)
    symbols = {}
    sizes = {}
    for line in output.splitlines():
        columns = line.split()
        if (len(columns) == 8 and columns[0].rstrip(":").isdigit()
                and columns[6] != "UND"):
            symbols[columns[7]] = int(columns[1], 16)
            sizes[columns[7]] = int(columns[2])
    ram_start, ram_end = 0x20000000, 0x20006800
    role = args.role or ("gateway" if "cdc_ep1" in symbols else "sensor")
    active, inactive = ("Tx", "Rx") if role == "sensor" else ("Rx", "Tx")
    irq = "GPIOA_IRQHandler" if role == "sensor" else "USB_IRQHandler"
    required = [f"RF_{active}", f"rf_{active.lower()}_auto_txProcess",
                f"rf_{active.lower()}_auto_rxProcess", irq, "RF_Shut", "tmos_set_event",
                "TMOS_SysRegister", "BLE_SetPHYRxMode", "BLE_SetPHYTxMode",
                "ble_ll_hw_api_shut", "ll_multi_states_process", "tmos_proces_idle"]
    required += [name for name in symbols if name.startswith(
        ("radio_exchange", "radio_wait", "radio_callback", "sensor_sleep"))]
    errors = [f"{name} missing or not in RAM: {symbols.get(name, 0):#x}"
              for name in required if not ram_start <= symbols.get(name, 0) < ram_end]
    if symbols.get(irq) == symbols.get("NMI_Handler"):
        errors.append(f"{irq} aliases default interrupt handler")
    excluded = (f"RF_{inactive}", f"rf_{inactive.lower()}_auto_")
    if role == "sensor":
        excluded += ("LowPower_Idle", "LowPower_Halt", "LowPower_Sleep", "LowPower_Shutdown")
    errors += [f"unused {name} in RAM" for name, address in symbols.items()
               if name.startswith(excluded) and ram_start <= address < ram_end]
    if sizes.get("MEM_BUF") != 6144:
        errors.append(f"BLE heap must be 6144 B, got {sizes.get('MEM_BUF', 0)} B")
    stack = symbols["_eusrstack"] - symbols["_susrstack"]
    for name in ("__wrap_main", "main", "app_stack", "app_stack_measure"):
        if name not in symbols:
            errors.append(f"missing stack instrumentation: {name}")
    if "__wrap_main" in symbols:
        # 厂家启动代码以 MPP=0 执行 mret，包装入口已在用户态。
        objdump = args.toolchain / "bin" / f"{args.prefix}objdump"
        entry = subprocess.check_output(
            [str(objdump), "-d", "--disassemble=__wrap_main", str(args.elf)], text=True)
        for encoded in re.findall(r"^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s", entry, re.M):
            instruction = int(encoded, 16)
            if instruction & 0x7f == 0x73 and (instruction >> 12) & 7:
                csr = instruction >> 20
                if (csr >> 8) & 3:
                    errors.append(f"user-mode stack entry accesses privileged CSR {csr:#05x}")
    spare = symbols["_susrstack"] - symbols["_ebss"]
    if stack != 2048 or spare < (256 if role == "sensor" else 0):
        errors.append(f"insufficient RAM: stack={stack}, unused={spare}")
    if errors:
        raise SystemExit("\n".join(errors))
    used = symbols["_ebss"] - ram_start
    print(f"RAM static/highcode={used} B; reserved stack={stack} B; unused={spare} B")
    print(f"RF/interrupt RAM placement: {len(required)} symbols checked")
    print("Stack entry user-mode CSR access: checked")


if __name__ == "__main__":
    main()
