// SPDX-License-Identifier: GPL-2.0
/*
 * arm_spe_decoder.c: ARM SPE support
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <linux/bitops.h>
#include <linux/compiler.h>
#include <linux/zalloc.h>

#include "../auxtrace.h"
#include "../debug.h"
#include "../util.h"

#include "arm-spe-decoder.h"

static u64 arm_spe_calc_ip(int index, u64 payload)
{
	u64 ns, el, val;

	/* Instruction virtual address or Branch target address */
	if (index == SPE_ADDR_PKT_HDR_INDEX_INS ||
	    index == SPE_ADDR_PKT_HDR_INDEX_BRANCH ||
	    index == SPE_ADDR_PKT_HDR_INDEX_PREV_BRANCH) {
		ns = SPE_ADDR_PKT_GET_NS(payload);
		el = SPE_ADDR_PKT_GET_EL(payload);

		/* Clean highest byte */
		payload = SPE_ADDR_PKT_ADDR_GET_BYTES_0_6(payload);

		/* Fill highest byte for EL1 or EL2 (VHE) mode */
		if (ns && (el == SPE_ADDR_PKT_EL1 || el == SPE_ADDR_PKT_EL2))
			payload |= 0xffULL << SPE_ADDR_PKT_ADDR_BYTE7_SHIFT;

	/* Data access virtual address */
	} else if (index == SPE_ADDR_PKT_HDR_INDEX_DATA_VIRT) {

		/* Clean tags */
		payload = SPE_ADDR_PKT_ADDR_GET_BYTES_0_6(payload);

		/*
		 * Armv8 ARM (ARM DDI 0487F.c), chapter "D10.2.1 Address packet"
		 * defines the data virtual address payload format, the top byte
		 * (bits [63:56]) is assigned as top-byte tag; so we only can
		 * retrieve address value from bits [55:0].
		 *
		 * According to Documentation/arch/arm64/memory.rst, if detects the
		 * specific pattern in bits [55:52] of payload which falls in
		 * the kernel space, should fixup the top byte and this allows
		 * perf tool to parse DSO symbol for data address correctly.
		 *
		 * For this reason, if detects the bits [55:52] is 0xf, will
		 * fill 0xff into the top byte.
		 */
		val = SPE_ADDR_PKT_ADDR_GET_BYTE_6(payload);
		if ((val & 0xf0ULL) == 0xf0ULL)
			payload |= 0xffULL << SPE_ADDR_PKT_ADDR_BYTE7_SHIFT;

	/* Data access physical address */
	} else if (index == SPE_ADDR_PKT_HDR_INDEX_DATA_PHYS) {
		/* Clean highest byte */
		payload = SPE_ADDR_PKT_ADDR_GET_BYTES_0_6(payload);
	} else {
		static u32 seen_idx = 0;
		if (!(seen_idx & BIT(index))) {
			seen_idx |= BIT(index);
			pr_warning("ignoring unsupported address packet index: 0x%x\n", index);
		}
	}

	return payload;
}

struct arm_spe_decoder *arm_spe_decoder_new(struct arm_spe_params *params)
{
	struct arm_spe_decoder *decoder;

	if (!params->get_trace)
		return NULL;

	decoder = zalloc(sizeof(struct arm_spe_decoder));
	if (!decoder)
		return NULL;

	decoder->get_trace = params->get_trace;
	decoder->data = params->data;

	return decoder;
}

void arm_spe_decoder_free(struct arm_spe_decoder *decoder)
{
	free(decoder);
}

static int arm_spe_get_data(struct arm_spe_decoder *decoder)
{
	struct arm_spe_buffer buffer = { .buf = 0, };
	int ret;

	pr_debug("Getting more data\n");
	ret = decoder->get_trace(&buffer, decoder->data);
	if (ret < 0)
		return ret;

	decoder->buf = buffer.buf;
	decoder->len = buffer.len;

	if (!decoder->len)
		pr_debug("No more data\n");

	return decoder->len;
}

static int arm_spe_get_next_packet(struct arm_spe_decoder *decoder)
{
	int ret;

	do {
		if (!decoder->len) {
			ret = arm_spe_get_data(decoder);

			/* Failed to read out trace data */
			if (ret <= 0)
				return ret;
		}

		ret = arm_spe_get_packet(decoder->buf, decoder->len,
					 &decoder->packet);
		if (ret <= 0) {
			/* Move forward for 1 byte */
			decoder->buf += 1;
			decoder->len -= 1;
			return -EBADMSG;
		}

		decoder->buf += ret;
		decoder->len -= ret;
	} while (decoder->packet.type == ARM_SPE_PAD);

	return 1;
}

static int arm_spe_read_record(struct arm_spe_decoder *decoder)
{
	int err;
	int idx;
	u64 payload, ip;

	memset(&decoder->record, 0x0, sizeof(decoder->record));
	decoder->record.context_id = (u64)-1;

	while (1) {
		err = arm_spe_get_next_packet(decoder);
		if (err <= 0)
			return err;

		idx = decoder->packet.index;
		payload = decoder->packet.payload;

		switch (decoder->packet.type) {
		case ARM_SPE_TIMESTAMP:
			decoder->record.timestamp = payload;
			return 1;
		case ARM_SPE_END:
			return 1;
		case ARM_SPE_ADDRESS:
			ip = arm_spe_calc_ip(idx, payload);
			if (idx == SPE_ADDR_PKT_HDR_INDEX_INS)
				decoder->record.from_ip = ip;
			else if (idx == SPE_ADDR_PKT_HDR_INDEX_BRANCH)
				decoder->record.to_ip = ip;
			else if (idx == SPE_ADDR_PKT_HDR_INDEX_DATA_VIRT)
				decoder->record.virt_addr = ip;
			else if (idx == SPE_ADDR_PKT_HDR_INDEX_DATA_PHYS)
				decoder->record.phys_addr = ip;
			else if (idx == SPE_ADDR_PKT_HDR_INDEX_PREV_BRANCH)
				decoder->record.prev_br_tgt = ip;
			break;
		case ARM_SPE_COUNTER:
			if (idx == SPE_CNT_PKT_HDR_INDEX_TOTAL_LAT)
				decoder->record.latency = payload;
			break;
		case ARM_SPE_CONTEXT:
			decoder->record.context_id = payload;
			break;
		case ARM_SPE_OP_TYPE:
			switch (idx) {
			case SPE_OP_PKT_HDR_CLASS_LD_ST_ATOMIC:
				decoder->record.op |= ARM_SPE_OP_LDST;
				if (payload & SPE_OP_PKT_ST)
					decoder->record.op |= ARM_SPE_OP_ST;
				else
					decoder->record.op |= ARM_SPE_OP_LD;
				if (SPE_OP_PKT_IS_LDST_SVE(payload))
					decoder->record.op |= ARM_SPE_OP_SVE_LDST;
				break;
			case SPE_OP_PKT_HDR_CLASS_OTHER:
				decoder->record.op |= ARM_SPE_OP_OTHER;
				if (SPE_OP_PKT_IS_OTHER_SVE_OP(payload))
					decoder->record.op |= ARM_SPE_OP_SVE_OTHER;
				break;
			case SPE_OP_PKT_HDR_CLASS_BR_ERET:
				decoder->record.op |= ARM_SPE_OP_BRANCH_ERET;
				if (payload & SPE_OP_PKT_COND)
					decoder->record.op |= ARM_SPE_OP_BR_COND;
				if (payload & SPE_OP_PKT_INDIRECT_BRANCH)
					decoder->record.op |= ARM_SPE_OP_BR_INDIRECT;
				if (payload & SPE_OP_PKT_GCS)
					decoder->record.op |= ARM_SPE_OP_BR_GCS;
				if (SPE_OP_PKT_CR_BL(payload))
					decoder->record.op |= ARM_SPE_OP_BR_CR_BL;
				if (SPE_OP_PKT_CR_RET(payload))
					decoder->record.op |= ARM_SPE_OP_BR_CR_RET;
				if (SPE_OP_PKT_CR_NON_BL_RET(payload))
					decoder->record.op |= ARM_SPE_OP_BR_CR_NON_BL_RET;
				break;
			default:
				pr_err("Get packet error!\n");
				return -1;
			}
			break;
		case ARM_SPE_EVENTS:
			if (payload & BIT(EV_L1D_REFILL))
				decoder->record.type |= ARM_SPE_L1D_MISS;

			if (payload & BIT(EV_L1D_ACCESS))
				decoder->record.type |= ARM_SPE_L1D_ACCESS;

			if (payload & BIT(EV_TLB_WALK))
				decoder->record.type |= ARM_SPE_TLB_MISS;

			if (payload & BIT(EV_TLB_ACCESS))
				decoder->record.type |= ARM_SPE_TLB_ACCESS;

			if (payload & BIT(EV_LLC_MISS))
				decoder->record.type |= ARM_SPE_LLC_MISS;

			if (payload & BIT(EV_LLC_ACCESS))
				decoder->record.type |= ARM_SPE_LLC_ACCESS;

			if (payload & BIT(EV_REMOTE_ACCESS))
				decoder->record.type |= ARM_SPE_REMOTE_ACCESS;

			if (payload & BIT(EV_MISPRED))
				decoder->record.type |= ARM_SPE_BRANCH_MISS;

			if (payload & BIT(EV_NOT_TAKEN))
				decoder->record.type |= ARM_SPE_BRANCH_NOT_TAKEN;

			if (payload & BIT(EV_TRANSACTIONAL))
				decoder->record.type |= ARM_SPE_IN_TXN;

			if (payload & BIT(EV_PARTIAL_PREDICATE))
				decoder->record.type |= ARM_SPE_SVE_PARTIAL_PRED;

			if (payload & BIT(EV_EMPTY_PREDICATE))
				decoder->record.type |= ARM_SPE_SVE_EMPTY_PRED;

			break;
		case ARM_SPE_DATA_SOURCE:
			decoder->record.source = payload;
			break;
		case ARM_SPE_BAD:
			break;
		case ARM_SPE_PAD:
			break;
		default:
			pr_err("Get packet error!\n");
			return -1;
		}
	}

	return 0;
}

int arm_spe_decode(struct arm_spe_decoder *decoder)
{
	return arm_spe_read_record(decoder);
}
