// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2018-2019 The Linux Foundation. All rights reserved.
 */

#include <linux/kfifo.h>
#include "core.h"
#include "dp_tx.h"
#include "hal_tx.h"
#include "debug.h"
#include "dp_rx.h"
#include "peer.h"

static void ath11k_dp_htt_htc_tx_complete(struct ath11k_base *ab,
					  struct sk_buff *skb)
{
	dev_kfree_skb_any(skb);
}

void ath11k_dp_peer_cleanup(struct ath11k *ar, int vdev_id, const u8 *addr)
{
	struct ath11k_base *ab = ar->ab;
	struct ath11k_peer *peer;

	/* TODO: Any other peer specific DP cleanup */

	spin_lock_bh(&ab->base_lock);
	peer = ath11k_peer_find(ab, vdev_id, addr);
	if (!peer) {
		ath11k_warn(ab, "failed to lookup peer %pM on vdev %d\n",
			    addr, vdev_id);
		spin_unlock_bh(&ab->base_lock);
		return;
	}

	ath11k_peer_rx_tid_cleanup(ar, peer);
	spin_unlock_bh(&ab->base_lock);
}

int ath11k_dp_peer_setup(struct ath11k *ar, int vdev_id, const u8 *addr)
{
	struct ath11k_base *ab = ar->ab;
	u32 reo_dest;
	int ret;

	/* NOTE: reo_dest ring id starts from 1 unlike mac_id which starts from 0 */
	reo_dest = ar->dp.mac_id + 1;
	ret = ath11k_wmi_set_peer_param(ar, addr, vdev_id,
					WMI_PEER_SET_DEFAULT_ROUTING,
					DP_RX_HASH_ENABLE | (reo_dest << 1));

	if (ret) {
		ath11k_warn(ab, "failed to set default routing %d peer :%pM vdev_id :%d\n",
			    ret, addr, vdev_id);
		return ret;
	}

	ret = ath11k_peer_rx_tid_setup(ar, addr, vdev_id,
				       HAL_DESC_REO_NON_QOS_TID, 1, 0);
	if (ret) {
		ath11k_warn(ab, "failed to setup rxd tid queue for non-qos tid %d\n",
			    ret);
		return ret;
	}

	ret = ath11k_peer_rx_tid_setup(ar, addr, vdev_id, 0, 1, 0);
	if (ret) {
		ath11k_warn(ab, "failed to setup rxd tid queue for tid 0 %d\n",
			    ret);
		return ret;
	}

	/* TODO: Setup other peer specific resource used in data path */

	return 0;
}

void ath11k_dp_srng_cleanup(struct ath11k_base *ab, struct dp_srng *ring)
{
	if (!ring->vaddr_unaligned)
		return;

	dma_free_coherent(ab->dev, ring->size, ring->vaddr_unaligned,
			  ring->paddr_unaligned);

	ring->vaddr_unaligned = NULL;
}

int ath11k_dp_srng_setup(struct ath11k_base *ab, struct dp_srng *ring,
			 enum hal_ring_type type, int ring_num,
			 int mac_id, int num_entries)
{
	struct hal_srng_params params = { 0 };
	int entry_sz = ath11k_hal_srng_get_entrysize(type);
	int max_entries = ath11k_hal_srng_get_max_entries(type);
	int ret;

	if (max_entries < 0 || entry_sz < 0)
		return -EINVAL;

	if (num_entries > max_entries)
		num_entries = max_entries;

	ring->size = (num_entries * entry_sz) + HAL_RING_BASE_ALIGN - 1;
	ring->vaddr_unaligned = dma_alloc_coherent(ab->dev, ring->size,
						   &ring->paddr_unaligned,
						   GFP_KERNEL);
	if (!ring->vaddr_unaligned)
		return -ENOMEM;

	ring->vaddr = PTR_ALIGN(ring->vaddr_unaligned, HAL_RING_BASE_ALIGN);
	ring->paddr = ring->paddr_unaligned + ((unsigned long)ring->vaddr -
		      (unsigned long)ring->vaddr_unaligned);

	params.ring_base_vaddr = ring->vaddr;
	params.ring_base_paddr = ring->paddr;
	params.num_entries = num_entries;

	switch (type) {
	case HAL_REO_DST:
		params.intr_batch_cntr_thres_entries =
					HAL_SRNG_INT_BATCH_THRESHOLD_RX;
		params.intr_timer_thres_us = HAL_SRNG_INT_TIMER_THRESHOLD_RX;
		break;
	case HAL_RXDMA_BUF:
	case HAL_RXDMA_MONITOR_BUF:
	case HAL_RXDMA_MONITOR_STATUS:
		params.low_threshold = num_entries >> 3;
		params.flags |= HAL_SRNG_FLAGS_LOW_THRESH_INTR_EN;
		params.intr_batch_cntr_thres_entries = 0;
		params.intr_timer_thres_us = HAL_SRNG_INT_TIMER_THRESHOLD_RX;
		break;
	case HAL_WBM2SW_RELEASE:
		if (ring_num < 3) {
			params.intr_batch_cntr_thres_entries =
					HAL_SRNG_INT_BATCH_THRESHOLD_TX;
			params.intr_timer_thres_us =
					HAL_SRNG_INT_TIMER_THRESHOLD_TX;
			break;
		}
		/* follow through when ring_num >= 3 */
		/* fall through */
	case HAL_REO_EXCEPTION:
	case HAL_REO_REINJECT:
	case HAL_REO_CMD:
	case HAL_REO_STATUS:
	case HAL_TCL_DATA:
	case HAL_TCL_CMD:
	case HAL_TCL_STATUS:
	case HAL_WBM_IDLE_LINK:
	case HAL_SW2WBM_RELEASE:
	case HAL_RXDMA_DST:
	case HAL_RXDMA_MONITOR_DST:
	case HAL_RXDMA_MONITOR_DESC:
	case HAL_RXDMA_DIR_BUF:
		params.intr_batch_cntr_thres_entries =
					HAL_SRNG_INT_BATCH_THRESHOLD_OTHER;
		params.intr_timer_thres_us = HAL_SRNG_INT_TIMER_THRESHOLD_OTHER;
		break;
	default:
		ath11k_warn(ab, "Not a valid ring type in dp :%d\n", type);
		return -EINVAL;
	}

	ret = ath11k_hal_srng_setup(ab, type, ring_num, mac_id, &params);
	if (ret < 0) {
		ath11k_warn(ab, "failed to setup srng: %d ring_id %d\n",
			    ret, ring_num);
		return ret;
	}

	ring->ring_id = ret;

	return 0;
}

static void ath11k_dp_srng_common_cleanup(struct ath11k_base *ab)
{
	struct ath11k_dp *dp = &ab->dp;
	int i;

	ath11k_dp_srng_cleanup(ab, &dp->wbm_desc_rel_ring);
	ath11k_dp_srng_cleanup(ab, &dp->tcl_cmd_ring);
	ath11k_dp_srng_cleanup(ab, &dp->tcl_status_ring);
	for (i = 0; i < DP_TCL_NUM_RING_MAX; i++) {
		ath11k_dp_srng_cleanup(ab, &dp->tx_ring[i].tcl_data_ring);
		ath11k_dp_srng_cleanup(ab, &dp->tx_ring[i].tcl_comp_ring);
	}
	ath11k_dp_srng_cleanup(ab, &dp->reo_reinject_ring);
	ath11k_dp_srng_cleanup(ab, &dp->rx_rel_ring);
	ath11k_dp_srng_cleanup(ab, &dp->reo_except_ring);
	ath11k_dp_srng_cleanup(ab, &dp->reo_cmd_ring);
	ath11k_dp_srng_cleanup(ab, &dp->reo_status_ring);
}

static int ath11k_dp_srng_common_setup(struct ath11k_base *ab)
{
	struct ath11k_dp *dp = &ab->dp;
	struct hal_srng *srng;
	int i, ret;

	ret = ath11k_dp_srng_setup(ab, &dp->wbm_desc_rel_ring,
				   HAL_SW2WBM_RELEASE, 0, 0,
				   DP_WBM_RELEASE_RING_SIZE);
	if (ret) {
		ath11k_warn(ab, "failed to set up wbm2sw_release ring :%d\n",
			    ret);
		goto err;
	}

	ret = ath11k_dp_srng_setup(ab, &dp->tcl_cmd_ring, HAL_TCL_CMD, 0, 0,
				   DP_TCL_CMD_RING_SIZE);
	if (ret) {
		ath11k_warn(ab, "failed to set up tcl_cmd ring :%d\n", ret);
		goto err;
	}

	ret = ath11k_dp_srng_setup(ab, &dp->tcl_status_ring, HAL_TCL_STATUS,
				   0, 0, DP_TCL_STATUS_RING_SIZE);
	if (ret) {
		ath11k_warn(ab, "failed to set up tcl_status ring :%d\n", ret);
		goto err;
	}

	for (i = 0; i < DP_TCL_NUM_RING_MAX; i++) {
		ret = ath11k_dp_srng_setup(ab, &dp->tx_ring[i].tcl_data_ring,
					   HAL_TCL_DATA, i, 0,
					   DP_TCL_DATA_RING_SIZE);
		if (ret) {
			ath11k_warn(ab, "failed to set up tcl_data ring (%d) :%d\n",
				    i, ret);
			goto err;
		}

		ret = ath11k_dp_srng_setup(ab, &dp->tx_ring[i].tcl_comp_ring,
					   HAL_WBM2SW_RELEASE, i, 0,
					   DP_TX_COMP_RING_SIZE);
		if (ret) {
			ath11k_warn(ab, "failed to set up tcl_comp ring ring (%d) :%d\n",
				    i, ret);
			goto err;
		}

		srng = &ab->hal.srng_list[dp->tx_ring[i].tcl_data_ring.ring_id];
		ath11k_hal_tx_init_data_ring(ab, srng);
	}

	ret = ath11k_dp_srng_setup(ab, &dp->reo_reinject_ring, HAL_REO_REINJECT,
				   0, 0, DP_REO_REINJECT_RING_SIZE);
	if (ret) {
		ath11k_warn(ab, "failed to set up reo_reinject ring :%d\n",
			    ret);
		goto err;
	}

	ret = ath11k_dp_srng_setup(ab, &dp->rx_rel_ring, HAL_WBM2SW_RELEASE,
				   3, 0, DP_RX_RELEASE_RING_SIZE);
	if (ret) {
		ath11k_warn(ab, "failed to set up rx_rel ring :%d\n", ret);
		goto err;
	}

	ret = ath11k_dp_srng_setup(ab, &dp->reo_except_ring, HAL_REO_EXCEPTION,
				   0, 0, DP_REO_EXCEPTION_RING_SIZE);
	if (ret) {
		ath11k_warn(ab, "failed to set up reo_exception ring :%d\n",
			    ret);
		goto err;
	}

	ret = ath11k_dp_srng_setup(ab, &dp->reo_cmd_ring, HAL_REO_CMD,
				   0, 0, DP_REO_CMD_RING_SIZE);
	if (ret) {
		ath11k_warn(ab, "failed to set up reo_cmd ring :%d\n", ret);
		goto err;
	}

	srng = &ab->hal.srng_list[dp->reo_cmd_ring.ring_id];
	ath11k_hal_reo_init_cmd_ring(ab, srng);

	ret = ath11k_dp_srng_setup(ab, &dp->reo_status_ring, HAL_REO_STATUS,
				   0, 0, DP_REO_STATUS_RING_SIZE);
	if (ret) {
		ath11k_warn(ab, "failed to set up reo_status ring :%d\n", ret);
		goto err;
	}

	ath11k_hal_reo_hw_setup(ab);

	return 0;

err:
	ath11k_dp_srng_common_cleanup(ab);

	return ret;
}

static void ath11k_dp_scatter_idle_link_desc_cleanup(struct ath11k_base *ab)
{
	struct ath11k_dp *dp = &ab->dp;
	struct hal_wbm_idle_scatter_list *slist = dp->scatter_list;
	int i;

	for (i = 0; i < DP_IDLE_SCATTER_BUFS_MAX; i++) {
		if (!slist[i].vaddr)
			continue;

		dma_free_coherent(ab->dev, HAL_WBM_IDLE_SCATTER_BUF_SIZE_MAX,
				  slist[i].vaddr, slist[i].paddr);
		slist[i].vaddr = NULL;
	}
}

static int ath11k_dp_scatter_idle_link_desc_setup(struct ath11k_base *ab,
						  int size,
						  u32 n_link_desc_bank,
						  u32 n_link_desc,
						  u32 last_bank_sz)
{
	struct ath11k_dp *dp = &ab->dp;
	struct dp_link_desc_bank *link_desc_banks = dp->link_desc_banks;
	struct hal_wbm_idle_scatter_list *slist = dp->scatter_list;
	u32 n_entries_per_buf;
	int num_scatter_buf, scatter_idx;
	struct hal_wbm_link_desc *scatter_buf;
	int align_bytes, n_entries;
	dma_addr_t paddr;
	int rem_entries;
	int i;
	int ret = 0;
	u32 end_offset;

	n_entries_per_buf = HAL_WBM_IDLE_SCATTER_BUF_SIZE /
			    ath11k_hal_srng_get_entrysize(HAL_WBM_IDLE_LINK);
	num_scatter_buf = DIV_ROUND_UP(size, HAL_WBM_IDLE_SCATTER_BUF_SIZE);

	if (num_scatter_buf > DP_IDLE_SCATTER_BUFS_MAX)
		return -EINVAL;

	for (i = 0; i < num_scatter_buf; i++) {
		slist[i].vaddr = dma_alloc_coherent(ab->dev,
						    HAL_WBM_IDLE_SCATTER_BUF_SIZE_MAX,
						    &slist[i].paddr, GFP_KERNEL);
		if (!slist[i].vaddr) {
			ret = -ENOMEM;
			goto err;
		}
	}

	scatter_idx = 0;
	scatter_buf = slist[scatter_idx].vaddr;
	rem_entries = n_entries_per_buf;

	for (i = 0; i < n_link_desc_bank; i++) {
		align_bytes = link_desc_banks[i].vaddr -
			      link_desc_banks[i].vaddr_unaligned;
		n_entries = (DP_LINK_DESC_ALLOC_SIZE_THRESH - align_bytes) /
			     HAL_LINK_DESC_SIZE;
		paddr = link_desc_banks[i].paddr;
		while (n_entries) {
			ath11k_hal_set_link_desc_addr(scatter_buf, i, paddr);
			n_entries--;
			paddr += HAL_LINK_DESC_SIZE;
			if (rem_entries) {
				rem_entries--;
				scatter_buf++;
				continue;
			}

			rem_entries = n_entries_per_buf;
			scatter_idx++;
			scatter_buf = slist[scatter_idx].vaddr;
		}
	}

	end_offset = (scatter_buf - slist[scatter_idx].vaddr) *
		     sizeof(struct hal_wbm_link_desc);
	ath11k_hal_setup_link_idle_list(ab, slist, num_scatter_buf,
					n_link_desc, end_offset);

	return 0;

err:
	ath11k_dp_scatter_idle_link_desc_cleanup(ab);

	return ret;
}

static void
ath11k_dp_link_desc_bank_free(struct ath11k_base *ab,
			      struct dp_link_desc_bank *link_desc_banks)
{
	int i;

	for (i = 0; i < DP_LINK_DESC_BANKS_MAX; i++) {
		if (link_desc_banks[i].vaddr_unaligned) {
			dma_free_coherent(ab->dev,
					  link_desc_banks[i].size,
					  link_desc_banks[i].vaddr_unaligned,
					  link_desc_banks[i].paddr_unaligned);
			link_desc_banks[i].vaddr_unaligned = NULL;
		}
	}
}

static int ath11k_dp_link_desc_bank_alloc(struct ath11k_base *ab,
					  struct dp_link_desc_bank *desc_bank,
					  int n_link_desc_bank,
					  int last_bank_sz)
{
	struct ath11k_dp *dp = &ab->dp;
	int i;
	int ret = 0;
	int desc_sz = DP_LINK_DESC_ALLOC_SIZE_THRESH;

	for (i = 0; i < n_link_desc_bank; i++) {
		if (i == (n_link_desc_bank - 1) && last_bank_sz)
			desc_sz = last_bank_sz;

		desc_bank[i].vaddr_unaligned =
					dma_alloc_coherent(ab->dev, desc_sz,
							   &desc_bank[i].paddr_unaligned,
							   GFP_KERNEL);
		if (!desc_bank[i].vaddr_unaligned) {
			ret = -ENOMEM;
			goto err;
		}

		desc_bank[i].vaddr = PTR_ALIGN(desc_bank[i].vaddr_unaligned,
					       HAL_LINK_DESC_ALIGN);
		desc_bank[i].paddr = desc_bank[i].paddr_unaligned +
				     ((unsigned long)desc_bank[i].vaddr -
				      (unsigned long)desc_bank[i].vaddr_unaligned);
		desc_bank[i].size = desc_sz;
	}

	return 0;

err:
	ath11k_dp_link_desc_bank_free(ab, dp->link_desc_banks);

	return ret;
}

void ath11k_dp_link_desc_cleanup(struct ath11k_base *ab,
				 struct dp_link_desc_bank *desc_bank,
				 u32 ring_type, struct dp_srng *ring)
{
	ath11k_dp_link_desc_bank_free(ab, desc_bank);

	if (ring_type != HAL_RXDMA_MONITOR_DESC) {
		ath11k_dp_srng_cleanup(ab, ring);
		ath11k_dp_scatter_idle_link_desc_cleanup(ab);
	}
}

static int ath11k_wbm_idle_ring_setup(struct ath11k_base *ab, u32 *n_link_desc)
{
	struct ath11k_dp *dp = &ab->dp;
	u32 n_mpdu_link_desc, n_mpdu_queue_desc;
	u32 n_tx_msdu_link_desc, n_rx_msdu_link_desc;
	int ret = 0;

	n_mpdu_link_desc = (DP_NUM_TIDS_MAX * DP_AVG_MPDUS_PER_TID_MAX) /
			   HAL_NUM_MPDUS_PER_LINK_DESC;

	n_mpdu_queue_desc = n_mpdu_link_desc /
			    HAL_NUM_MPDU_LINKS_PER_QUEUE_DESC;

	n_tx_msdu_link_desc = (DP_NUM_TIDS_MAX * DP_AVG_FLOWS_PER_TID *
			       DP_AVG_MSDUS_PER_FLOW) /
			      HAL_NUM_TX_MSDUS_PER_LINK_DESC;

	n_rx_msdu_link_desc = (DP_NUM_TIDS_MAX * DP_AVG_MPDUS_PER_TID_MAX *
			       DP_AVG_MSDUS_PER_MPDU) /
			      HAL_NUM_RX_MSDUS_PER_LINK_DESC;

	*n_link_desc = n_mpdu_link_desc + n_mpdu_queue_desc +
		      n_tx_msdu_link_desc + n_rx_msdu_link_desc;

	if (*n_link_desc & (*n_link_desc - 1))
		*n_link_desc = 1 << fls(*n_link_desc);

	ret = ath11k_dp_srng_setup(ab, &dp->wbm_idle_ring,
				   HAL_WBM_IDLE_LINK, 0, 0, *n_link_desc);
	if (ret) {
		ath11k_warn(ab, "failed to setup wbm_idle_ring: %d\n", ret);
		return ret;
	}
	return ret;
}

int ath11k_dp_link_desc_setup(struct ath11k_base *ab,
			      struct dp_link_desc_bank *link_desc_banks,
			      u32 ring_type, struct hal_srng *srng,
			      u32 n_link_desc)
{
	u32 tot_mem_sz;
	u32 n_link_desc_bank, last_bank_sz;
	u32 entry_sz, align_bytes, n_entries;
	u32 paddr;
	u32 *desc;
	int i, ret;

	tot_mem_sz = n_link_desc * HAL_LINK_DESC_SIZE;
	tot_mem_sz += HAL_LINK_DESC_ALIGN;

	if (tot_mem_sz <= DP_LINK_DESC_ALLOC_SIZE_THRESH) {
		n_link_desc_bank = 1;
		last_bank_sz = tot_mem_sz;
	} else {
		n_link_desc_bank = tot_mem_sz /
				   (DP_LINK_DESC_ALLOC_SIZE_THRESH -
				    HAL_LINK_DESC_ALIGN);
		last_bank_sz = tot_mem_sz %
			       (DP_LINK_DESC_ALLOC_SIZE_THRESH -
				HAL_LINK_DESC_ALIGN);

		if (last_bank_sz)
			n_link_desc_bank += 1;
	}

	if (n_link_desc_bank > DP_LINK_DESC_BANKS_MAX)
		return -EINVAL;

	ret = ath11k_dp_link_desc_bank_alloc(ab, link_desc_banks,
					     n_link_desc_bank, last_bank_sz);
	if (ret)
		return ret;

	/* Setup link desc idle list for HW internal usage */
	entry_sz = ath11k_hal_srng_get_entrysize(ring_type);
	tot_mem_sz = entry_sz * n_link_desc;

	/* Setup scatter desc list when the total memory requirement is more */
	if (tot_mem_sz > DP_LINK_DESC_ALLOC_SIZE_THRESH &&
	    ring_type != HAL_RXDMA_MONITOR_DESC) {
		ret = ath11k_dp_scatter_idle_link_desc_setup(ab, tot_mem_sz,
							     n_link_desc_bank,
							     n_link_desc,
							     last_bank_sz);
		if (ret) {
			ath11k_warn(ab, "failed to setup scatting idle list descriptor :%d\n",
				    ret);
			goto fail_desc_bank_free;
		}

		return 0;
	}

	spin_lock_bh(&srng->lock);

	ath11k_hal_srng_access_begin(ab, srng);

	for (i = 0; i < n_link_desc_bank; i++) {
		align_bytes = link_desc_banks[i].vaddr -
			      link_desc_banks[i].vaddr_unaligned;
		n_entries = (link_desc_banks[i].size - align_bytes) /
			    HAL_LINK_DESC_SIZE;
		paddr = link_desc_banks[i].paddr;
		while (n_entries &&
		       (desc = ath11k_hal_srng_src_get_next_entry(ab, srng))) {
			ath11k_hal_set_link_desc_addr((struct hal_wbm_link_desc *)desc,
						      i, paddr);
			n_entries--;
			paddr += HAL_LINK_DESC_SIZE;
		}
	}

	ath11k_hal_srng_access_end(ab, srng);

	spin_unlock_bh(&srng->lock);

	return 0;

fail_desc_bank_free:
	ath11k_dp_link_desc_bank_free(ab, link_desc_banks);

	return ret;
}

int ath11k_dp_service_srng(struct ath11k_base *ab,
			   struct ath11k_ext_irq_grp *irq_grp,
			   int budget)
{
	struct napi_struct *napi = &irq_grp->napi;
	int grp_id = irq_grp->grp_id;
	int work_done = 0;
	int i = 0;
	int tot_work_done = 0;

	while (ath11k_tx_ring_mask[grp_id] >> i) {
		if (ath11k_tx_ring_mask[grp_id] & BIT(i))
			ath11k_dp_tx_completion_handler(ab, i);
		i++;
	}

	if (ath11k_rx_err_ring_mask[grp_id]) {
		work_done = ath11k_dp_process_rx_err(ab, napi, budget);
		budget -= work_done;
		tot_work_done += work_done;
		if (budget <= 0)
			goto done;
	}

	if (ath11k_rx_wbm_rel_ring_mask[grp_id]) {
		work_done = ath11k_dp_rx_process_wbm_err(ab,
							 napi,
							 budget);
		budget -= work_done;
		tot_work_done += work_done;

		if (budget <= 0)
			goto done;
	}

	if (ath11k_rx_ring_mask[grp_id]) {
		for (i = 0; i <  ab->num_radios; i++) {
			if (ath11k_rx_ring_mask[grp_id] & BIT(i)) {
				work_done = ath11k_dp_process_rx(ab, i, napi,
								 &irq_grp->pending_q,
								 budget);
				budget -= work_done;
				tot_work_done += work_done;
			}
			if (budget <= 0)
				goto done;
		}
	}

	if (rx_mon_status_ring_mask[grp_id]) {
		for (i = 0; i <  ab->num_radios; i++) {
			if (rx_mon_status_ring_mask[grp_id] & BIT(i)) {
				work_done =
				ath11k_dp_rx_process_mon_rings(ab,
							       i, napi,
							       budget);
				budget -= work_done;
				tot_work_done += work_done;
			}
			if (budget <= 0)
				goto done;
		}
	}

	if (ath11k_reo_status_ring_mask[grp_id])
		ath11k_dp_process_reo_status(ab);

	for (i = 0; i < ab->num_radios; i++) {
		if (ath11k_rxdma2host_ring_mask[grp_id] & BIT(i)) {
			work_done = ath11k_dp_process_rxdma_err(ab, i, budget);
			budget -= work_done;
			tot_work_done += work_done;
		}

		if (budget <= 0)
			goto done;

		if (ath11k_host2rxdma_ring_mask[grp_id] & BIT(i)) {
			struct ath11k_pdev_dp *dp = &ab->pdevs[i].ar->dp;
			struct dp_rxdma_ring *rx_ring = &dp->rx_refill_buf_ring;

			ath11k_dp_rxbufs_replenish(ab, i, rx_ring, 0,
						   HAL_RX_BUF_RBM_SW3_BM,
						   GFP_ATOMIC);
		}
	}
	/* TODO: Implement handler for other interrupts */

done:
	return tot_work_done;
}

void ath11k_dp_pdev_free(struct ath11k_base *ab)
{
	struct ath11k *ar;
	int i;

	for (i = 0; i < ab->num_radios; i++) {
		ar = ab->pdevs[i].ar;
		ath11k_dp_rx_pdev_free(ab, i);
		ath11k_debug_unregister(ar);
		ath11k_dp_rx_pdev_mon_detach(ar);
	}
}

void ath11k_dp_pdev_pre_alloc(struct ath11k_base *ab)
{
	struct ath11k *ar;
	struct ath11k_pdev_dp *dp;
	int i;

	for (i = 0; i <  ab->num_radios; i++) {
		ar = ab->pdevs[i].ar;
		dp = &ar->dp;
		dp->mac_id = i;
		idr_init(&dp->rx_refill_buf_ring.bufs_idr);
		spin_lock_init(&dp->rx_refill_buf_ring.idr_lock);
		atomic_set(&dp->num_tx_pending, 0);
		init_waitqueue_head(&dp->tx_empty_waitq);
		idr_init(&dp->rx_mon_status_refill_ring.bufs_idr);
		spin_lock_init(&dp->rx_mon_status_refill_ring.idr_lock);
		idr_init(&dp->rxdma_mon_buf_ring.bufs_idr);
		spin_lock_init(&dp->rxdma_mon_buf_ring.idr_lock);
	}
}

int ath11k_dp_pdev_alloc(struct ath11k_base *ab)
{
	struct ath11k *ar;
	int ret;
	int i;

	/* TODO:Per-pdev rx ring unlike tx ring which is mapped to different AC's */
	for (i = 0; i < ab->num_radios; i++) {
		ar = ab->pdevs[i].ar;
		ret = ath11k_dp_rx_pdev_alloc(ab, i);
		if (ret) {
			ath11k_warn(ab, "failed to allocate pdev rx for pdev_id :%d\n",
				    i);
			goto err;
		}
		ret = ath11k_dp_rx_pdev_mon_attach(ar);
		if (ret) {
			ath11k_warn(ab, "failed to initialize mon pdev %d\n",
				    i);
			goto err;
		}
	}

	return 0;

err:
	ath11k_dp_pdev_free(ab);

	return ret;
}

int ath11k_dp_htt_connect(struct ath11k_dp *dp)
{
	struct ath11k_htc_svc_conn_req conn_req;
	struct ath11k_htc_svc_conn_resp conn_resp;
	int status;

	memset(&conn_req, 0, sizeof(conn_req));
	memset(&conn_resp, 0, sizeof(conn_resp));

	conn_req.ep_ops.ep_tx_complete = ath11k_dp_htt_htc_tx_complete;
	conn_req.ep_ops.ep_rx_complete = ath11k_dp_htt_htc_t2h_msg_handler;

	/* connect to control service */
	conn_req.service_id = ATH11K_HTC_SVC_ID_HTT_DATA_MSG;

	status = ath11k_htc_connect_service(&dp->ab->htc, &conn_req,
					    &conn_resp);

	if (status)
		return status;

	dp->eid = conn_resp.eid;

	return 0;
}

static void ath11k_dp_update_vdev_search(struct ath11k_vif *arvif)
{
	 /* For STA mode, enable address search index,
	  * tcl uses ast_hash value in the descriptor.
	  */
	switch (arvif->vdev_type) {
	case WMI_VDEV_TYPE_STA:
		arvif->hal_addr_search_flags = HAL_TX_ADDRX_EN;
		arvif->search_type = HAL_TX_ADDR_SEARCH_INDEX;
		break;
	case WMI_VDEV_TYPE_AP:
	case WMI_VDEV_TYPE_IBSS:
		arvif->hal_addr_search_flags = HAL_TX_ADDRX_EN;
		arvif->search_type = HAL_TX_ADDR_SEARCH_DEFAULT;
		break;
	case WMI_VDEV_TYPE_MONITOR:
	default:
		return;
	}
}

void ath11k_dp_vdev_tx_attach(struct ath11k *ar, struct ath11k_vif *arvif)
{
	arvif->tcl_metadata |= FIELD_PREP(HTT_TCL_META_DATA_TYPE, 1) |
			       FIELD_PREP(HTT_TCL_META_DATA_VDEV_ID,
					  arvif->vdev_id) |
			       FIELD_PREP(HTT_TCL_META_DATA_PDEV_ID,
					  ar->pdev->pdev_id);

	/* set HTT extension valid bit to 0 by default */
	arvif->tcl_metadata &= ~HTT_TCL_META_DATA_VALID_HTT;

	ath11k_dp_update_vdev_search(arvif);
}

static int ath11k_dp_tx_pending_cleanup(int buf_id, void *skb, void *ctx)
{
	struct ath11k_base *ab = (struct ath11k_base *)ctx;
	struct sk_buff *msdu = skb;

	dma_unmap_single(ab->dev, ATH11K_SKB_CB(msdu)->paddr, msdu->len,
			 DMA_TO_DEVICE);

	dev_kfree_skb_any(msdu);

	return 0;
}

void ath11k_dp_free(struct ath11k_base *ab)
{
	struct ath11k_dp *dp = &ab->dp;
	int i;

	ath11k_dp_link_desc_cleanup(ab, dp->link_desc_banks,
				    HAL_WBM_IDLE_LINK, &dp->wbm_idle_ring);

	ath11k_dp_srng_common_cleanup(ab);

	ath11k_dp_reo_cmd_list_cleanup(ab);

	for (i = 0; i < DP_TCL_NUM_RING_MAX; i++) {
		spin_lock_bh(&dp->tx_ring[i].tx_idr_lock);
		idr_for_each(&dp->tx_ring[i].txbuf_idr,
			     ath11k_dp_tx_pending_cleanup, ab);
		idr_destroy(&dp->tx_ring[i].txbuf_idr);
		spin_unlock_bh(&dp->tx_ring[i].tx_idr_lock);

		spin_lock_bh(&dp->tx_ring[i].tx_status_lock);
		kfifo_free(&dp->tx_ring[i].tx_status_fifo);
		spin_unlock_bh(&dp->tx_ring[i].tx_status_lock);
	}

	/* Deinit any SOC level resource */
}

int ath11k_dp_alloc(struct ath11k_base *ab)
{
	struct ath11k_dp *dp = &ab->dp;
	struct hal_srng *srng = NULL;
	size_t size = 0;
	u32 n_link_desc = 0;
	int ret;
	int i;

	dp->ab = ab;

	INIT_LIST_HEAD(&dp->reo_cmd_list);
	INIT_LIST_HEAD(&dp->reo_cmd_cache_flush_list);
	spin_lock_init(&dp->reo_cmd_lock);

	ret = ath11k_wbm_idle_ring_setup(ab, &n_link_desc);
	if (ret) {
		ath11k_warn(ab, "failed to setup wbm_idle_ring: %d\n", ret);
		return ret;
	}

	srng = &ab->hal.srng_list[dp->wbm_idle_ring.ring_id];

	ret = ath11k_dp_link_desc_setup(ab, dp->link_desc_banks,
					HAL_WBM_IDLE_LINK, srng, n_link_desc);
	if (ret) {
		ath11k_warn(ab, "failed to setup link desc: %d\n", ret);
		return ret;
	}

	ret = ath11k_dp_srng_common_setup(ab);
	if (ret)
		goto fail_link_desc_cleanup;

	size = roundup_pow_of_two(DP_TX_COMP_RING_SIZE);

	for (i = 0; i < DP_TCL_NUM_RING_MAX; i++) {
		idr_init(&dp->tx_ring[i].txbuf_idr);
		spin_lock_init(&dp->tx_ring[i].tx_idr_lock);
		dp->tx_ring[i].tcl_data_ring_id = i;

		spin_lock_init(&dp->tx_ring[i].tx_status_lock);
		ret = kfifo_alloc(&dp->tx_ring[i].tx_status_fifo, size,
				  GFP_KERNEL);
		if (ret)
			goto fail_cmn_srng_cleanup;
	}

	for (i = 0; i < HAL_DSCP_TID_MAP_TBL_NUM_ENTRIES_MAX; i++)
		ath11k_hal_tx_set_dscp_tid_map(ab, i);

	/* Init any SOC level resource for DP */

	return 0;

fail_cmn_srng_cleanup:
	ath11k_dp_srng_common_cleanup(ab);

fail_link_desc_cleanup:
	ath11k_dp_link_desc_cleanup(ab, dp->link_desc_banks,
				    HAL_WBM_IDLE_LINK, &dp->wbm_idle_ring);

	return ret;
}
