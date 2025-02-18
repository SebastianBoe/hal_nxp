/*
 * Copyright 2018 - 2019 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "fsl_prince.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/
/* Component ID definition, used by tools. */

#ifndef FSL_COMPONENT_ID
#define FSL_COMPONENT_ID "platform.drivers.prince"
#endif

/*******************************************************************************
 * Variables
 ******************************************************************************/
const bus_crypto_engine_interface_t *s_busCryptoEngineInterface;

/*******************************************************************************
 * Code
 ******************************************************************************/
static secure_bool_t PRINCE_CheckerAlgorithm(uint32_t address,
                                             uint32_t length,
                                             uint32_t flag,
                                             flash_config_t *flash_context)
{
    uint32_t temp_base = 0, temp_sr = 0, region_index = 0, contiguous_start_index = 0, contiguous_end_index = 32;
    secure_bool_t is_prince_region_contiguous      = kSECURE_TRUE;
    uint8_t prince_iv_code[FLASH_FFR_IV_CODE_SIZE] = {0};

    if (address > 0xA0000u)
    {
        /* If it is not in flash region, return true to allow erase/write operation. */
        return kSECURE_TRUE;
    }

    /* Iterate for all PRINCE regions */
    for (region_index = kPRINCE_Region0; region_index <= kPRINCE_Region2; region_index++)
    {
        contiguous_start_index = 0;
        contiguous_end_index   = 32;
        switch (region_index)
        {
            case kPRINCE_Region0:
                temp_base = PRINCE->BASE_ADDR0;
                temp_sr   = PRINCE->SR_ENABLE0;
                break;

            case kPRINCE_Region1:
                temp_base = PRINCE->BASE_ADDR1;
                temp_sr   = PRINCE->SR_ENABLE1;
                break;

            case kPRINCE_Region2:
                temp_base = PRINCE->BASE_ADDR2;
                temp_sr   = PRINCE->SR_ENABLE2;
                break;
        }

        if (((address >= temp_base) &&
             ((address + length) < (temp_base + (FSL_PRINCE_DRIVER_SUBREGION_SIZE_IN_KB * 32 * 1024)))) &&
            (temp_sr != 0))
        {
            /* Check if the mask is contiguous */
            secure_bool_t first_set_bit_found  = kSECURE_FALSE;
            secure_bool_t contiguous_end_found = kSECURE_FALSE;
            for (int i = 0; i < 32; i++)
            {
                if (temp_sr & (1 << i))
                {
                    if (kSECURE_FALSE == first_set_bit_found)
                    {
                        first_set_bit_found    = kSECURE_TRUE;
                        contiguous_start_index = i;
                    }
                    if (kSECURE_TRUE == contiguous_end_found)
                    {
                        is_prince_region_contiguous = kSECURE_FALSE;
                        break;
                    }
                }
                else
                {
                    if ((kSECURE_TRUE == first_set_bit_found) && (kSECURE_FALSE == contiguous_end_found))
                    {
                        contiguous_end_found = kSECURE_TRUE;
                        contiguous_end_index = i;
                    }
                }
            }
        }
        else
        {
            continue; /* No encryption enabled, continue with the next region checking. */
        }

        /* Check if the provided memory range covers all addresses defined in the SR mask */
        if ((kSECURE_TRUE == is_prince_region_contiguous) &&
            ((address <= (temp_base + (contiguous_start_index * FSL_PRINCE_DRIVER_SUBREGION_SIZE_IN_KB * 1024)))) &&
            (((address + length) >=
              (temp_base + (contiguous_end_index * FSL_PRINCE_DRIVER_SUBREGION_SIZE_IN_KB * 1024)))))
        {
            /* In case of erase operation, invalidate the old PRINCE IV by regenerating the new one */
            if (kPRINCE_Flag_EraseCheck == flag)
            {
                /* Re-generate the PRINCE IV in case of erase operation */

                /* Generate new IV code for the PRINCE region and store the new IV into the respective FFRs */
                if (kStatus_Success ==
                    PRINCE_GenNewIV((prince_region_t)region_index, &prince_iv_code[0], true, flash_context))
                {
                    /* Store the new IV for the PRINCE region into PRINCE registers. */
                    if (kStatus_Success == PRINCE_LoadIV((prince_region_t)region_index, &prince_iv_code[0]))
                    {
                        /* Encryption is enabled, all subregions are to be erased/written at once, IV successfully
                         * regenerated, return true to allow erase operation. */
                        return kSECURE_TRUE;
                    }
                }
                /* Encryption is enabled, all subregions are to be erased/written at once but IV has not been correctly
                 * regenerated, return false to disable erase operation. */
                return kSECURE_FALSE;
            }

            /* Encryption is enabled and all subregions are to be erased/written at once, return true to allow
             * erase/write operation. */
            return kSECURE_TRUE;
        }
        /* The provided memory range does not cover all addresses defined in the SR mask. */
        else
        {
            /* Is the provided memory range outside the addresses defined by the SR mask? */
            if ((kSECURE_TRUE == is_prince_region_contiguous) &&
                ((((address + length) <=
                   (temp_base + (contiguous_start_index * FSL_PRINCE_DRIVER_SUBREGION_SIZE_IN_KB * 1024)))) ||
                 ((address >= (temp_base + (contiguous_end_index * FSL_PRINCE_DRIVER_SUBREGION_SIZE_IN_KB * 1024))))))
            {
                /* No encryption enabled for the provided memory range, true could be returned to allow erase/write
                   operation, but due to the same base address for all three prince regions on Niobe4Mini we should
                   continue with other regions (SR mask) checking. */
                continue;
            }
            else
            {
                /* Encryption is enabled but not all subregions are to be erased/written at once, return false to
                 * disable erase/write operation. */
                return kSECURE_FALSE;
            }
        }
    }
    return kSECURE_TRUE;
}

/*!
 * @brief Generate new IV code.
 *
 * This function generates new IV code and stores it into the persistent memory.
 * This function is implemented as a wrapper of the exported ROM bootloader API.
 * Ensure about 800 bytes free space on the stack when calling this routine with the store parameter set to true!
 *
 * @param region PRINCE region index.
 * @param iv_code IV code pointer used for storing the newly generated 52 bytes long IV code.
 * @param store flag to allow storing the newly generated IV code into the persistent memory (FFR).
 * param flash_context pointer to the flash driver context structure.
 *
 * @return kStatus_Success upon success
 * @return kStatus_Fail    otherwise, kStatus_Fail is also returned if the key code for the particular
 *                         PRINCE region is not present in the keystore (though new IV code has been provided)
 */
status_t PRINCE_GenNewIV(prince_region_t region, uint8_t *iv_code, bool store, flash_config_t *flash_context)
{
    status_t status            = kStatus_Fail;
    s_busCryptoEngineInterface = (const bus_crypto_engine_interface_t *)(*(uint32_t **)0x13000020)[9];

    status = s_busCryptoEngineInterface->bus_crypto_engine_gen_new_iv(
        (uint32_t)region, &iv_code[0], (store == true) ? kSECURE_TRUE : kSECURE_FALSE, flash_context);

    if (kStatus_SKBOOT_Success == status)
    {
        return kStatus_Success;
    }
    else
    {
        return kStatus_Fail;
    }
}

/*!
 * @brief Load IV code.
 *
 * This function enables IV code loading into the PRINCE bus encryption engine.
 * This function is implemented as a wrapper of the exported ROM bootloader API.
 *
 * @param region PRINCE region index.
 * @param iv_code IV code pointer used for passing the IV code.
 *
 * @return kStatus_Success upon success
 * @return kStatus_Fail    otherwise
 */
status_t PRINCE_LoadIV(prince_region_t region, uint8_t *iv_code)
{
    status_t status            = kStatus_Fail;
    s_busCryptoEngineInterface = (const bus_crypto_engine_interface_t *)(*(uint32_t **)0x13000020)[9];

    status = s_busCryptoEngineInterface->bus_crypto_engine_load_iv((uint32_t)region, &iv_code[0]);

    if (kStatus_SKBOOT_Success == status)
    {
        return kStatus_Success;
    }
    else
    {
        return kStatus_Fail;
    }
}

/*!
 * @brief Allow encryption/decryption for specified address range.
 *
 * This function sets the encryption/decryption for specified address range.
 * This function is implemented as a wrapper of the exported ROM bootloader API.
 * Ensure about 800 bytes free space on the stack when calling this routine!
 *
 * @param region PRINCE region index.
 * @param start_address start address of the area to be encrypted/decrypted.
 * @param length length of the area to be encrypted/decrypted.
 * param flash_context pointer to the flash driver context structure.
 *
 * @return kStatus_Success upon success
 * @return kStatus_Fail    otherwise
 */
status_t PRINCE_SetEncryptForAddressRange(prince_region_t region,
                                          uint32_t start_address,
                                          uint32_t length,
                                          flash_config_t *flash_context)
{
    status_t status            = kStatus_Fail;
    s_busCryptoEngineInterface = (const bus_crypto_engine_interface_t *)(*(uint32_t **)0x13000020)[9];

    status = s_busCryptoEngineInterface->bus_crypto_engine_set_encrypt_for_address_range((uint8_t)region, start_address,
                                                                                         length, flash_context);

    if (kStatus_SKBOOT_Success == status)
    {
        return kStatus_Success;
    }
    else
    {
        return kStatus_Fail;
    }
}

/*!
 * @brief Gets the PRINCE Sub-Region Enable register.
 *
 * This function gets PRINCE SR_ENABLE register.
 *
 * @param base PRINCE peripheral address.
 * @param region PRINCE region index.
 * @param sr_enable Sub-Region Enable register pointer.
 *
 * @return kStatus_Success upon success
 * @return kStatus_InvalidArgument
 */
status_t PRINCE_GetRegionSREnable(PRINCE_Type *base, prince_region_t region, uint32_t *sr_enable)
{
    status_t status = kStatus_Success;

    switch (region)
    {
        case kPRINCE_Region0:
            *sr_enable = base->SR_ENABLE0;
            break;

        case kPRINCE_Region1:
            *sr_enable = base->SR_ENABLE1;
            break;

        case kPRINCE_Region2:
            *sr_enable = base->SR_ENABLE2;
            break;

        default:
            status = kStatus_InvalidArgument;
            break;
    }

    return status;
}

/*!
 * @brief Gets the PRINCE region base address register.
 *
 * This function gets PRINCE BASE_ADDR register.
 *
 * @param base PRINCE peripheral address.
 * @param region PRINCE region index.
 * @param region_base_addr Region base address pointer.
 *
 * @return kStatus_Success upon success
 * @return kStatus_InvalidArgument
 */
status_t PRINCE_GetRegionBaseAddress(PRINCE_Type *base, prince_region_t region, uint32_t *region_base_addr)
{
    status_t status = kStatus_Success;

    switch (region)
    {
        case kPRINCE_Region0:
            *region_base_addr = base->BASE_ADDR0;
            break;

        case kPRINCE_Region1:
            *region_base_addr = base->BASE_ADDR1;
            break;

        case kPRINCE_Region2:
            *region_base_addr = base->BASE_ADDR2;
            break;

        default:
            status = kStatus_InvalidArgument;
            break;
    }

    return status;
}

/*!
 * @brief Sets the PRINCE region IV.
 *
 * This function sets specified AES IV for the given region.
 *
 * @param base PRINCE peripheral address.
 * @param region Selection of the PRINCE region to be configured.
 * @param iv 64-bit AES IV in little-endian byte order.
 */
status_t PRINCE_SetRegionIV(PRINCE_Type *base, prince_region_t region, const uint8_t iv[8])
{
    status_t status              = kStatus_Fail;
    volatile uint32_t *IVMsb_reg = NULL;
    volatile uint32_t *IVLsb_reg = NULL;

    switch (region)
    {
        case kPRINCE_Region0:
            IVLsb_reg = &base->IV_LSB0;
            IVMsb_reg = &base->IV_MSB0;
            break;

        case kPRINCE_Region1:
            IVLsb_reg = &base->IV_LSB1;
            IVMsb_reg = &base->IV_MSB1;
            break;

        case kPRINCE_Region2:
            IVLsb_reg = &base->IV_LSB2;
            IVMsb_reg = &base->IV_MSB2;
            break;

        default:
            status = kStatus_InvalidArgument;
            break;
    }

    if (status != kStatus_InvalidArgument)
    {
        *IVLsb_reg = ((uint32_t *)(uintptr_t)iv)[0];
        *IVMsb_reg = ((uint32_t *)(uintptr_t)iv)[1];
        status     = kStatus_Success;
    }

    return status;
}

/*!
 * @brief Sets the PRINCE region base address.
 *
 * This function configures PRINCE region base address.
 *
 * @param base PRINCE peripheral address.
 * @param region Selection of the PRINCE region to be configured.
 * @param region_base_addr Base Address for region.
 */
status_t PRINCE_SetRegionBaseAddress(PRINCE_Type *base, prince_region_t region, uint32_t region_base_addr)
{
    status_t status = kStatus_Success;

    switch (region)
    {
        case kPRINCE_Region0:
            base->BASE_ADDR0 = region_base_addr;
            break;

        case kPRINCE_Region1:
            base->BASE_ADDR1 = region_base_addr;
            break;

        case kPRINCE_Region2:
            base->BASE_ADDR2 = region_base_addr;
            break;

        default:
            status = kStatus_InvalidArgument;
            break;
    }

    return status;
}

/*!
 * @brief Sets the PRINCE Sub-Region Enable register.
 *
 * This function configures PRINCE SR_ENABLE register.
 *
 * @param base PRINCE peripheral address.
 * @param region Selection of the PRINCE region to be configured.
 * @param sr_enable Sub-Region Enable register value.
 */
status_t PRINCE_SetRegionSREnable(PRINCE_Type *base, prince_region_t region, uint32_t sr_enable)
{
    status_t status = kStatus_Success;

    switch (region)
    {
        case kPRINCE_Region0:
            base->SR_ENABLE0 = sr_enable;
            break;

        case kPRINCE_Region1:
            base->SR_ENABLE1 = sr_enable;
            break;

        case kPRINCE_Region2:
            base->SR_ENABLE2 = sr_enable;
            break;

        default:
            status = kStatus_InvalidArgument;
            break;
    }

    return status;
}

/*!
 * @brief Erases the flash sectors encompassed by parameters passed into function.
 *
 * This function erases the appropriate number of flash sectors based on the
 * desired start address and length. It deals with the flash erase function
 * complenentary to the standard erase API of the IAP1 driver. This implementation
 * additionally checks if the whole encrypted PRINCE subregions are erased at once
 * to avoid secrets revealing.
 *
 * @param config The pointer to the storage for the driver runtime state.
 * @param start The start address of the desired flash memory to be erased.
 *              The start address does not need to be sector-aligned.
 * @param lengthInBytes The length, given in bytes (not words or long-words)
 *                      to be erased. Must be word-aligned.
 * @param key The value used to validate all flash erase APIs.
 *
 * @return #kStatus_FLASH_Success API was executed successfully.
 * @return #kStatus_FLASH_InvalidArgument An invalid argument is provided.
 * @return #kStatus_FLASH_AlignmentError The parameter is not aligned with the specified baseline.
 * @return #kStatus_FLASH_AddressError The address is out of range.
 * @return #kStatus_FLASH_EraseKeyError The API erase key is invalid.
 * @return #kStatus_FLASH_CommandFailure Run-time error during the command execution.
 * @return #kStatus_FLASH_CommandNotSupported Flash API is not supported.
 * @return #kStatus_FLASH_EccError A correctable or uncorrectable error during command execution.
 * @return #kStatus_FLASH_EncryptedRegionsEraseNotDoneAtOnce Encrypted flash subregions are not erased at once.
 */
status_t PRINCE_FlashEraseWithChecker(flash_config_t *config, uint32_t start, uint32_t lengthInBytes, uint32_t key)
{
    /* Check that the whole encrypted region is erased at once. */
    if (kSECURE_TRUE != PRINCE_CheckerAlgorithm(start, lengthInBytes, kPRINCE_Flag_EraseCheck, config))
    {
        return kStatus_FLASH_EncryptedRegionsEraseNotDoneAtOnce;
    }
    return FLASH_Erase(config, start, lengthInBytes, key);
}

/*!
 * @brief Programs flash with data at locations passed in through parameters.
 *
 * This function programs the flash memory with the desired data for a given
 * flash area as determined by the start address and the length. It deals with the
 * flash program function complenentary to the standard program API of the IAP1 driver.
 * This implementation additionally checks if the whole PRINCE subregions are
 * programmed at once to avoid secrets revealing.
 *
 * @param config A pointer to the storage for the driver runtime state.
 * @param start The start address of the desired flash memory to be programmed. Must be
 *              word-aligned.
 * @param src A pointer to the source buffer of data that is to be programmed
 *            into the flash.
 * @param lengthInBytes The length, given in bytes (not words or long-words),
 *                      to be programmed. Must be word-aligned.
 *
 * @return #kStatus_FLASH_Success API was executed successfully.
 * @return #kStatus_FLASH_InvalidArgument An invalid argument is provided.
 * @return #kStatus_FLASH_AlignmentError Parameter is not aligned with the specified baseline.
 * @return #kStatus_FLASH_AddressError Address is out of range.
 * @return #kStatus_FLASH_AccessError Invalid instruction codes and out-of bounds addresses.
 * @return #kStatus_FLASH_CommandFailure Run-time error during the command execution.
 * @return #kStatus_FLASH_CommandFailure Run-time error during the command execution.
 * @return #kStatus_FLASH_CommandNotSupported Flash API is not supported.
 * @return #kStatus_FLASH_EccError A correctable or uncorrectable error during command execution.
 * @return #kStatus_FLASH_SizeError Encrypted flash subregions are not programmed at once.
 */
status_t PRINCE_FlashProgramWithChecker(flash_config_t *config, uint32_t start, uint8_t *src, uint32_t lengthInBytes)
{
    /* Check that the whole encrypted subregions will be writen at once. */
    if (kSECURE_TRUE != PRINCE_CheckerAlgorithm(start, lengthInBytes, kPRINCE_Flag_WriteCheck, config))
    {
        return kStatus_FLASH_SizeError;
    }
    return FLASH_Program(config, start, src, lengthInBytes);
}
