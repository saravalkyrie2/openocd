# N32G45x Flash Driver - Setup & Testing Guide

## Prerequisites
- OpenOCD source code (already in workspace)
- ARM toolchain
- J-Link, ST-Link, or compatible debug adapter
- N32G45x evaluation board or custom board

## Building OpenOCD with N32G45x Driver

### 1. Configure OpenOCD
```bash
cd /c/msys64/home/supersarah/openocd2
./configure --prefix=/usr/local --enable-jlink --enable-stlink
```

### 2. Compile
```bash
make -j4
```

### 3. Install (optional)
```bash
make install
```

## Testing the Driver

### 1. Create OpenOCD Config (n32g45x.cfg)
```tcl
# N32G45x Configuration
source [find interface/stlink.cfg]
transport select hla_swd

# Chip configuration
set CHIPNAME n32g45x
set CPUTAPID 0x2ba01477

# SWJ-DP configuration
source [find target/swj-dp.tcl]
swj_newdap $CHIPNAME cpu -irlen 4 -ircapture 0x1 -irmask 0xf

# Target configuration
set _TARGETNAME $_CHIPNAME.cpu
target create $_TARGETNAME cortex_m -chain-position $_TARGETNAME

# Flash configuration
flash bank $_CHIPNAME n32g45x 0x08000000 0x00040000 0 0 $_TARGETNAME

# Halt target on reset
init
```

### 2. Connect and Test in OpenOCD Console
```tcl
# Connect to target
openocd -f n32g45x.cfg

# In OpenOCD console:
> targets
  TargetName         Type       Endian TapName             State
  -- --------------- ---------- ------ ------------------- -------
  0* n32g45x.cpu     cortex_m   little n32g45x.cpu        halted

# Get flash info
> flash info 0
  #0 : n32g45x at 0x08000000, size 0x00040000, buswidth 0, chipwidth 0
  flash banks in bank_list:
  #-1 (n32g45x) at 0x08000000 of 262144 bytes for 0x00000000
  
# Erase a sector
> flash erase_sector 0 0 0

# Write firmware
> flash write_bank 0 firmware.bin 0x00000000

# Verify
> verify_image firmware.bin 0x08000000
```

### 3. GDB Integration
```tcl
# In GDB:
(gdb) target extended-remote :3333
(gdb) monitor reset init
(gdb) file firmware.elf
(gdb) load
(gdb) break main
(gdb) continue
```

## Troubleshooting

### "Unknown flash type" Error
- Verify N32G45x is in Makefile.am
- Rebuild: `make clean && make`
- Check drivers.c has n32g45x_flash reference

### Flash Operations Timeout
- Reduce debug adapter speed: `adapter speed 1000`
- Check target power supply
- Verify JTAG/SWD connection

### Read/Write Errors
- Ensure target is halted before flash operations
- Check flash is not read-protected
- Verify correct flash base address (0x08000000)

### Device Not Recognized
- Run `monitor n32g45x info` (if command exists)
- Check DBGMCU_ID register: `mdw 0xE0042000`
- Expected device ID should start with 0x200

## Configuration Parameters

### Flash Bank Command
```tcl
flash bank <name> n32g45x <base> <size> 0 0 <target>
```
- `base`: 0x08000000 (main flash)
- `size`: Flash size in bytes (e.g., 0x40000 for 256KB)
- Other params should be 0

### Erase Options
```tcl
# Erase specific sector
flash erase_sector 0 <first> <last>

# Erase all
flash erase_sector 0 0 127  # For 256KB with 2KB pages
```

### Write Options
```tcl
# Write binary
flash write_bank 0 <filename.bin> <offset>

# Write with ECC/verification
flash write_bank 0 firmware.bin 0
```

## Key Register Addresses (Reference)

| Register | Address | Purpose |
|----------|---------|---------|
| FLASH_AC | 0x40022000 | Access Control (latency, cache) |
| FLASH_KEY | 0x40022004 | Unlock Key |
| FLASH_OPTKEY | 0x40022008 | Option Key |
| FLASH_SR | 0x4002200C | Status (busy, errors) |
| FLASH_CR | 0x40022010 | Control (PG, PER, MER, START) |
| FLASH_ADD | 0x40022014 | Address for erase |
| FLASH_OBR | 0x4002201C | Option Bytes |
| FLASH_WRP | 0x40022020 | Write Protection |

## Driver Features Implemented

✅ Erase (sector and mass)
✅ Write (word-aligned)
✅ Read (via default handler)
✅ Device probe/detection
✅ Status checking
⚠️ Protection (stub only)

## Debug Output

Enable verbose logging:
```tcl
debug_level 3
```

This will show:
- Register reads/writes
- Flash operations
- Timeout/error conditions
