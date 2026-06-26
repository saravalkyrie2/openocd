// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Copyright (C) 2019 Nations Technologies Inc.                          *
 *   Based on stm32f2x driver by Dominic Rath, Spencer Oliver              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/cortex_m.h>

/* N32G45x Flash Controller Register Addresses */
#define N32_FLASH_BASE      0x40022000
#define N32_FLASH_AC        0x40022000  /* Access Control */
#define N32_FLASH_KEY       0x40022004  /* Key Register */
#define N32_FLASH_OPTKEY    0x40022008  /* Option Key Register */
#define N32_FLASH_SR        0x4002200C  /* Status Register */
#define N32_FLASH_CR        0x40022010  /* Control Register */
#define N32_FLASH_ADD       0x40022014  /* Address Register */
#define N32_FLASH_OBR       0x4002201C  /* Option Bytes Register */
#define N32_FLASH_WRP       0x40022020  /* Write Protection Register */

/* N32G45x Flash page size */
#define N32_FLASH_PAGE_SIZE 2048

/* Flash keys */
#define FLASH_KEY1          0x45670123
#define FLASH_KEY2          0xCDEF89AB

/* Flash Control Register bits */
#define FLASH_CR_PG         (1 << 0)    /* Programming */
#define FLASH_CR_PER        (1 << 1)    /* Page Erase */
#define FLASH_CR_MER        (1 << 2)    /* Mass Erase */
#define FLASH_CR_OPTPG      (1 << 4)    /* Option Byte Programming */
#define FLASH_CR_OPTER      (1 << 5)    /* Option Byte Erase */
#define FLASH_CR_START      (1 << 6)    /* Start */
#define FLASH_CR_LOCK       (1 << 7)    /* Lock */
#define FLASH_CR_SMPSEL     (1 << 8)    /* Sampling Selection */
#define FLASH_CR_OPTWRE     (1 << 9)    /* Option Write Enable */
#define FLASH_CR_ERRITE     (1 << 10)   /* Error Interrupt Enable */
#define FLASH_CR_FERRITE    (1 << 11)   /* EVERR PVERR Error Interrupt Enable */
#define FLASH_CR_EOPITE     (1 << 12)   /* End of Operation Interrupt Enable */

/* Flash Status Register bits */
#define FLASH_SR_BUSY       (1 << 0)    /* Busy */
#define FLASH_SR_PGERR      (1 << 2)    /* Programming Error */
#define FLASH_SR_PVERR      (1 << 3)    /* Programming Verify Error */
#define FLASH_SR_WRPERR     (1 << 4)    /* Write Protection Error */
#define FLASH_SR_EOP        (1 << 5)    /* End of Operation */
#define FLASH_SR_EVERR      (1 << 6)    /* Erase Verify Error */

#define FLASH_SR_ERROR (FLASH_SR_PGERR | FLASH_SR_PVERR | FLASH_SR_WRPERR | FLASH_SR_EVERR)

/* Timeouts */
#define FLASH_ERASE_TIMEOUT  10000
#define FLASH_WRITE_TIMEOUT  1000

struct n32g45x_flash_bank {
	bool probed;
	uint32_t user_bank_size;
};

static int n32g45x_unlock_reg(struct target *target)
{
	uint32_t ctrl;
	int retval;

	retval = target_read_u32(target, N32_FLASH_CR, &ctrl);
	if (retval != ERROR_OK)
		return retval;

	if ((ctrl & FLASH_CR_LOCK) == 0)
		return ERROR_OK;

	/* Unlock flash registers */
	retval = target_write_u32(target, N32_FLASH_KEY, FLASH_KEY1);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, N32_FLASH_KEY, FLASH_KEY2);
	if (retval != ERROR_OK)
		return retval;

	retval = target_read_u32(target, N32_FLASH_CR, &ctrl);
	if (retval != ERROR_OK)
		return retval;

	if (ctrl & FLASH_CR_LOCK) {
		LOG_ERROR("Flash unlock failed");
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int n32g45x_wait_status_busy(struct flash_bank *bank, int timeout)
{
	struct target *target = bank->target;
	uint32_t status;
	int retval = ERROR_OK;

	/* Wait for busy to clear */
	for (;;) {
		retval = target_read_u32(target, N32_FLASH_SR, &status);
		if (retval != ERROR_OK) {
			LOG_ERROR("Error reading flash status");
			return retval;
		}

		if ((status & FLASH_SR_BUSY) == 0)
			break;

		if (timeout-- <= 0) {
			LOG_ERROR("Timed out waiting for flash");
			return ERROR_FAIL;
		}

		alive_sleep(1);
	}

	/* Check for errors */
	if (status & FLASH_SR_WRPERR) {
		LOG_ERROR("Flash write protection error");
		retval = ERROR_FAIL;
	}

	if (status & FLASH_SR_ERROR) {
		LOG_ERROR("Flash error status = 0x%x", status);
		retval = ERROR_FAIL;
	}

	/* Clear but report errors */
	if (status & FLASH_SR_ERROR) {
		target_write_u32(target, N32_FLASH_SR, FLASH_SR_ERROR);
	}

	return retval;
}

static int n32g45x_erase(struct flash_bank *bank, unsigned int first,
		unsigned int last)
{
	struct target *target = bank->target;
	uint32_t addr;
	unsigned int i;
	int retval;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if ((first == 0) && (last == (bank->num_sectors - 1))) {
		/* Mass erase */
		retval = n32g45x_unlock_reg(target);
		if (retval != ERROR_OK)
			return retval;

		retval = target_write_u32(target, N32_FLASH_CR, FLASH_CR_MER);
		if (retval != ERROR_OK)
			return retval;

		retval = target_write_u32(target, N32_FLASH_CR, FLASH_CR_MER | FLASH_CR_START);
		if (retval != ERROR_OK)
			return retval;

		retval = n32g45x_wait_status_busy(bank, FLASH_ERASE_TIMEOUT);
		if (retval != ERROR_OK)
			return retval;

		retval = target_write_u32(target, N32_FLASH_CR, FLASH_CR_LOCK);
		if (retval != ERROR_OK)
			return retval;
	} else {
		/* Page erase */
		retval = n32g45x_unlock_reg(target);
		if (retval != ERROR_OK)
			return retval;

		for (i = first; i <= last; i++) {
			addr = bank->base + (i * N32_FLASH_PAGE_SIZE);

			retval = target_write_u32(target, N32_FLASH_CR, FLASH_CR_PER);
			if (retval != ERROR_OK)
				return retval;

			retval = target_write_u32(target, N32_FLASH_ADD, addr);
			if (retval != ERROR_OK)
				return retval;

			retval = target_write_u32(target, N32_FLASH_CR, FLASH_CR_PER | FLASH_CR_START);
			if (retval != ERROR_OK)
				return retval;

			retval = n32g45x_wait_status_busy(bank, FLASH_ERASE_TIMEOUT);
			if (retval != ERROR_OK)
				return retval;
		}

		retval = target_write_u32(target, N32_FLASH_CR, FLASH_CR_LOCK);
		if (retval != ERROR_OK)
			return retval;
	}

	return ERROR_OK;
}

static int n32g45x_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	uint32_t address;
	uint32_t written;
	uint32_t data;
	int retval;

	address = bank->base + offset;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset & 0x3) {
		LOG_ERROR("Unaligned write offset");
		return ERROR_FAIL;
	}

	if (count & 0x3) {
		LOG_ERROR("Unaligned write size");
		return ERROR_FAIL;
	}

	retval = n32g45x_unlock_reg(target);
	if (retval != ERROR_OK)
		return retval;

	/* Write words */
	for (written = 0; written < count; written += 4) {
		data = buf_get_u32(buffer + written, 0, 32);

		/* Enable programming */
		retval = target_write_u32(target, N32_FLASH_CR, FLASH_CR_PG);
		if (retval != ERROR_OK)
			return retval;

		/* Write the word */
		retval = target_write_u32(target, address + written, data);
		if (retval != ERROR_OK)
			return retval;

		/* Wait for operation to complete */
		retval = n32g45x_wait_status_busy(bank, FLASH_WRITE_TIMEOUT);
		if (retval != ERROR_OK)
			return retval;

		/* Clear PG bit to end programming */
		retval = target_write_u32(target, N32_FLASH_CR, 0);
		if (retval != ERROR_OK)
			return retval;

		/* Clear EOP status bit for next operation */
		retval = target_write_u32(target, N32_FLASH_SR, FLASH_SR_EOP);
		if (retval != ERROR_OK)
			return retval;
	}

	/* Lock flash */
	retval = target_write_u32(target, N32_FLASH_CR, FLASH_CR_LOCK);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static int n32g45x_protect_check(struct flash_bank *bank)
{
	unsigned int i;

	for (i = 0; i < bank->num_sectors; i++)
		bank->sectors[i].is_protected = 0;

	return ERROR_OK;
}

static int n32g45x_protect(struct flash_bank *bank, int set,
		unsigned int first, unsigned int last)
{
	LOG_WARNING("N32G45x flash protection not implemented");
	return ERROR_OK;
}

static int n32g45x_get_info(struct flash_bank *bank, struct command_invocation *cmd)
{
	command_print(cmd, "N32G45x flash driver");
	return ERROR_OK;
}

static int n32g45x_blank_check(struct flash_bank *bank)
{
	struct target *target = bank->target;
	int retval;
	unsigned int i;
	uint32_t address;
	uint32_t data;

	for (i = 0; i < bank->num_sectors; i++) {
		address = bank->base + (i * N32_FLASH_PAGE_SIZE);

		retval = target_read_u32(target, address, &data);
		if (retval != ERROR_OK)
			return retval;

		if (data != 0xFFFFFFFF) {
			bank->sectors[i].is_erased = 0;
		} else {
			bank->sectors[i].is_erased = 1;
		}
	}

	return ERROR_OK;
}

static int n32g45x_probe(struct flash_bank *bank)
{
	struct n32g45x_flash_bank *n32g45x_info;
	struct target *target = bank->target;
	uint32_t device_id;
	uint16_t flash_size_kb;
	uint32_t num_sectors;
	unsigned int i;
	int retval;

	n32g45x_info = bank->driver_priv;

	if (n32g45x_info->probed)
		return ERROR_OK;

	/* Read device ID */
	retval = target_read_u32(target, 0xE0042000, &device_id);
	if (retval != ERROR_OK) {
		LOG_ERROR("Error reading device ID");
		return retval;
	}

	LOG_INFO("N32G45x device ID: 0x%08" PRIx32, device_id);

	/* N32G45x defaults: 256KB flash with 2KB pages = 128 sectors */
	flash_size_kb = 256;

	/* Try to read flash size from info */
	retval = target_read_u16(target, 0x1FFFF7E0, &flash_size_kb);
	if (retval != ERROR_OK) {
		LOG_WARNING("Could not read flash size, assuming 256KB");
		flash_size_kb = 256;
	}

	if (n32g45x_info->user_bank_size) {
		flash_size_kb = n32g45x_info->user_bank_size / 1024;
	}

	LOG_INFO("Flash size = %" PRIu16 " KiB", flash_size_kb);

	/* Setup sectors */
	num_sectors = (flash_size_kb * 1024) / N32_FLASH_PAGE_SIZE;
	bank->base = 0x08000000;
	bank->size = flash_size_kb * 1024;
	bank->num_sectors = num_sectors;
	bank->sectors = malloc(sizeof(struct flash_sector) * num_sectors);

	for (i = 0; i < num_sectors; i++) {
		bank->sectors[i].offset = i * N32_FLASH_PAGE_SIZE;
		bank->sectors[i].size = N32_FLASH_PAGE_SIZE;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = 0;
	}

	n32g45x_info->probed = true;

	return ERROR_OK;
}

static int n32g45x_auto_probe(struct flash_bank *bank)
{
	struct n32g45x_flash_bank *n32g45x_info = bank->driver_priv;

	if (n32g45x_info->probed)
		return ERROR_OK;

	return n32g45x_probe(bank);
}

FLASH_BANK_COMMAND_HANDLER(n32g45x_flash_bank_command)
{
	struct n32g45x_flash_bank *n32g45x_info;

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	n32g45x_info = malloc(sizeof(struct n32g45x_flash_bank));
	if (!n32g45x_info) {
		LOG_ERROR("No memory for flash bank info");
		return ERROR_FAIL;
	}

	bank->driver_priv = n32g45x_info;
	n32g45x_info->probed = false;
	n32g45x_info->user_bank_size = bank->size;

	return ERROR_OK;
}

static const struct command_registration n32g45x_exec_command_handlers[] = {
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration n32g45x_command_handlers[] = {
	{
		.name = "n32g45x",
		.mode = COMMAND_ANY,
		.help = "n32g45x flash command group",
		.usage = "",
		.chain = n32g45x_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

const struct flash_driver n32g45x_flash = {
	.name = "n32g45x",
	.commands = n32g45x_command_handlers,
	.flash_bank_command = n32g45x_flash_bank_command,
	.erase = n32g45x_erase,
	.protect = n32g45x_protect,
	.write = n32g45x_write,
	.read = default_flash_read,
	.probe = n32g45x_probe,
	.auto_probe = n32g45x_auto_probe,
	.erase_check = n32g45x_blank_check,
	.protect_check = n32g45x_protect_check,
	.info = n32g45x_get_info,
};
