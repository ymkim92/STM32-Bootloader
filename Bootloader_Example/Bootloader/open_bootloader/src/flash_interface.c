/**
  ******************************************************************************
  * @file    flash_interface.c
  * @author  MCD Application Team
  * @brief   Contains FLASH access functions
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "platform.h"
#include "openbl_mem.h"
#include "openbl_core.h"
#include "app_openbootloader.h"
#include "common_interface.h"
#include "flash_interface.h"
//#include "i2c_interface.h"
#include "optionbytes_interface.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
uint32_t Flash_BusyState = FLASH_BUSY_STATE_DISABLED;
FLASH_ProcessTypeDef FlashProcess = {};

/* Private function prototypes -----------------------------------------------*/
static void OPENBL_FLASH_ProgramDoubleWord(uint32_t Address, uint32_t Data);
static ErrorStatus OPENBL_FLASH_EnableWriteProtection(uint8_t *ListOfPages, uint32_t Length);
static ErrorStatus OPENBL_FLASH_DisableWriteProtection(void);
#if defined (__ICCARM__)
//__ramfunc static HAL_StatusTypeDef OPENBL_FLASH_SendBusyState(uint32_t Timeout);
__ramfunc static HAL_StatusTypeDef OPENBL_FLASH_WaitForLastOperation(uint32_t Timeout);
__ramfunc static HAL_StatusTypeDef OPENBL_FLASH_ExtendedErase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *pPageError);
#else
//__attribute__((section(".ramfunc"))) static HAL_StatusTypeDef OPENBL_FLASH_SendBusyState(uint32_t Timeout);
//__attribute__((section(".ramfunc"))) static HAL_StatusTypeDef OPENBL_FLASH_WaitForLastOperation(uint32_t Timeout);
__attribute__((section(".ramfunc"))) static HAL_StatusTypeDef OPENBL_FLASH_ExtendedErase(
  FLASH_EraseInitTypeDef *pEraseInit, uint32_t *pPageError);
#endif /* (__ICCARM__) */

/* Exported variables --------------------------------------------------------*/
OPENBL_MemoryTypeDef FLASH_Descriptor =
{
  FLASH_START_ADDRESS,
  FLASH_END_ADDRESS,
  FLASH_BL_SIZE,
  FLASH_AREA,
  OPENBL_FLASH_Read,
  OPENBL_FLASH_Write,
  OPENBL_FLASH_SetReadOutProtectionLevel,
  OPENBL_FLASH_SetWriteProtection,
  OPENBL_FLASH_JumpToAddress,
  NULL,
  OPENBL_FLASH_Erase
};

/* Exported functions --------------------------------------------------------*/

/**
  * @brief  Unlock the FLASH control register access.
  * @retval None.
  */
void OPENBL_FLASH_Unlock(void)
{
  HAL_FLASH_Unlock();
}

/**
  * @brief  Lock the FLASH control register access.
  * @retval None.
  */
void OPENBL_FLASH_Lock(void)
{
  HAL_FLASH_Lock();
}

/**
  * @brief  Unlock the FLASH Option Bytes Registers access.
  * @retval None.
  */
void OPENBL_FLASH_OB_Unlock(void)
{
  HAL_FLASH_Unlock();

  HAL_FLASH_OB_Unlock();
}

/**
  * @brief  This function is used to read data from a given address.
  * @param  Address The address to be read.
  * @retval Returns the read value.
  */
uint8_t OPENBL_FLASH_Read(uint32_t Address)
{
  return (*(uint8_t *)(Address));
}

/**
  * @brief  This function is used to write data in FLASH memory.
  * @param  Address The address where that data will be written.
  * @param  Data The data to be written.
  * @param  DataLength The length of the data to be written.
  * @retval None.
  */
void OPENBL_FLASH_Write(uint32_t Address, uint8_t *Data, uint32_t DataLength)
{
  uint32_t index;
  uint32_t length = DataLength;
  uint32_t remainder;
  uint8_t remainder_data[8] = {0x0};

  /* Check the remaining of double-word */
  remainder = length & 0x7U;

  if (remainder)
  {
    length = (length & 0xFFFFFFF8U);

    /* copy the remaining bytes */
    for (index = 0U; index < remainder; index++)
    {
      remainder_data[index] = *(Data + length + index);
    }

    /* fill the upper bytes with 0xFF */
    for (index = remainder; index < 8U; index++)
    {
      remainder_data[index] = 0xFF;
    }
  }

  /* Unlock the flash memory for write operation */
  OPENBL_FLASH_Unlock();

  EraseFlash(Address, DataLength);

  for (index = 0U; index < length; (index += 8U))
  {
    OPENBL_FLASH_ProgramDoubleWord((Address + index), (uint32_t)((Data + index)));
  }

  if (remainder)
  {
    OPENBL_FLASH_ProgramDoubleWord((Address + length), (uint32_t)((remainder_data)));
  }

  /* Lock the Flash to disable the flash control register access */
  OPENBL_FLASH_Lock();
}

/**
  * @brief  This function is used to jump to a given address.
  * @param  Address The address where the function will jump.
  * @retval None.
  */
void OPENBL_FLASH_JumpToAddress(uint32_t Address)
{
  Function_Pointer jump_to_address;

  /* De-initialize all HW resources used by the Open Bootloader to their reset values */
  OPENBL_DeInit();

  /* Enable IRQ */
  Common_EnableIrq();

  jump_to_address = (Function_Pointer)(*(__IO uint32_t *)(Address + 4U));

  /* Initialize user application's stack pointer */
  Common_SetMsp(*(__IO uint32_t *) Address);

  jump_to_address();
}

/**
  * @brief  Return the FLASH Read Protection level.
  * @retval The return value can be one of the following values:
  *         @arg OB_RDP_LEVEL_0: No protection
  *         @arg OB_RDP_LEVEL_1: Read protection of the memory
  *         @arg OB_RDP_LEVEL_2: Full chip protection
  */
uint32_t OPENBL_FLASH_GetReadOutProtectionLevel(void)
{
  FLASH_OBProgramInitTypeDef flash_ob;

  /* Get the Option bytes configuration */
  HAL_FLASHEx_OBGetConfig(&flash_ob);

  return flash_ob.RDPLevel;
}

/**
  * @brief  Return the FLASH Read Protection level.
  * @param  Level Can be one of these values:
  *         @arg OB_RDP_LEVEL_0: No protection
  *         @arg OB_RDP_LEVEL_1: Read protection of the memory
  *         @arg OB_RDP_LEVEL_2: Full chip protection
  * @retval None.
  */
void OPENBL_FLASH_SetReadOutProtectionLevel(uint32_t Level)
{
  FLASH_OBProgramInitTypeDef flash_ob;

  if (Level != OB_RDP_LEVEL2)
  {
    flash_ob.OptionType = OPTIONBYTE_RDP;
    flash_ob.RDPLevel   = Level;

    /* Unlock the FLASH registers & Option Bytes registers access */
    OPENBL_FLASH_OB_Unlock();

    /* Change the RDP level */
    HAL_FLASHEx_OBProgram(&flash_ob);
  }

  /* Register system reset callback */
  Common_SetPostProcessingCallback(OPENBL_OB_Launch);
}

/**
  * @brief  This function is used to enable or disable write protection of the specified FLASH areas.
  * @param  State Can be one of these values:
  *         @arg DISABLE: Disable FLASH write protection
  *         @arg ENABLE: Enable FLASH write protection
  * @param  ListOfPages Contains the list of pages to be protected.
  * @param  Length The length of the list of pages to be protected.
  * @retval An ErrorStatus enumeration value:
  *          - SUCCESS: Enable or disable of the write protection is done
  *          - ERROR:   Enable or disable of the write protection is not done
  */
ErrorStatus OPENBL_FLASH_SetWriteProtection(FunctionalState State, uint8_t *ListOfPages, uint32_t Length)
{
  ErrorStatus status = SUCCESS;

  if (State == ENABLE)
  {
    OPENBL_FLASH_EnableWriteProtection(ListOfPages, Length);

    /* Register system reset callback */
    Common_SetPostProcessingCallback(OPENBL_OB_Launch);
  }
  else if (State == DISABLE)
  {
    OPENBL_FLASH_DisableWriteProtection();

    /* Register system reset callback */
    Common_SetPostProcessingCallback(OPENBL_OB_Launch);
  }
  else
  {
    status = ERROR;
  }

  return status;
}

/**
  * @brief  This function is used to start FLASH mass erase operation.
  * @param  *p_Data Pointer to the buffer that contains mass erase operation options.
  * @param  DataLength Size of the Data buffer.
  * @retval An ErrorStatus enumeration value:
  *          - SUCCESS: Mass erase operation done
  *          - ERROR:   Mass erase operation failed or the value of one parameter is not OK
  */
ErrorStatus OPENBL_FLASH_MassErase(uint8_t *p_Data, uint32_t DataLength)
{
  uint32_t page_error;
  uint16_t bank_option;
  ErrorStatus status   = SUCCESS;
  FLASH_EraseInitTypeDef erase_init_struct;

  /* Unlock the flash memory for erase operation */
  OPENBL_FLASH_Unlock();

  erase_init_struct.TypeErase = FLASH_TYPEERASE_MASSERASE;

  if (DataLength >= 2)
  {
    bank_option = *(uint16_t *)(p_Data);

    if (bank_option == FLASH_MASS_ERASE)
    {
      erase_init_struct.Banks = 0U;
    }
    else if (bank_option == FLASH_BANK1_ERASE)
    {
      erase_init_struct.Banks = FLASH_BANK_1;
    }
    else if (bank_option == FLASH_BANK2_ERASE)
    {
      erase_init_struct.Banks = FLASH_BANK_2;
    }
    else
    {
      status = ERROR;
    }

    if (status == SUCCESS)
    {
      if (OPENBL_FLASH_ExtendedErase(&erase_init_struct, &page_error) != HAL_OK)
      {
        status = ERROR;
      }
      else
      {
        status = SUCCESS;
      }
    }
  }
  else
  {
    status = ERROR;
  }

  /* Lock the Flash to disable the flash control register access */
  OPENBL_FLASH_Lock();

  return status;
}

/**
  * @brief  This function is used to erase the specified FLASH pages.
  * @param  *p_Data Pointer to the buffer that contains erase operation options.
  * @param  DataLength Size of the Data buffer.
  * @retval An ErrorStatus enumeration value:
  *          - SUCCESS: Erase operation done
  *          - ERROR:   Erase operation failed or the value of one parameter is not OK
  */
ErrorStatus OPENBL_FLASH_Erase(uint32_t Address, uint8_t *p_Data, uint32_t DataLength)
{
      FLASH_EraseInitTypeDef EraseInitStruct;
    uint32_t SectorError = 0;
    HAL_StatusTypeDef status;

  /* Unlock the flash memory for erase operation */
  OPENBL_FLASH_Unlock();

  /* Clear error programming flags */
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);


    // Fill EraseInit structure
    EraseInitStruct.TypeErase = FLASH_TYPEERASE_SECTORS;
    EraseInitStruct.VoltageRange = FLASH_VOLTAGE_RANGE_3; // Choose voltage range according to your requirements
    EraseInitStruct.Sector = FLASH_SECTOR_5;             // Start sector to erase
//    EraseInitStruct.NbSectors = (DataLength / FLASH_SECTOR_SIZE) + 1; // Number of sectors to erase

    // Perform the erase operation
    // status = HAL_FLASHEx_Erase(&EraseInitStruct, &SectorError);

    // Lock the Flash to disable the flash control register access
    HAL_FLASH_Lock();

    // Check if erase operation was successful
    if (status != HAL_OK)
    {
        // If the erase operation failed, return an error
        return ERROR;
    }

    // If the erase operation was successful, return success
    return SUCCESS;














//  uint32_t counter;
//  uint32_t pages_number;
//  uint32_t page_error;
//  uint32_t errors = 0U;
//  ErrorStatus status = SUCCESS;
////  FLASH_EraseInitTypeDef erase_init_struct;
//
////  pages_number  = (uint32_t)(*(uint16_t *)(p_Data));
//  p_Data       += 2;

//  erase_init_struct.TypeErase = FLASH_TYPEERASE_PAGES;
//  erase_init_struct.NbPages   = 1U;
//
//  for (counter = 0U; ((counter < pages_number) && (counter < (DataLength / 2U))) ; counter++)
//  {
//    erase_init_struct.Page = ((uint32_t)(*(uint16_t *)(p_Data)));
//
//    if (erase_init_struct.Page <= 127)
//    {
//      erase_init_struct.Banks = FLASH_BANK_1;
//    }
//    else if (erase_init_struct.Page <= 255)
//    {
//      erase_init_struct.Banks = FLASH_BANK_2;
//    }
//    else
//    {
//      status = ERROR;
//    }
//
//    if (status != ERROR)
//    {
//      if (OPENBL_FLASH_ExtendedErase(&erase_init_struct, &page_error) != HAL_OK)
//      {
//        errors++;
//      }
//    }
//    else
//    {
//      /* Reset the status for next erase operation */
//      status = SUCCESS;
//    }
//
//    p_Data += 2;
//  }

//  if (errors > 0)
//  {
//    status = ERROR;
//  }
//  else
//  {
//    status = SUCCESS;
//  }
//
//  /* Lock the Flash to disable the flash control register access */
//  OPENBL_FLASH_Lock();
//
//  return status;
}

/**
 * @brief  This function is used to Set Flash busy state variable to activate busy state sending
 *         during flash operations
 * @retval None.
*/
void OPENBL_Enable_BusyState_Flag(void)
{
  /* Enable Flash busy state sending */
  Flash_BusyState = FLASH_BUSY_STATE_ENABLED;
}

/**
 * @brief  This function is used to disable the send of busy state in I2C non stretch mode.
 * @retval None.
*/
void OPENBL_Disable_BusyState_Flag(void)
{
  /* Disable Flash busy state sending */
  Flash_BusyState = FLASH_BUSY_STATE_DISABLED;
}

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Program double word at a specified FLASH address.
  * @param  Address specifies the address to be programmed.
  * @param  Data specifies the data to be programmed.
  * @retval None.
  */
static void OPENBL_FLASH_ProgramDoubleWord(uint32_t Address, uint32_t Data)
{
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, Address, Data);
}

/**
  * @brief  This function is used to enable write protection of the specified FLASH areas.
  * @param  ListOfPages Contains the list of pages to be protected.
  * @param  Length The length of the list of pages to be protected.
  * @retval An ErrorStatus enumeration value:
  *          - SUCCESS: Enable or disable of the write protection is done
  *          - ERROR:   Enable or disable of the write protection is not done
  */
static ErrorStatus OPENBL_FLASH_EnableWriteProtection(uint8_t *ListOfPages, uint32_t Length)
{
  ErrorStatus status       = SUCCESS;
  return status;
}

/**
  * @brief  This function is used to disable write protection.
  * @retval An ErrorStatus enumeration value:
  *          - SUCCESS: Enable or disable of the write protection is done
  *          - ERROR:   Enable or disable of the write protection is not done
  */
static ErrorStatus OPENBL_FLASH_DisableWriteProtection(void)
{
  ErrorStatus status       = SUCCESS;
  return status;
}

/**
  * @brief  Wait for a FLASH operation to complete.
  * @param  Timeout maximum flash operation timeout.
  * @retval HAL_Status
  */
//#if defined (__ICCARM__)
//__ramfunc static HAL_StatusTypeDef OPENBL_FLASH_WaitForLastOperation(uint32_t Timeout)
//#else
//__attribute__((section(".ramfunc"))) static HAL_StatusTypeDef OPENBL_FLASH_WaitForLastOperation(uint32_t Timeout)
//#endif /* (__ICCARM__) */
//{
//	return HAL_OK;
//}

/**
  * @brief  Perform a mass erase or erase the specified FLASH memory pages.
  * @param[in]  pEraseInit pointer to an FLASH_EraseInitTypeDef structure that
  *         contains the configuration information for the erasing.
  * @param[out]  PageError pointer to variable that contains the configuration
  *         information on faulty page in case of error (0xFFFFFFFF means that all
  *         the pages have been correctly erased).
  * @retval HAL_Status
  */
#if defined (__ICCARM__)
__ramfunc static HAL_StatusTypeDef OPENBL_FLASH_ExtendedErase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError)
#else
__attribute__((section(".ramfunc"))) static HAL_StatusTypeDef OPENBL_FLASH_ExtendedErase(
  FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError)
#endif /* (__ICCARM__) */
{
	return HAL_OK;
//  HAL_StatusTypeDef status;
//  uint32_t errors = 0U;
//  __IO uint32_t *reg_cr;
//#if defined (__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 3U)
//  uint32_t primask_bit;
//#endif
//
//  /* Process Locked */
//  __HAL_LOCK(&FlashProcess);
//
//  /* Reset error code */
//  FlashProcess.ErrorCode = HAL_FLASH_ERROR_NONE;
//
//  /* Verify that next operation can be proceed */
//  status = OPENBL_FLASH_WaitForLastOperation(PROGRAM_TIMEOUT);
//
//  if (status == HAL_OK)
//  {
//    FlashProcess.ProcedureOnGoing = pEraseInit->TypeErase;
//
//    /* Access to SECCR or NSCR depends on operation type */
//    reg_cr = IS_FLASH_SECURE_OPERATION() ? &(FLASH->SECCR) : &(FLASH->NSCR);
//
//    /*Initialization of PageError variable*/
//    *PageError = 0xFFFFFFFFU;
//
//    /* Access to SECCR or NSCR registers depends on operation type */
//    reg_cr = IS_FLASH_SECURE_OPERATION() ? &(FLASH->SECCR) : &(FLASH_NS->NSCR);
//
//    if (((pEraseInit->Banks) & FLASH_BANK_1) != 0U)
//    {
//      CLEAR_BIT((*reg_cr), FLASH_NSCR_BKER);
//    }
//    else
//    {
//      SET_BIT((*reg_cr), FLASH_NSCR_BKER);
//    }
//
//    /* Proceed to erase the page */
//    MODIFY_REG((*reg_cr), (FLASH_NSCR_PNB | FLASH_NSCR_PER | FLASH_NSCR_STRT),
//               (((pEraseInit->Page) << FLASH_NSCR_PNB_Pos) | FLASH_NSCR_PER | FLASH_NSCR_STRT));
//
//    if (Flash_BusyState == FLASH_BUSY_STATE_ENABLED)
//    {
//      /* Wait for last operation to be completed to send busy byte */
//      if (OPENBL_FLASH_SendBusyState(PROGRAM_TIMEOUT) != HAL_OK)
//      {
//        errors++;
//      }
//    }
//    else
//    {
//      /* Wait for last operation to be completed */
//      if (OPENBL_FLASH_WaitForLastOperation(PROGRAM_TIMEOUT) != HAL_OK)
//      {
//        errors++;
//      }
//    }
//
//    /* If the erase operation is completed, disable the associated bits */
//    CLEAR_BIT((*reg_cr), (pEraseInit->TypeErase) & (~(FLASH_NON_SECURE_MASK)));
//  }
//
//  /* Process Unlocked */
//  __HAL_UNLOCK(&FlashProcess);
//
//  if (errors > 0)
//  {
//    status = HAL_ERROR;
//  }
//  else
//  {
//    status = HAL_OK;
//  }
//
//  return status;
}

// It assume it uses 2 Mbytes single bank Flash
static ErrorStatus EraseFlash(uint32_t Address, uint32_t DataLength)
{
  FLASH_EraseInitTypeDef EraseInitStruct;
  uint32_t SectorError = 0;
  HAL_StatusTypeDef status;

  /* Unlock the flash memory for erase operation */
  OPENBL_FLASH_Unlock();

  /* Clear error programming flags */
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

  // Fill EraseInit structure
  EraseInitStruct.TypeErase = FLASH_TYPEERASE_SECTORS;
  EraseInitStruct.VoltageRange =
      FLASH_VOLTAGE_RANGE_3; // Choose voltage range according to your
                             // requirements
  EraseInitStruct.Sector = FLASH_SECTOR_5; // Start sector to erase
  //    EraseInitStruct.NbSectors = (DataLength / FLASH_SECTOR_SIZE) + 1; //
  //    Number of sectors to erase

  // Perform the erase operation
  // status = HAL_FLASHEx_Erase(&EraseInitStruct, &SectorError);

  // Lock the Flash to disable the flash control register access
  HAL_FLASH_Lock();

  // Check if erase operation was successful
  if (status != HAL_OK) {
    // If the erase operation failed, return an error
    return ERROR;
  }

  // If the erase operation was successful, return success
  return SUCCESS;
}