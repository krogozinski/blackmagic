/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2014 Allen Ibara <aibara>
 * Copyright (C) 2015 Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "general.h"
#include "target.h"
#include "target_internal.h"
#include "cortexm.h"
#include "lpc_common.h"

#define LPC43xx_CHIPID                0x40043200U
#define LPC43xx_CHIPID_FAMILY_MASK    0x0fffffffU
#define LPC43xx_CHIPID_FAMILY_CODE    0x0906002bU
#define LPC43xx_CHIPID_CHIP_MASK      0xf0000000U
#define LPC43xx_CHIPID_CHIP_SHIFT     28U
#define LPC43xx_CHIPID_CORE_TYPE_MASK 0xff0ffff0U
#define LPC43xx_CHIPID_CORE_TYPE_M0   0x4100c200U
#define LPC43xx_CHIPID_CORE_TYPE_M4   0x4100c240U

#define IAP_ENTRYPOINT_LOCATION 0x10400100U

#define LPC43xx_ETBAHB_SRAM_BASE 0x2000c000U
#define LPC43xx_ETBAHB_SRAM_SIZE (16U * 1024U)

#define LPC43xx_CGU_BASE               0x40050000U
#define LPC43xx_CGU_CPU_CLK            (LPC43xx_CGU_BASE + 0x06CU)
#define LPC43xx_CGU_BASE_CLK_AUTOBLOCK (1U << 11U)
#define LPC43xx_CGU_BASE_CLK_SEL_IRC   (1U << 24U)

/* Cortex-M4 Application Interrupt and Reset Control Register */
#define LPC43xx_AIRCR       0xe000ed0cU
/* Magic value reset key */
#define LPC43xx_AIRCR_RESET 0x05fA0004U

#define LPC43xx_WDT_MODE       0x40080000U
#define LPC43xx_WDT_CNT        0x40080004U
#define LPC43xx_WDT_FEED       0x40080008U
#define LPC43xx_WDT_PERIOD_MAX 0xffffffU
#define LPC43xx_WDT_PROTECT    (1U << 4U)

#define IAP_RAM_SIZE LPC43xx_ETBAHB_SRAM_SIZE
#define IAP_RAM_BASE LPC43xx_ETBAHB_SRAM_BASE

#define IAP_PGM_CHUNKSIZE 4096U

#define FLASH_NUM_BANK   2U
#define FLASH_NUM_SECTOR 15U

static bool lpc43xx_cmd_reset(target *t, int argc, const char **argv);
static bool lpc43xx_cmd_mkboot(target *t, int argc, const char **argv);

static bool lpc43xx_flash_init(struct target_flash *flash);
static int lpc43xx_flash_erase(struct target_flash *f, target_addr addr, size_t len);
static bool lpc43xx_mass_erase(target *t);
static void lpc43xx_wdt_set_period(target *t);
static void lpc43xx_wdt_kick(target *t);

const struct command_s lpc43xx_cmd_list[] = {
	{"reset", lpc43xx_cmd_reset, "Reset target"},
	{"mkboot", lpc43xx_cmd_mkboot, "Make flash bank bootable"},
	{NULL, NULL, NULL},
};

static void lpc43xx_add_flash(
	target *t, uint32_t iap_entry, uint8_t bank, uint8_t base_sector, uint32_t addr, size_t len, size_t erasesize)
{
	struct lpc_flash *lf = lpc_add_flash(t, addr, len);
	lf->f.erase = lpc43xx_flash_erase;
	lf->f.blocksize = erasesize;
	lf->f.buf_size = IAP_PGM_CHUNKSIZE;
	lf->bank = bank;
	lf->base_sector = base_sector;
	lf->iap_entry = iap_entry;
	lf->iap_ram = IAP_RAM_BASE;
	lf->iap_msp = IAP_RAM_BASE + IAP_RAM_SIZE;
	lf->wdt_kick = lpc43xx_wdt_kick;
}

static void lpc43xx_detect_flash(target *const t, const uint32_t core_type)
{
	(void)core_type;
	/* LPC4337 */
	const uint32_t iap_entry = target_mem_read32(t, IAP_ENTRYPOINT_LOCATION);
	target_add_ram(t, 0, 0x1A000000);
	lpc43xx_add_flash(t, iap_entry, 0, 0, 0x1A000000, 0x10000, 0x2000);
	lpc43xx_add_flash(t, iap_entry, 0, 8, 0x1A010000, 0x70000, 0x10000);
	target_add_ram(t, 0x1A080000, 0xF80000);
	lpc43xx_add_flash(t, iap_entry, 1, 0, 0x1B000000, 0x10000, 0x2000);
	lpc43xx_add_flash(t, iap_entry, 1, 8, 0x1B010000, 0x70000, 0x10000);
	target_add_commands(t, lpc43xx_cmd_list, "LPC43xx");
	target_add_ram(t, 0x1B080000, 0xE4F80000UL);
	t->target_options |= CORTEXM_TOPT_INHIBIT_NRST;
}

static void lpc43xx_detect_flashless(target *const t, const uint32_t core_type)
{
	(void)t;
	(void)core_type;
}

bool lpc43xx_probe(target *const t)
{
	const uint32_t chipid = target_mem_read32(t, LPC43xx_CHIPID);
	if ((chipid & LPC43xx_CHIPID_FAMILY_MASK) != LPC43xx_CHIPID_FAMILY_CODE)
		return false;

	const uint32_t core_type = t->cpuid & LPC43xx_CHIPID_CORE_TYPE_MASK;
	const uint32_t chip_code = (chipid & LPC43xx_CHIPID_CHIP_MASK) >> LPC43xx_CHIPID_CHIP_SHIFT;

	t->driver = "LPC43xx";
	t->mass_erase = lpc43xx_mass_erase;

	/* 4 is for parts with on-chip Flash, 7 is undocumented but might be for LM43S parts */
	if (chip_code == 4U || chip_code == 7U)
		lpc43xx_detect_flash(t, core_type);
	else if (chip_code == 5U || chip_code == 6U)
		lpc43xx_detect_flashless(t, core_type);
	else
		return false;
	return true;
}

static bool lpc43xx_mass_erase(target *t)
{
	platform_timeout timeout;
	platform_timeout_set(&timeout, 500);
	lpc43xx_flash_init(t->flash);

	for (size_t bank = 0; bank < FLASH_NUM_BANK; ++bank) {
		struct lpc_flash *f = (struct lpc_flash *)t->flash;
		if (lpc_iap_call(f, NULL, IAP_CMD_PREPARE, 0, FLASH_NUM_SECTOR - 1U, bank) ||
			lpc_iap_call(f, NULL, IAP_CMD_ERASE, 0, FLASH_NUM_SECTOR - 1U, CPU_CLK_KHZ, bank))
			return false;
		target_print_progress(&timeout);
	}

	return true;
}

static bool lpc43xx_flash_init(struct target_flash *const flash)
{
	target *const t = flash->t;
	struct lpc_flash *const f = (struct lpc_flash *const)flash;
	/* Deal with WDT */
	lpc43xx_wdt_set_period(t);

	/* Force internal clock */
	target_mem_write32(t, LPC43xx_CGU_CPU_CLK, LPC43xx_CGU_BASE_CLK_AUTOBLOCK | LPC43xx_CGU_BASE_CLK_SEL_IRC);

	/* Initialize flash IAP */
	return lpc_iap_call(f, NULL, IAP_CMD_INIT) == IAP_STATUS_CMD_SUCCESS;
}

static int lpc43xx_flash_erase(struct target_flash *f, target_addr addr, size_t len)
{
	if (!lpc43xx_flash_init(f))
		return -1;
	return lpc_flash_erase(f, addr, len);
}

/* Reset all major systems _except_ debug */
static bool lpc43xx_cmd_reset(target *t, int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	/* System reset on target */
	target_mem_write32(t, LPC43xx_AIRCR, LPC43xx_AIRCR_RESET);
	return true;
}

/*
 * Call Boot ROM code to make a flash bank bootable by computing and writing the
 * correct signature into the exception table near the start of the bank.
 *
 * This is done indepently of writing to give the user a chance to verify flash
 * before changing it.
 */
static bool lpc43xx_cmd_mkboot(target *t, int argc, const char **argv)
{
	/* Usage: mkboot 0 or mkboot 1 */
	if (argc != 2) {
		tc_printf(t, "Expected bank argument 0 or 1.\n");
		return false;
	}

	const uint32_t bank = strtoul(argv[1], NULL, 0);
	if (bank > 1) {
		tc_printf(t, "Unexpected bank number, should be 0 or 1.\n");
		return false;
	}

	lpc43xx_flash_init(t->flash);

	/* special command to compute/write magic vector for signature */
	struct lpc_flash *f = (struct lpc_flash *)t->flash;
	if (lpc_iap_call(f, NULL, IAP_CMD_SET_ACTIVE_BANK, bank, CPU_CLK_KHZ)) {
		tc_printf(t, "Set bootable failed.\n");
		return false;
	}

	tc_printf(t, "Set bootable OK.\n");
	return true;
}

static void lpc43xx_wdt_set_period(target *t)
{
	/* Check if WDT is on */
	uint32_t wdt_mode = target_mem_read32(t, LPC43xx_WDT_MODE);

	/* If WDT on, we can't disable it, but we may be able to set a long period */
	if (wdt_mode && !(wdt_mode & LPC43xx_WDT_PROTECT))
		target_mem_write32(t, LPC43xx_WDT_CNT, LPC43xx_WDT_PERIOD_MAX);
}

static void lpc43xx_wdt_kick(target *t)
{
	/* Check if WDT is on */
	uint32_t wdt_mode = target_mem_read32(t, LPC43xx_WDT_MODE);

	/* If WDT on, kick it so we don't get the target reset */
	if (wdt_mode) {
		target_mem_write32(t, LPC43xx_WDT_FEED, 0xAA);
		target_mem_write32(t, LPC43xx_WDT_FEED, 0xFF);
	}
}
