/*
 * This file is part of the coreboot project.
 *
 * Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Copyright 2017 Facebook Inc.
 * Copyright 2018 Siemens AG
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <console/cbmem_console.h>
#include <console/console.h>
#include <security/tpm/tspi.h>
#include <security/tpm/tss.h>
#include <stdlib.h>
#if CONFIG(VBOOT)
#include <vb2_api.h>
#include <vb2_sha.h>
#include <assert.h>
#endif

#if CONFIG(TPM1)
static uint32_t tpm1_invoke_state_machine(void)
{
	uint8_t disabled;
	uint8_t deactivated;
	uint32_t result = TPM_SUCCESS;

	/* Check that the TPM is enabled and activated. */
	result = tlcl_get_flags(&disabled, &deactivated, NULL);
	if (result != TPM_SUCCESS) {
		printk(BIOS_ERR, "TPM: Can't read capabilities.\n");
		return result;
	}

	if (disabled) {
		printk(BIOS_INFO, "TPM: is disabled. Enabling...\n");

		result = tlcl_set_enable();
		if (result != TPM_SUCCESS) {
			printk(BIOS_ERR, "TPM: Can't set enabled state.\n");
			return result;
		}
	}

	if (!!deactivated != CONFIG(TPM_DEACTIVATE)) {
		printk(BIOS_INFO,
		       "TPM: Unexpected TPM deactivated state. Toggling...\n");
		result = tlcl_set_deactivated(!deactivated);
		if (result != TPM_SUCCESS) {
			printk(BIOS_ERR,
			       "TPM: Can't toggle deactivated state.\n");
			return result;
		}

		deactivated = !deactivated;
		result = TPM_E_MUST_REBOOT;
	}

	return result;
}
#endif

static uint32_t tpm_setup_s3_helper(void)
{
	uint32_t result;

	result = tlcl_resume();
	switch (result) {
	case TPM_SUCCESS:
		break;

	case TPM_E_INVALID_POSTINIT:
		/*
		 * We're on a platform where the TPM maintains power
		 * in S3, so it's already initialized.
		 */
		printk(BIOS_INFO, "TPM: Already initialized.\n");
		result = TPM_SUCCESS;
		break;

	default:
		printk(BIOS_ERR, "TPM: Resume failed (%#x).\n", result);
		break;
	}

	return result;
}

static uint32_t tpm_setup_epilogue(uint32_t result)
{
	if (result != TPM_SUCCESS)
		post_code(POST_TPM_FAILURE);
	else
		printk(BIOS_INFO, "TPM: setup succeeded\n");

	return result;
}

/*
 * tpm_setup starts the TPM and establishes the root of trust for the
 * anti-rollback mechanism.  tpm_setup can fail for three reasons.  1 A bug.
 * 2 a TPM hardware failure. 3 An unexpected TPM state due to some attack.  In
 * general we cannot easily distinguish the kind of failure, so our strategy is
 * to reboot in recovery mode in all cases.  The recovery mode calls tpm_setup
 * again, which executes (almost) the same sequence of operations.  There is a
 * good chance that, if recovery mode was entered because of a TPM failure, the
 * failure will repeat itself.  (In general this is impossible to guarantee
 * because we have no way of creating the exact TPM initial state at the
 * previous boot.)  In recovery mode, we ignore the failure and continue, thus
 * giving the recovery kernel a chance to fix things (that's why we don't set
 * bGlobalLock).  The choice is between a knowingly insecure device and a
 * bricked device.
 *
 * As a side note, observe that we go through considerable hoops to avoid using
 * the STCLEAR permissions for the index spaces.  We do this to avoid writing
 * to the TPM flashram at every reboot or wake-up, because of concerns about
 * the durability of the NVRAM.
 */
uint32_t tpm_setup(int s3flag)
{
	uint32_t result;

	result = tlcl_lib_init();
	if (result != TPM_SUCCESS) {
		printk(BIOS_ERR, "TPM: Can't initialize.\n");
		return tpm_setup_epilogue(result);
	}

	/* Handle special init for S3 resume path */
	if (s3flag) {
		printk(BIOS_INFO, "TPM: Handle S3 resume.\n");
		return tpm_setup_epilogue(tpm_setup_s3_helper());
	}

	result = tlcl_startup();
	if (CONFIG(TPM_STARTUP_IGNORE_POSTINIT)
	    && result == TPM_E_INVALID_POSTINIT) {
		printk(BIOS_DEBUG, "TPM: ignoring invalid POSTINIT\n");
		result = TPM_SUCCESS;
	}
	if (result != TPM_SUCCESS) {
		printk(BIOS_ERR, "TPM: Can't run startup command.\n");
		return tpm_setup_epilogue(result);
	}

	result = tlcl_assert_physical_presence();
	if (result != TPM_SUCCESS) {
		/*
		 * It is possible that the TPM was delivered with the physical
		 * presence command disabled.  This tries enabling it, then
		 * tries asserting PP again.
		 */
		result = tlcl_physical_presence_cmd_enable();
		if (result != TPM_SUCCESS) {
			printk(BIOS_ERR, "TPM: Can't enable physical presence command.\n");
			return tpm_setup_epilogue(result);
		}

		result = tlcl_assert_physical_presence();
		if (result != TPM_SUCCESS) {
			printk(BIOS_ERR, "TPM: Can't assert physical presence.\n");
			return tpm_setup_epilogue(result);
		}
	}

#if CONFIG(TPM1)
	result = tpm1_invoke_state_machine();
#endif

	return tpm_setup_epilogue(result);
}

uint32_t tpm_clear_and_reenable(void)
{
	uint32_t result;

	printk(BIOS_INFO, "TPM: Clear and re-enable\n");
	result = tlcl_force_clear();
	if (result != TPM_SUCCESS) {
		printk(BIOS_ERR, "TPM: Can't initiate a force clear.\n");
		return result;
	}

#if CONFIG(TPM1)
	result = tlcl_set_enable();
	if (result != TPM_SUCCESS) {
		printk(BIOS_ERR, "TPM: Can't set enabled state.\n");
		return result;
	}

	result = tlcl_set_deactivated(0);
	if (result != TPM_SUCCESS) {
		printk(BIOS_ERR, "TPM: Can't set deactivated state.\n");
		return result;
	}
#endif

	return TPM_SUCCESS;
}

uint32_t tpm_extend_pcr(int pcr, enum vb2_hash_algorithm digest_algo,
			uint8_t *digest, size_t digest_len, const char *name)
{
	uint32_t result;
	uint16_t algorithm = 0;

	if (!digest)
		return TPM_E_IOERROR;

#if CONFIG(TPM2)
	switch (digest_algo) {
	case VB2_HASH_SHA1:
		algorithm = TPM_ALG_SHA1;
		break;
	case VB2_HASH_SHA256:
		algorithm = TPM_ALG_SHA256;
		break;
	case VB2_HASH_SHA512:
		algorithm = TPM_ALG_SHA512;
		break;
	default:
		return TPM_E_HASH_ERROR;
	}
#endif

	result = tlcl_extend(pcr, algorithm, digest, digest_len, NULL);
	if (result != TPM_SUCCESS)
		return result;

	if (CONFIG(VBOOT_MEASURED_BOOT))
		tcpa_log_add_table_entry(name, pcr, digest_algo,
			digest, digest_len);

	return TPM_SUCCESS;
}

#if CONFIG(VBOOT)
uint32_t tpm_measure_region(const struct region_device *rdev, uint8_t pcr,
			    const char *rname)
{
	uint8_t digest[TPM_PCR_MAX_LEN], digest_len;
	uint8_t buf[HASH_DATA_CHUNK_SIZE];
	uint32_t result, offset;
	size_t len;
	struct vb2_digest_context ctx;
	enum vb2_hash_algorithm hash_alg;

	if (!rdev || !rname)
		return TPM_E_INVALID_ARG;
	result = tlcl_lib_init();
	if (result != TPM_SUCCESS) {
		printk(BIOS_ERR, "TPM: Can't initialize library.\n");
		return result;
	}
	if (CONFIG(TPM1)) {
		hash_alg = VB2_HASH_SHA1;
	} else { /* CONFIG_TPM2 */
		hash_alg = VB2_HASH_SHA256;
	}

	digest_len = vb2_digest_size(hash_alg);
	assert(digest_len <= sizeof(digest));
	if (vb2_digest_init(&ctx, hash_alg)) {
		printk(BIOS_ERR, "TPM: Error initializing hash.\n");
		return TPM_E_HASH_ERROR;
	}
	/*
	 * Though one can mmap the full needed region on x86 this is not the
	 * case for e.g. ARM. In order to make this code as universal as
	 * possible across different platforms read the data to hash in chunks.
	 */
	for (offset = 0; offset < region_device_sz(rdev); offset += len) {
		len = MIN(sizeof(buf), region_device_sz(rdev) - offset);
		if (rdev_readat(rdev, buf, offset, len) < 0) {
			printk(BIOS_ERR, "TPM: Not able to read region %s.\n",
			       rname);
			return TPM_E_READ_FAILURE;
		}
		if (vb2_digest_extend(&ctx, buf, len)) {
			printk(BIOS_ERR, "TPM: Error extending hash.\n");
			return TPM_E_HASH_ERROR;
		}
	}
	if (vb2_digest_finalize(&ctx, digest, digest_len)) {
		printk(BIOS_ERR, "TPM: Error finalizing hash.\n");
		return TPM_E_HASH_ERROR;
	}
	result = tpm_extend_pcr(pcr, hash_alg, digest, digest_len, rname);
	if (result != TPM_SUCCESS) {
		printk(BIOS_ERR, "TPM: Extending hash into PCR failed.\n");
		return result;
	}
	printk(BIOS_DEBUG, "TPM: Measured %s into PCR %d\n", rname, pcr);
	return TPM_SUCCESS;
}
#endif /* VBOOT */
