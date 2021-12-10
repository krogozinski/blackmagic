#ifndef ATMEL___H
#define ATMEL___H

#include "jtag_scan.h"

typedef struct Atmel_DP_s {
	int refcnt;

	uint32_t idcode;

	uint8_t dp_jd_index;
} avr_pdi_t;

void avr_pdi_init(avr_pdi_t *pdi);

void avr_jtag_pdi_handler(uint8_t jd_index, uint32_t j_idcode);
int platform_avr_jtag_pdi_init(avr_pdi_t *pdi);

#endif /*ATMEL___H*/
