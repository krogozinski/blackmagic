#include "general.h"
#include "exception.h"
#include "avr.h"
#include "jtag_scan.h"
#include "jtagtap.h"

void avr_jtag_pdi_handler(uint8_t jd_index, uint32_t j_idcode)
{
	avr_pdi_t *pdi = calloc(1, sizeof(*pdi));
	if (!pdi) { /* calloc failed: heap exhaustion */
		DEBUG_WARN("calloc: failed in %s\n", __func__);
		return;
	}
	(void)j_idcode;

	pdi->dp_jd_index = jd_index;
	pdi->idcode = jtag_devs[jd_index].jd_idcode;
	avr_pdi_init(pdi);
}

bool avr_jtag_shift_dr(jtag_proc_t *jp, uint8_t jd_index, uint8_t *dout, const uint8_t din)
{
	jtag_dev_t *d = &jtag_devs[jd_index];
	uint8_t result = 0;
	uint16_t request = 0, response = 0;
	uint8_t *data = (uint8_t *)&request;
	if (!dout)
		return false;
	jtagtap_shift_dr();
	jp->jtagtap_tdi_seq(false, ones, d->dr_prescan);
	data[0] = din;
	// Calculate the parity bit
	for (uint8_t i = 0; i < 8; ++i)
		data[1] ^= (din >> i) & 1U;
	jp->jtagtap_tdi_tdo_seq((uint8_t *)&response, true, (uint8_t *)&request, 9);
	jp->jtagtap_tdi_seq(true, ones, d->dr_postscan);
	jtagtap_return_idle(0);
	data = (uint8_t *)&response;
	// Calculate the parity bit
	for (uint8_t i = 0; i < 8; ++i)
		result ^= (data[0] >> i) & 1U;
	*dout = data[0];
	return result == data[1];
}
