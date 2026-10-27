/*******************************************************************************
 Copyright 2016, STMicroelectronics International N.V.
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright
 notice, this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright
 notice, this list of conditions and the following disclaimer in the
 documentation and/or other materials provided with the distribution.
 * Neither the name of STMicroelectronics nor the
 names of its contributors may be used to endorse or promote products
 derived from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND
 NON-INFRINGEMENT OF INTELLECTUAL PROPERTY RIGHTS ARE DISCLAIMED.
 IN NO EVENT SHALL STMICROELECTRONICS INTERNATIONAL N.V. BE LIABLE FOR ANY
 DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ******************************************************************************/

VL53L0X_Error VL53L0X_load_tuning_settings(VL53L0X_DEV Dev,
                                           uint8_t* pTuningSettingBuffer) {
  VL53L0X_Error Status = VL53L0X_ERROR_NONE;
  int i;
  int Index;
  uint8_t msb;
  uint8_t lsb;
  uint8_t SelectParam;
  uint8_t NumberOfWrites;
  uint8_t Address;
  uint8_t localBuffer[4]; /* max */
  uint16_t Temp16;

  LOG_FUNCTION_START("");

  Index = 0;

  while ((*(pTuningSettingBuffer + Index) != 0) &&
         (Status == VL53L0X_ERROR_NONE)) {
    NumberOfWrites = *(pTuningSettingBuffer + Index);
    Index++;
    if (NumberOfWrites == 0xFF) {
      /* internal parameters */
      SelectParam = *(pTuningSettingBuffer + Index);
      Index++;
      switch (SelectParam) {
        case 0: /* uint16_t SigmaEstRefArray -> 2 bytes */
          msb = *(pTuningSettingBuffer + Index);
          Index++;
          lsb = *(pTuningSettingBuffer + Index);
          Index++;
          Temp16 = VL53L0X_MAKEUINT16(lsb, msb);
          PALDevDataSet(Dev, SigmaEstRefArray, Temp16);
          break;
        case 1: /* uint16_t SigmaEstEffPulseWidth -> 2 bytes */
          msb = *(pTuningSettingBuffer + Index);
          Index++;
          lsb = *(pTuningSettingBuffer + Index);
          Index++;
          Temp16 = VL53L0X_MAKEUINT16(lsb, msb);
          PALDevDataSet(Dev, SigmaEstEffPulseWidth, Temp16);
          break;
        case 2: /* uint16_t SigmaEstEffAmbWidth -> 2 bytes */
          msb = *(pTuningSettingBuffer + Index);
          Index++;
          lsb = *(pTuningSettingBuffer + Index);
          Index++;
          Temp16 = VL53L0X_MAKEUINT16(lsb, msb);
          PALDevDataSet(Dev, SigmaEstEffAmbWidth, Temp16);
          break;
        case 3: /* uint16_t targetRefRate -> 2 bytes */
          msb = *(pTuningSettingBuffer + Index);
          Index++;
          lsb = *(pTuningSettingBuffer + Index);
          Index++;
          Temp16 = VL53L0X_MAKEUINT16(lsb, msb);
          PALDevDataSet(Dev, targetRefRate, Temp16);
          break;
        default: /* invalid parameter */
          Status = VL53L0X_ERROR_INVALID_PARAMS;
      }

    } else if (NumberOfWrites <= 4) {
      Address = *(pTuningSettingBuffer + Index);
      Index++;

      for (i = 0; i < NumberOfWrites; i++) {
        localBuffer[i] = *(pTuningSettingBuffer + Index);
        Index++;
      }

      Status = VL53L0X_WriteMulti(Dev, Address, localBuffer, NumberOfWrites);

    } else {
      Status = VL53L0X_ERROR_INVALID_PARAMS;
    }
  }

  LOG_FUNCTION_END(Status);
  return Status;
}
