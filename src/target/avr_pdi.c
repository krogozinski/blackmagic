#include "general.h"
#include "target_probe.h"
#include "target_internal.h"
#include "avr.h"
#include "exception.h"
#include "gdb_packet.h"

#define IR_PDI		0x7U
#define IR_BYPASS	0xFU

#define PDI_BREAK	0xBBU
#define PDI_DELAY	0xDBU
#define PDI_EMPTY	0xEBU

#define PDI_LDCS	0x80U
#define PDI_STCS	0xC0U

#define PDI_REG_STATUS	0U
#define PDI_REG_RESET	1U
#define PDI_REG_CTRL	2U
#define PDI_REG_R3		3U
#define PDI_REG_R4		4U

#define PDI_RESET	0x59U

static void avr_reset(target *t);
static void avr_halt_request(target *t);
static enum target_halt_reason avr_halt_poll(target *t, target_addr *watch);

bool avr_pdi_init(avr_pdi_t *pdi)
{
	target *t;

	/* Check for a valid part number in the IDCode */
	if ((pdi->idcode & 0x0FFFF000) == 0) {
		DEBUG_WARN("Invalid PDI idcode %08" PRIx32 "\n", pdi->idcode);
		free(pdi);
		return false;
	}
	DEBUG_INFO("AVR ID 0x%08" PRIx32 " (v%d)\n", pdi->idcode,
		(uint8_t)((pdi->idcode >> 28U) & 0xfU));
	jtag_dev_write_ir(&jtag_proc, pdi->dp_jd_index, IR_BYPASS);

	t = target_new();
	if (!t)
		return false;

	t->cpuid = pdi->idcode;
	t->part_id = (pdi->idcode >> 12) & 0xFFFFU;
	t->driver = "Atmel AVR";
	t->core = "AVR";
	t->priv = pdi;
	t->priv_free = free;

	t->attach = avr_attach;
	t->detach = avr_detach;
	t->reset = avr_reset;
	t->halt_request = avr_halt_request;
	t->halt_poll = avr_halt_poll;

	if (atxmega_probe(t))
		return true;
	pdi->halt_reason = TARGET_HALT_RUNNING;
	return true;
}

bool avr_pdi_reg_write(avr_pdi_t *pdi, uint8_t reg, uint8_t value)
{
	uint8_t result = 0, command = PDI_STCS | reg;
	if (reg >= 16 ||
		avr_jtag_shift_dr(&jtag_proc, pdi->dp_jd_index, &result, command) ||
		result != PDI_EMPTY ||
		avr_jtag_shift_dr(&jtag_proc, pdi->dp_jd_index, &result, value))
		return false;
	return result == PDI_EMPTY;
}

uint8_t avr_pdi_reg_read(avr_pdi_t *pdi, uint8_t reg)
{
	uint8_t result = 0, command = PDI_LDCS | reg;
	if (reg >= 16 ||
		avr_jtag_shift_dr(&jtag_proc, pdi->dp_jd_index, &result, command) ||
		result != PDI_EMPTY ||
		!avr_jtag_shift_dr(&jtag_proc, pdi->dp_jd_index, &result, command))
		return 0xFFU; // TODO - figure out a better way to indicate failure.
	return result;
}

void avr_add_flash(target *t, uint32_t start, size_t length)
{
	struct target_flash *f = calloc(1, sizeof(*f));
	if (!f) {			/* calloc failed: heap exhaustion */
		DEBUG_WARN("calloc: failed in %s\n", __func__);
		return;
	}

	f->start = start;
	f->length = length;
	f->blocksize = 0x100;
	f->erased = 0xff;
	target_add_flash(t, f);
}

bool avr_attach(target *t)
{
	avr_pdi_t *pdi = t->priv;
	jtag_dev_write_ir(&jtag_proc, pdi->dp_jd_index, IR_PDI);

	return true;
}

void avr_detach(target *t)
{
	avr_pdi_t *pdi = t->priv;
	jtag_dev_write_ir(&jtag_proc, pdi->dp_jd_index, IR_BYPASS);
}

static void avr_reset(target *t)
{
	avr_pdi_t *pdi = t->priv;
	if (!avr_pdi_reg_write(pdi, PDI_REG_RESET, PDI_RESET) ||
		avr_pdi_reg_read(pdi, PDI_REG_STATUS) != 0x00)
		raise_exception(EXCEPTION_ERROR, "Error resetting device, device in incorrect state\n");
}

static void avr_halt_request(target *t)
{
	avr_pdi_t *pdi = t->priv;
	/* To halt the processor we go through a few really specific steps:
	 * Write r4 to 1 to indicate we want to put the processor into debug-based pause
	 * Read r3 and check it's 0x10 which indicates the processor is held in reset and no debugging is active
	 * Releae reset
	 * Read r3 twice more, the first time should respond 0x14 to indicate the processor is still reset
	 * but that debug pause is requested, and the second should respond 0x04 to indicate the processor is now
	 * in debug pause state (halted)
	 */
	if (!avr_pdi_reg_write(pdi, PDI_REG_R4, 1) ||
		avr_pdi_reg_read(pdi, PDI_REG_R3) != 0x10U ||
		!avr_pdi_reg_write(pdi, PDI_REG_RESET, 0) ||
		avr_pdi_reg_read(pdi, PDI_REG_R3) != 0x14U ||
		avr_pdi_reg_read(pdi, PDI_REG_R3) != 0x04U)
		raise_exception(EXCEPTION_ERROR, "Error halting device, device in incorrect state\n");
	pdi->halt_reason = TARGET_HALT_REQUEST;
}

static enum target_halt_reason avr_halt_poll(target *t, target_addr *watch)
{
	avr_pdi_t *pdi = t->priv;
	(void)watch;
	return pdi->halt_reason;
}
