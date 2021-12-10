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
