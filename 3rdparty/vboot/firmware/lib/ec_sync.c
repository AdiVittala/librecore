/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * EC software sync routines for vboot
 */

#include "2sysincludes.h"
#include "2common.h"
#include "2misc.h"
#include "2nvstorage.h"

#include "sysincludes.h"
#include "ec_sync.h"
#include "gbb_header.h"
#include "vboot_common.h"
#include "vboot_kernel.h"

#define VB2_SD_FLAG_ECSYNC_RW					\
	(VB2_SD_FLAG_ECSYNC_EC_RW | VB2_SD_FLAG_ECSYNC_PD_RW)
#define VB2_SD_FLAG_ECSYNC_ANY					\
	(VB2_SD_FLAG_ECSYNC_EC_RO | VB2_SD_FLAG_ECSYNC_RW)
#define VB2_SD_FLAG_ECSYNC_IN_RW					\
	(VB2_SD_FLAG_ECSYNC_EC_IN_RW | VB2_SD_FLAG_ECSYNC_PD_IN_RW)

#define IN_RW(devidx)							\
	((devidx) ? VB2_SD_FLAG_ECSYNC_PD_IN_RW : VB2_SD_FLAG_ECSYNC_EC_IN_RW)

#define WHICH_EC(devidx, select) \
	((select) == VB_SELECT_FIRMWARE_READONLY ? VB2_SD_FLAG_ECSYNC_EC_RO : \
	 ((devidx) ? VB2_SD_FLAG_ECSYNC_PD_RW : VB2_SD_FLAG_ECSYNC_EC_RW))

static void request_recovery(struct vb2_context *ctx, uint32_t recovery_request)
{
	VB2_DEBUG("request_recovery(%u)\n", recovery_request);

	vb2_nv_set(ctx, VB2_NV_RECOVERY_REQUEST, recovery_request);
}

/**
 * Wrapper around VbExEcProtect() which sets recovery reason on error.
 */
static VbError_t protect_ec(struct vb2_context *ctx, int devidx,
			   enum VbSelectFirmware_t select)
{
	int rv = VbExEcProtect(devidx, select);

	if (rv == VBERROR_EC_REBOOT_TO_RO_REQUIRED) {
		VB2_DEBUG("VbExEcProtect() needs reboot\n");
	} else if (rv != VBERROR_SUCCESS) {
		VB2_DEBUG("VbExEcProtect() returned %d\n", rv);
		request_recovery(ctx, VB2_RECOVERY_EC_PROTECT);
	}
	return rv;
}

/**
 * Print a hash to debug output
 *
 * @param hash		Pointer to the hash
 * @param hash_size	Size of the hash in bytes
 * @param desc		Description of what's being hashed
 */
static void print_hash(const uint8_t *hash, uint32_t hash_size,
		       const char *desc)
{
	int i;

	VB2_DEBUG("%s hash: ", desc);
	for (i = 0; i < hash_size; i++)
		VB2_DEBUG_RAW("%02x", hash[i]);
	VB2_DEBUG_RAW("\n");
}

/**
 * Check if the hash of the EC code matches the expected hash.
 *
 * @param ctx		Vboot2 context
 * @param devidx	Index of EC device to check
 * @param select	Which firmware image to check
 * @return VB2_SUCCESS, or non-zero error code.
 */
static int check_ec_hash(struct vb2_context *ctx, int devidx,
			 enum VbSelectFirmware_t select)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);

	/* Get current EC hash. */
	const uint8_t *ec_hash = NULL;
	int ec_hash_size;
	int rv = VbExEcHashImage(devidx, select, &ec_hash, &ec_hash_size);
	if (rv) {
		VB2_DEBUG("VbExEcHashImage() returned %d\n", rv);
		request_recovery(ctx, VB2_RECOVERY_EC_HASH_FAILED);
		return VB2_ERROR_EC_HASH_IMAGE;
	}
	print_hash(ec_hash, ec_hash_size,
		   select == VB_SELECT_FIRMWARE_READONLY ? "RO" : "RW");

	/* Get expected EC hash. */
	const uint8_t *hash = NULL;
	int hash_size;
	rv = VbExEcGetExpectedImageHash(devidx, select, &hash, &hash_size);
	if (rv) {
		VB2_DEBUG("VbExEcGetExpectedImageHash() returned %d\n", rv);
		request_recovery(ctx, VB2_RECOVERY_EC_EXPECTED_HASH);
		return VB2_ERROR_EC_HASH_EXPECTED;
	}
	if (ec_hash_size != hash_size) {
		VB2_DEBUG("EC uses %d-byte hash, but AP-RW contains %d bytes\n",
			  ec_hash_size, hash_size);
		request_recovery(ctx, VB2_RECOVERY_EC_HASH_SIZE);
		return VB2_ERROR_EC_HASH_SIZE;
	}

	if (vb2_safe_memcmp(ec_hash, hash, hash_size)) {
		print_hash(hash, hash_size, "Expected");
		sd->flags |= WHICH_EC(devidx, select);
	}

	return VB2_SUCCESS;
}

/**
 * Update the specified EC and verify the update succeeded
 *
 * @param ctx		Vboot2 context
 * @param devidx	Index of EC device to check
 * @param select	Which firmware image to check
 * @return VBERROR_SUCCESS, or non-zero error code.
 */
static VbError_t update_ec(struct vb2_context *ctx, int devidx,
			   enum VbSelectFirmware_t select)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);

	VB2_DEBUG("updating %s...\n",
		  select == VB_SELECT_FIRMWARE_READONLY ? "RO" : "RW");

	/* Get expected EC image */
	const uint8_t *want = NULL;
	int want_size;
	int rv = VbExEcGetExpectedImage(devidx, select, &want, &want_size);
	if (rv) {
		VB2_DEBUG("VbExEcGetExpectedImage() returned %d\n", rv);
		request_recovery(ctx, VB2_RECOVERY_EC_EXPECTED_IMAGE);
		return rv;
	}
	VB2_DEBUG("image len = %d\n", want_size);

	rv = VbExEcUpdateImage(devidx, select, want, want_size);
	if (rv != VBERROR_SUCCESS) {
		VB2_DEBUG("VbExEcUpdateImage() returned %d\n", rv);

		/*
		 * The EC may know it needs a reboot.  It may need to
		 * unprotect the region before updating, or may need to
		 * reboot after updating.  Either way, it's not an error
		 * requiring recovery mode.
		 *
		 * If we fail for any other reason, trigger recovery
		 * mode.
		 */
		if (rv != VBERROR_EC_REBOOT_TO_RO_REQUIRED)
			request_recovery(ctx, VB2_RECOVERY_EC_UPDATE);

		return rv;
	}

	/* Verify the EC was updated properly */
	sd->flags &= ~WHICH_EC(devidx, select);
	if (check_ec_hash(ctx, devidx, select) != VB2_SUCCESS)
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	if (sd->flags & WHICH_EC(devidx, select)) {
		VB2_DEBUG("Failed to update\n");
		request_recovery(ctx, VB2_RECOVERY_EC_UPDATE);
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	}

	return VBERROR_SUCCESS;
}

/**
 * Check if the EC has the correct image active.
 *
 * @param ctx		Vboot2 context
 * @param devidx	Which device (EC=0, PD=1)
 * @return VBERROR_SUCCESS, or non-zero if error.
 */
static VbError_t check_ec_active(struct vb2_context *ctx, int devidx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);

	/* Determine whether the EC is in RO or RW */
	int in_rw = 0;
	int rv = VbExEcRunningRW(devidx, &in_rw);
	if (in_rw) {
		sd->flags |= IN_RW(devidx);
	}

	if (sd->recovery_reason) {
		/*
		 * Recovery mode; just verify the EC is in RO code.  Don't do
		 * software sync, since we don't have a RW image.
		 */
		if (rv == VBERROR_SUCCESS && in_rw == 1) {
			/*
			 * EC is definitely in RW firmware.  We want it in
			 * read-only code, so preserve the current recovery
			 * reason and reboot.
			 *
			 * We don't reboot on error or unknown EC code, because
			 * we could end up in an endless reboot loop.  If we
			 * had some way to track that we'd already rebooted for
			 * this reason, we could retry only once.
			 */
			VB2_DEBUG("want recovery but got EC-RW\n");
			request_recovery(ctx, sd->recovery_reason);
			return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
		}

		VB2_DEBUG("in recovery; EC-RO\n");
		return VBERROR_SUCCESS;
	}

	/*
	 * Not in recovery.  If we couldn't determine where the EC was,
	 * reboot to recovery.
	 */
	if (rv != VBERROR_SUCCESS) {
		VB2_DEBUG("VbExEcRunningRW() returned %d\n", rv);
		request_recovery(ctx, VB2_RECOVERY_EC_UNKNOWN_IMAGE);
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	}

	return VBERROR_SUCCESS;
}

#define RO_RETRIES 2  /* Maximum times to retry flashing RO */

/**
 * Sync, jump, and protect one EC device
 *
 * @param ctx		Vboot2 context
 * @param devidx	Which device (EC=0, PD=1)
 * @return VBERROR_SUCCESS, or non-zero if error.
 */
static VbError_t sync_one_ec(struct vb2_context *ctx, int devidx,
			     VbCommonParams *cparams)
{
	VbSharedDataHeader *shared =
			(VbSharedDataHeader *)cparams->shared_data_blob;
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	const enum VbSelectFirmware_t select_rw =
		shared->firmware_index ? VB_SELECT_FIRMWARE_B :
		VB_SELECT_FIRMWARE_A;
	int rv;

	VB2_DEBUG("devidx=%d\n", devidx);

	/* Update the RW Image */
	if (sd->flags & VB2_SD_FLAG_ECSYNC_RW) {
		if (VB2_SUCCESS != update_ec(ctx, devidx, select_rw))
			return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	}

	/* Tell EC to jump to its RW image */
	if (!(sd->flags & IN_RW(devidx))) {
		VB2_DEBUG("jumping to EC-RW\n");
		rv = VbExEcJumpToRW(devidx);
		if (rv != VBERROR_SUCCESS) {
			VB2_DEBUG("VbExEcJumpToRW() returned %x\n", rv);

			/*
			 * If a previous AP boot has called VbExEcStayInRO(),
			 * we need to reboot the EC to unlock the ability to
			 * jump to the RW firmware.
			 *
			 * All other errors trigger recovery mode.
			 */
			if (rv != VBERROR_EC_REBOOT_TO_RO_REQUIRED)
				request_recovery(ctx, VB2_RECOVERY_EC_JUMP_RW);

			return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
		}
	}

	/* Might need to update EC-RO (but not PD-RO) */
	if (sd->flags & VB2_SD_FLAG_ECSYNC_EC_RO) {
		VB2_DEBUG("RO Software Sync\n");

		/* Reset RO Software Sync NV flag */
		vb2_nv_set(ctx, VB2_NV_TRY_RO_SYNC, 0);

		/*
		 * Get the current recovery request (if any).  This gets
		 * overwritten by a failed try.  If a later try succeeds, we'll
		 * need to restore this request (or the lack of a request), or
		 * else we'll end up in recovery mode even though RO software
		 * sync did eventually succeed.
		 */
		uint32_t recovery_request =
				vb2_nv_get(ctx, VB2_NV_RECOVERY_REQUEST);

		/* Update the RO Image. */
		int num_tries;
		for (num_tries = 0; num_tries < RO_RETRIES; num_tries++) {
			if (VB2_SUCCESS ==
			    update_ec(ctx, devidx, VB_SELECT_FIRMWARE_READONLY))
				break;
		}
		if (num_tries == RO_RETRIES) {
			/* Ran out of tries */
			return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
		} else if (num_tries) {
			/*
			 * Update succeeded after a failure, so we've polluted
			 * the recovery request.  Restore it.
			 */
			request_recovery(ctx, recovery_request);
		}
	}

	/* Protect RO flash */
	rv = protect_ec(ctx, devidx, VB_SELECT_FIRMWARE_READONLY);
	if (rv != VBERROR_SUCCESS)
		return rv;

	/* Protect RW flash */
	rv = protect_ec(ctx, devidx, select_rw);
	if (rv != VBERROR_SUCCESS)
		return rv;

	rv = VbExEcDisableJump(devidx);
	if (rv != VBERROR_SUCCESS) {
		VB2_DEBUG("VbExEcDisableJump() returned %d\n", rv);
		request_recovery(ctx, VB2_RECOVERY_EC_SOFTWARE_SYNC);
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	}

	return rv;
}

VbError_t ec_sync_phase1(struct vb2_context *ctx, VbCommonParams *cparams)
{
	VbSharedDataHeader *shared =
		(VbSharedDataHeader *)cparams->shared_data_blob;
	struct vb2_shared_data *sd = vb2_get_sd(ctx);

	/* Reasons not to do sync at all */
	if (!(shared->flags & VBSD_EC_SOFTWARE_SYNC))
		return VBERROR_SUCCESS;
	if (cparams->gbb->flags & GBB_FLAG_DISABLE_EC_SOFTWARE_SYNC)
		return VBERROR_SUCCESS;

#ifdef PD_SYNC
	const int do_pd_sync = !(cparams->gbb->flags &
				 GBB_FLAG_DISABLE_PD_SOFTWARE_SYNC);
#else
	const int do_pd_sync = 0;
#endif

	/* Make sure the EC is running the correct image */
	if (check_ec_active(ctx, 0))
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	if (do_pd_sync && check_ec_active(ctx, 1))
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;

	/*
	 * In recovery mode; just verify the EC is in RO code.  Don't do
	 * software sync, since we don't have a RW image.
	 */
	if (sd->recovery_reason)
		return VBERROR_SUCCESS;

	/* See if we need to update RW.  Failures trigger recovery mode. */
	const enum VbSelectFirmware_t select_rw =
			shared->firmware_index ? VB_SELECT_FIRMWARE_B :
			VB_SELECT_FIRMWARE_A;
	if (check_ec_hash(ctx, 0, select_rw))
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	if (do_pd_sync && check_ec_hash(ctx, 1, select_rw))
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	/*
	 * See if we need to update EC-RO (devidx=0).
	 *
	 * If we want to extend this in the future to update PD-RO, we'll use a
	 * different NV flag so we can track EC-RO and PD-RO updates
	 * separately.
	 */
	if (vb2_nv_get(ctx, VB2_NV_TRY_RO_SYNC) &&
	    !(shared->flags & VBSD_BOOT_FIRMWARE_WP_ENABLED) &&
	    check_ec_hash(ctx, 0, VB_SELECT_FIRMWARE_READONLY)) {
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	}

	/*
	 * If we're in RW, we need to reboot back to RO because RW can't be
	 * updated while we're running it.
	 *
	 * TODO: Technically this isn't true for ECs which don't execute from
	 * flash.  For example, if the EC loads code from SPI into RAM before
	 * executing it.
	 */
	if ((sd->flags & VB2_SD_FLAG_ECSYNC_RW) &&
	    (sd->flags & VB2_SD_FLAG_ECSYNC_IN_RW)) {
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	}

	return VBERROR_SUCCESS;
}

int ec_will_update_slowly(struct vb2_context *ctx, VbCommonParams *cparams)
{
	VbSharedDataHeader *shared =
		(VbSharedDataHeader *)cparams->shared_data_blob;
	struct vb2_shared_data *sd = vb2_get_sd(ctx);

	return ((sd->flags & VB2_SD_FLAG_ECSYNC_ANY) &&
		(shared->flags & VBSD_EC_SLOW_UPDATE));
}


VbError_t ec_sync_phase2(struct vb2_context *ctx, VbCommonParams *cparams)
{
	VbSharedDataHeader *shared =
		(VbSharedDataHeader *)cparams->shared_data_blob;
	struct vb2_shared_data *sd = vb2_get_sd(ctx);

	/* Reasons not to do sync at all */
	if (!(shared->flags & VBSD_EC_SOFTWARE_SYNC))
		return VBERROR_SUCCESS;
	if (cparams->gbb->flags & GBB_FLAG_DISABLE_EC_SOFTWARE_SYNC)
		return VBERROR_SUCCESS;
	if (sd->recovery_reason)
		return VBERROR_SUCCESS;

	/* Handle updates and jumps for EC */
	VbError_t retval = sync_one_ec(ctx, 0, cparams);
	if (retval != VBERROR_SUCCESS)
		return retval;

#ifdef PD_SYNC
	/* Handle updates and jumps for PD */
	if (!(cparams->gbb->flags & GBB_FLAG_DISABLE_PD_SOFTWARE_SYNC)) {
		retval = sync_one_ec(ctx, 1, cparams);
		if (retval != VBERROR_SUCCESS)
			return retval;
	}
#endif

	return VBERROR_SUCCESS;
}

VbError_t ec_sync_phase3(struct vb2_context *ctx, VbCommonParams *cparams)
{
	VbSharedDataHeader *shared =
		(VbSharedDataHeader *)cparams->shared_data_blob;

	/* EC verification (and possibly updating / jumping) is done */
	VbError_t rv = VbExEcVbootDone(!!shared->recovery_reason);
	if (rv)
		return rv;

	/* Check if we need to cut-off battery. This must be done after EC
         * firmware updating and before kernel started. */
	if (vb2_nv_get(ctx, VB2_NV_BATTERY_CUTOFF_REQUEST)) {
		VB2_DEBUG("Request to cut-off battery\n");
		vb2_nv_set(ctx, VB2_NV_BATTERY_CUTOFF_REQUEST, 0);
		VbExEcBatteryCutOff();
		return VBERROR_SHUTDOWN_REQUESTED;
	}

	return VBERROR_SUCCESS;
}
