// ----------------------------------------------------------------------------
// Copyright 2016-2017 ARM Ltd.
//
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ----------------------------------------------------------------------------

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include "upgrade.h"

#include "update-client-paal/arm_uc_paal_update.h"
#include "active_application.h"
#include "bootloader_common.h"

#include "mbedtls/sha256.h"
#include "mbed.h"

#include <inttypes.h>

#if defined(BOOTLOADER_POWER_CUT_TEST) && (BOOTLOADER_POWER_CUT_TEST == 1)
#include "bootloader_power_cut_test.h"
#endif

#if defined(FIRMWARE_UPDATE_TEST) && (FIRMWARE_UPDATE_TEST == 1)
#include "firmware_update_test.h"
#endif

#ifndef MAX_FIRMWARE_LOCATIONS
#define MAX_FIRMWARE_LOCATIONS             1
#endif

#define INVALID_IMAGE_INDEX          0xFFFFFFFF

/* SHA256 pointer to buffer in the heap */
uint64_t* heapVersion = NULL;

/* pointer to reboot counter in the heap */
uint8_t* bootCounter = NULL;

/**
 * Verify the integrity of stored firmware
 * @detail Read the firmware and compute its hash.
 *         Compare the computed hash with the one given in the header
 *         to verify the firmware integrity
 * @param  headerP
 *             Caller-allocated header structure containing the hash and size
 *             of the firmware.
 * @param  index
 *             Index of firmware to check.
 * @return true if the validation succeeds.
 */
bool checkStoredApplication(uint32_t source,
                            arm_uc_firmware_details_t* details)
{
    tr_debug("checkStoredApplication");

    bool result = false;

    if (details)
    {
#if defined(BOOTLOADER_POWER_CUT_TEST) && (BOOTLOADER_POWER_CUT_TEST == 1)
        power_cut_test_assert_state(POWER_CUT_TEST_STATE_FIRMWARE_VALIDATION);
#endif

        /* setup UCP buffer for reading firmware */
        arm_uc_buffer_t buffer = {
            .size_max = BUFFER_SIZE,
            .size     = 0,
            .ptr      = buffer_array
        };

        /* initialize hashing facility */
        mbedtls_sha256_context mbedtls_ctx;
        mbedtls_sha256_init(&mbedtls_ctx);
        mbedtls_sha256_starts(&mbedtls_ctx, 0);

        /* read full firmware using PAL Update API */
        uint32_t offset = 0;
        while (offset < details->size)
        {
            /* clear most recent UCP event */
            event_callback = CLEAR_EVENT;

            /* set the number of bytes expected */
            buffer.size = (details->size - offset) > buffer.size_max ?
                            buffer.size_max : (details->size - offset);

            /* fill buffer using UCP */
            arm_uc_error_t ucp_status = ARM_UCP_Read(source,
                                                     offset,
                                                     &buffer);

            /* wait for event if the call is accepted */
            if (ucp_status.error == ERR_NONE)
            {
                while (event_callback == CLEAR_EVENT)
                {
                    __WFI();
                }
            }

            /* check status and actual read size */
            if ((event_callback == ARM_UC_PAAL_EVENT_READ_DONE) &&
                (buffer.size > 0))
            {
                /* update hash */
                mbedtls_sha256_update(&mbedtls_ctx, buffer.ptr, buffer.size);

                offset += buffer.size;
            }
            else
            {
                tr_trace("\r\n");
                tr_debug("ARM_UCP_Read returned 0 bytes");
                break;
            }

#if defined(SHOW_PROGRESS_BAR) && SHOW_PROGRESS_BAR == 1
            printProgress(offset, details->size);
#endif
        }

/* make sure buffer is large enough to contain both the SHA and HMAC */
#if BUFFER_SIZE < (2*SIZEOF_SHA256)
#error "BUFFER_SIZE too small to contain SHA and HMAC"
#endif

        /* now we are finished with the buffer we can reuse the underlying buffer */
        arm_uc_buffer_t hash_buffer = {
            .size_max = SIZEOF_SHA256,
            .size     = 0,
            .ptr      = buffer_array
        };

        /* finalize hash */
        mbedtls_sha256_finish(&mbedtls_ctx, hash_buffer.ptr);
        mbedtls_sha256_free(&mbedtls_ctx);
        hash_buffer.size = SIZEOF_SHA256;

        /* compare calculated hash with hash from header */
        int diff = memcmp(details->hash,
                          hash_buffer.ptr,
                          SIZEOF_SHA256);

        if (diff == 0)
        {
            result = true;
        }
        else
        {
            printSHA256(details->hash);
            printSHA256(hash_buffer.ptr);
        }
    }

    return result;
}

/**
 * Find suitable update candidate and copy firmware into active region
 * @return true if the active firmware region is valid.
 */
bool upgradeApplicationFromStorage(void)
{
    /* Track the validity of the active image throughout this function. */
    bool activeFirmwareValid = false;

    /* Find the firmware with the highest version.
       If the active image is corrupt, any replacement will do.
    */
    uint32_t bestStoredFirmwareIndex = INVALID_IMAGE_INDEX;

    arm_uc_firmware_details_t bestStoredFirmwareImageDetails = {
        .version  = 0,
        .size     = 0,
        .hash     = { 0 },
        .campaign = { 0 }
    };

    /* Image details buffer struct */
    arm_uc_firmware_details_t imageDetails = {
        .version  = 0,
        .size     = 0,
        .hash     = { 0 },
        .campaign = { 0 }
    };

    /*************************************************************************/
    /* Step 1. Validate the active application.                              */
    /*************************************************************************/

    tr_info("Active firmware integrity check:");

    int activeApplicationStatus = checkActiveApplication(&imageDetails);

#if (defined(BOOTLOADER_POWER_CUT_TEST) && (BOOTLOADER_POWER_CUT_TEST == 1)) ||\
    (defined(FIRMWARE_UPDATE_TEST) && (FIRMWARE_UPDATE_TEST == 1))
    /* for tests, always copy firmware from sd card
     * hence disable version check by setting the active version to 0.
     */
    imageDetails.version = 0;
#endif

    /* Compare active firmware hash with SHA256 in heap. Because mbed Cloud
       Client uses dynamic memory, if the hash is identical then:
        (1) the bootloader must have already copied it in once and
        (2) the active application failed to initialize correctly.
    */

    /* default to a fresh boot */
    uint8_t localCounter = 0;

    if (heapVersion && bootCounter)
    {
        /* fresh boot */
        if (*heapVersion != imageDetails.version)
        {
            /* copy version to heap */
            *heapVersion = imageDetails.version;

            /* reset boot counter */
            *bootCounter = 0;

            tr_debug("heapVersion: %" PRIu64, *heapVersion);
            tr_debug("bootCounter: %" PRIu8, *bootCounter);
        }
        /* reboot */
        else
        {
            /* increment boot counter*/
            *bootCounter += 1;

            tr_debug("bootCounter: %" PRIu8, *bootCounter);
        }

        /* transfer value */
        localCounter = *bootCounter;
    }

    /* mark active image as valid */
    if ((activeApplicationStatus == RESULT_SUCCESS) &&
        (localCounter < MAX_BOOT_RETRIES))
    {
        printSHA256(imageDetails.hash);
        tr_info("Version: %" PRIu64, imageDetails.version);

        /* mark active firmware as usable */
        activeFirmwareValid = true;

        /* Update version to reflect a valid active image */
        bestStoredFirmwareImageDetails.version = imageDetails.version;
    }
    /* active image is empty */
    else if (activeApplicationStatus == RESULT_EMPTY)
    {
        tr_info("Active firmware slot is empty");
    }
    /* active image cannot be run */
    else if (localCounter >= MAX_BOOT_RETRIES)
    {
        tr_error("Failed to boot active application %d times", MAX_BOOT_RETRIES);
    }
    /* active image failed integrity check */
    else
    {
        tr_error("Active firmware integrity check failed");
    }

    /*************************************************************************/
    /* Step 2. Search all available firmware images for newer firmware or    */
    /*         replacement firmware for corrupted active image.              */
    /*************************************************************************/

    for (uint32_t index = 0; index < MAX_FIRMWARE_LOCATIONS; index++)
    {
        /* clear most recent UCP event */
        event_callback = CLEAR_EVENT;

        /* Check version and checksum first */
        arm_uc_error_t ucp_status = ARM_UCP_GetFirmwareDetails(index,
                                                               &imageDetails);

        /* wait for event if the call is accepted */
        if (ucp_status.error == ERR_NONE)
        {
            while (event_callback == CLEAR_EVENT)
            {
                __WFI();
            }
        }

        /* check event */
        if (event_callback == ARM_UC_PAAL_EVENT_GET_FIRMWARE_DETAILS_DONE)
        {
            /* default to use firmware candidate */
            bool firmwareDifferentFromActive = true;

/* disable duplicate hash check when running test */
#if !defined(FIRMWARE_UPDATE_TEST) || (FIRMWARE_UPDATE_TEST == 0)

            /* compare stored firmware with the currently active one */
            if (heapVersion)
            {
                firmwareDifferentFromActive =
                    (*heapVersion != imageDetails.version);
            }
#endif

            /* Only hash check firmwares with higher version number than the
               active image and with a different hash. This prevents rollbacks
               and hash checks of old images. If the active image is not valid,
               bestStoredFirmwareImageDetails.version equals 0.
            */
            if ((imageDetails.version > bestStoredFirmwareImageDetails.version) &&
                (imageDetails.size > 0) &&
                (firmwareDifferentFromActive || !activeFirmwareValid))
            {
                tr_info("Slot %" PRIu32 " firmware integrity check:",
                        index);

                /* Validate candidate firmware body. */
                bool firmwareValid = checkStoredApplication(index,
                                                            &imageDetails);

                if (firmwareValid)
                {
                    /* Integrity check passed */
                    printSHA256(imageDetails.hash);
                    tr_info("Version: %" PRIu64, imageDetails.version);

                    /* check firmware size fits */
                    if (imageDetails.size <= MBED_CONF_APP_MAX_APPLICATION_SIZE)
                    {
                        /* Update best candidate information */
                        bestStoredFirmwareIndex = index;
                        bestStoredFirmwareImageDetails.version = imageDetails.version;
                        bestStoredFirmwareImageDetails.size = imageDetails.size;
                        memcpy(bestStoredFirmwareImageDetails.hash,
                               imageDetails.hash,
                               ARM_UC_SHA256_SIZE);
                        memcpy(bestStoredFirmwareImageDetails.campaign,
                               imageDetails.campaign,
                               ARM_UC_GUID_SIZE);
                    }
                    else
                    {
                        /* Firmware candidate size too large */
                        tr_error("Slot %" PRIu32 " firmware size too large %"
                                 PRIu32 " > %" PRIu32, index,
                                 (uint32_t) imageDetails.size,
                                 (uint32_t) MBED_CONF_APP_MAX_APPLICATION_SIZE);
                    }
                }
                else
                {
                    /* Integrity check failed */
                    tr_error("Slot %" PRIu32 " firmware integrity check failed",
                             index);
                }
            }
            else
            {
                tr_info("Slot %" PRIu32 " firmware is of older date",
                        index);
                /* do not print HMAC version
                printSHA256(imageDetails.hash);
                */
                tr_info("Version: %" PRIu64, imageDetails.version);
            }
        }
        else
        {
            tr_info("Slot %" PRIu32 " is empty", index);
        }
    }

    /*************************************************************************/
    /* Step 3. Apply new firmware if a suitable candidate was found.         */
    /*************************************************************************/

    /* only replace active image if there is a better candidate */
    if (bestStoredFirmwareIndex != INVALID_IMAGE_INDEX)
    {
        /* if copy fails, retry up to MAX_COPY_RETRIES */
        for (uint32_t retries = 0; retries < MAX_COPY_RETRIES; retries++)
        {
            tr_info("Update active firmware using slot %" PRIu32 ":",
                    bestStoredFirmwareIndex);

            activeFirmwareValid = copyStoredApplication(bestStoredFirmwareIndex,
                                                        &bestStoredFirmwareImageDetails);

            /* if image is valid, break out from loop */
            if (activeFirmwareValid)
            {
                tr_info("New active firmware is valid");
#if defined(FIRMWARE_UPDATE_TEST) && (FIRMWARE_UPDATE_TEST == 1)
                firmware_update_test_validate();
#endif
                break;
            }
            else
            {
                tr_error("Firmware update failed");
            }
        }
    }
    else if (activeFirmwareValid)
    {
        tr_info("Active firmware up-to-date");
    }
    else
    {
        tr_error("Active firmware invalid");
    }

    // return the integrity of the active image
    return activeFirmwareValid;
}
