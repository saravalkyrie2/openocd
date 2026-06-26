# N32G45x OpenOCD Flash Driver

## Summary
Created a working OpenOCD flash driver for Nations N32G45x microcontroller that uses the correct memory/flash register addresses. The STM32F2x driver was failing because the flash controller register addresses are different.

## Key Differences from STM32F2x

### Flash Register Base Address
- **STM32F2x**: 0x40023c00
- **N32G45x**: 0x40022000

### Flash Control Registers
- FLASH_AC (Access Control): 0x40022000
- FLASH_KEY (Unlock Key): 0x40022004
- FLASH_OPTKEY (Option Key): 0x40022008
- FLASH_SR (Status): 0x4002200C
- FLASH_CR (Control): 0x40022010
- FLASH_ADD (Address): 0x40022014
- FLASH_OBR (Option Bytes): 0x4002201C
- FLASH_WRP (Write Protection): 0x40022020

### Flash Memory Layout
- **Page Size**: 2048 bytes (2KB)
- **Flash Base Address**: 0x08000000
- **Typical Size**: 256KB (128 sectors)

### Control Register Bits (CR)
| Bit | Name | Function |
|-----|------|----------|
| 0 | PG | Programming |
| 1 | PER | Page Erase |
| 2 | MER | Mass Erase |
| 4 | OPTPG | Option Byte Programming |
| 5 | OPTER | Option Byte Erase |
| 6 | START | Start operation |
| 7 | LOCK | Lock flash |
| 8 | SMPSEL | Sampling Selection |

### Status Register Bits (SR)
| Bit | Name | Function |
|-----|------|----------|
| 0 | BUSY | Busy flag |
| 2 | PGERR | Programming Error |
| 3 | PVERR | Programming Verify Error |
| 4 | WRPERR | Write Protection Error |
| 5 | EOP | End of Operation |
| 6 | EVERR | Erase Verify Error |

## Driver Implementation

### Files Modified
1. **src/flash/nor/n32g45x.c** - New driver implementation
2. **src/flash/nor/Makefile.am** - Added to NOR_DRIVERS list
3. **src/flash/nor/drivers.c** - Added to flash_drivers array

### Supported Operations
- ✓ Flash reading (via default handler)
- ✓ Flash writing (word-aligned)
- ✓ Sector erase
- ✓ Mass erase
- ✓ Device probing/detection
- ✓ Erase verification

### Key Functions
- `n32g45x_unlock_reg()` - Unlock flash controller with keys
- `n32g45x_wait_status_busy()` - Wait for flash operation completion
- `n32g45x_erase()` - Erase sectors or mass erase
- `n32g45x_write()` - Write data to flash
- `n32g45x_probe()` - Probe and identify device

## Configuration Example

In OpenOCD config file (e.g., n32g45x.cfg):
```tcl
# Define the chip
set CHIPNAME n32g45x
set CPUTAPID 0x2ba01477

# Add debug port
adapter speed 1000
source [find interface/stlink.cfg]

# Configure target
transport select hla_swd
set WORKAREASIZE 0x4000
source [find target/swj-dp.tcl]

swj_newdap $CHIPNAME cpu -irlen 4 -ircapture 0x1 -irmask 0xf

set _TARGETNAME $_CHIPNAME.cpu
target create $_TARGETNAME cortex_m -chain-position $_TARGETNAME

# Configure flash
flash bank $_CHIPNAME n32g45x 0x08000000 0 0 0 $_TARGETNAME
```

## Unlock Keys
- KEY1: 0x45670123
- KEY2: 0xCDEF89AB

These are used to unlock the flash controller for programming/erasing operations.

## Tested Features
- Device identification
- Sector erase operations
- Mass erase
- Flash programming
- Status checking

## Known Limitations
- Write protection control not fully implemented (returns OK without action)
- Only supports basic flash operations
- No OTP (One-Time Programmable) support yet
- No advanced security features

## References
- N32G45x Library Reference Manual v1.0
- Nations Technologies flash controller documentation
