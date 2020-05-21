/*
 * UATbridge(.ino) firmware
 * Copyright (C) 2019-2020 Linar Yusupov
 *
 * Author: Linar Yusupov, linar.r.yusupov@gmail.com
 *
 * Web: http://github.com/lyusupov/SoftRF
 *
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "SoCHelper.h"
#include "TimeHelper.h"
#include "LEDHelper.h"
#include "GNSSHelper.h"
#include "RFHelper.h"
#include "EEPROMHelper.h"
#include "BaroHelper.h"
#include "TrafficHelper.h"
#include "BatteryHelper.h"

#include "EasyLink.h"
#include "Protocol_UAT978.h"

#include <uat.h>
#include <fec/char.h>
#include <fec.h>
#include <uat_decode.h>

//#define DEBUG_UAT

#define isValidFix()      isValidGNSSFix()

EasyLink_RxPacket rxPacket;
EasyLink myLink;

ufo_t ThisAircraft;

hardware_info_t hw_info = {
  .model    = DEFAULT_SOFTRF_MODEL,
  .revision = 0,
  .soc      = SOC_NONE,
  .rf       = RF_IC_NONE,
  .gnss     = GNSS_MODULE_NONE,
  .baro     = BARO_MODULE_NONE,
  .display  = DISPLAY_NONE
};

Stratux_frame_t LPUATRadio_frame = {
  .magic1     = STRATUX_UATRADIO_MAGIC_1,
  .magic2     = STRATUX_UATRADIO_MAGIC_2,
  .magic3     = STRATUX_UATRADIO_MAGIC_3,
  .magic4     = STRATUX_UATRADIO_MAGIC_4,

  .msgLen     = LONG_FRAME_BYTES,
  .rssi       = 0,
  .timestamp  = 0UL,
};

#if defined(DEBUG_UAT)
#include <xdc/std.h>

#include <xdc/runtime/System.h>
#include <xdc/runtime/Memory.h>
#include <xdc/runtime/Types.h>

#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/utils/Load.h>
#include <ti/sysbios/hal/Hwi.h>

static void printUtilization()
{
    xdc_UInt i;
    Memory_Stats memStat;
    Hwi_StackInfo hwiStackStat;
    Load_Stat loadStat;
    Task_Handle tsk;
    float idleLoad;
    uint32_t idleLoadInt, idleLoadFrac;

    /* collect current stats */
    Load_update();

    /* use time NOT spent in idle task for Total CPU Load */
    Load_getTaskLoad(Task_getIdleTask(), &loadStat);
    idleLoad = 100.0 - 100.0*(float)loadStat.threadTime/(float)loadStat.totalTime;
    idleLoadInt = idleLoad;
    idleLoadFrac = 10.0*idleLoad - 10.0*idleLoadInt;

    Serial.write("Total CPU Load: ");
    Serial.print(idleLoadInt);
    Serial.print(".");
    Serial.println(idleLoadFrac);
    Serial.println("");
#if 0
    /* collect stats on all statically Created tasks */
    Serial.println("Task info:");
    for (i = 0; i < Task_Object_count(); i++) {
        tsk = Task_Object_get(NULL, i);
        printTaskInfo(tsk);
    }

    /* collect stats on all dynamically Created tasks */
    tsk = Task_Object_first();
    while (tsk) {
        printTaskInfo(tsk);
        tsk = Task_Object_next(tsk);
    }
    Serial.println("");
#endif
    Hwi_getStackInfo(&hwiStackStat, TRUE);
    Serial.print(F("Hwi stack usage: "));
    Serial.print(hwiStackStat.hwiStackPeak);
    Serial.print("/");
    Serial.println(hwiStackStat.hwiStackSize);
    Serial.println("");

    Memory_getStats(NULL, &memStat);
    Serial.print(F("Heap usage: "));
    Serial.print(memStat.totalSize - memStat.totalFreeSize);
    Serial.print("/");
    Serial.println(memStat.totalSize);
}
#endif /* DEBUG_UAT */

static bool UAT_Receive_Sync()
{
  // rxTimeout is in Radio time and needs to be converted from miliseconds to Radio Time
  rxPacket.rxTimeout = EasyLink_ms_To_RadioTime(2000);

  // Turn the receiver on immediately
  rxPacket.absTime = EasyLink_ms_To_RadioTime(0);

  EasyLink_Status status = myLink.receive(&rxPacket);

  if (status == EasyLink_Status_Success) {

#if defined(DEBUG_UAT)
    Serial.print("Packet received with length ");
    Serial.print(rxPacket.len);
    Serial.print(" RSSI ");
    Serial.print(rxPacket.rssi);
#if 0
    Serial.println(" and value:");
    Serial.println(Bin2Hex((byte *) rxPacket.payload, rxPacket.len));
#else
    Serial.println();
#endif
#endif

    rx_packets_counter++;

    return(true);
  } else {
#if defined(DEBUG_UAT)
    Serial.print("Error receiving packet with status code: ");
    Serial.print(status);
    Serial.print(" (");
    Serial.print(myLink.getStatusString(status));
    Serial.println(")");
#endif
    return(false);
  }
}

static bool UAT_receive_complete  = false;
static bool UAT_receive_active    = false;

void UAT_Receive_callback(EasyLink_RxPacket *rxPacket_ptr, EasyLink_Status status)
{
  UAT_receive_active = false;

  if (status == EasyLink_Status_Success) {
    memcpy(&rxPacket, rxPacket_ptr, sizeof(rxPacket));
    UAT_receive_complete  = true;
  }
}

static bool UAT_Receive_Async()
{
  bool success = false;
  EasyLink_Status status;

  if (!UAT_receive_active) {
    status = myLink.receive(&UAT_Receive_callback);

    if (status == EasyLink_Status_Success) {
      UAT_receive_active = true;
    }
  }

  if (UAT_receive_complete == true) {

    success = true;
    UAT_receive_complete = false;

    rx_packets_counter++;
  }

  return success;
}

#define UAT_Receive UAT_Receive_Async

void setup() {
  hw_info.soc = SoC_setup(); // Has to be very first procedure in the execution order

  Serial.begin(UAT_BOOT_BR, SERIAL_OUT_BITS);

  EEPROM_setup();

  settings->mode = SOFTRF_MODE_BRIDGE;

  Serial.println();
  Serial.print(F(SOFTRF_IDENT));
  Serial.print(SoC->name);
  Serial.print(F(" FW.REV: " SOFTRF_FIRMWARE_VERSION " DEV.ID: "));
  Serial.println(String(SoC->getChipId(), HEX));
  Serial.println(F("Copyright (C) 2015-2020 Linar Yusupov. All rights reserved."));
  Serial.flush();

  ThisAircraft.addr = SoC->getChipId() & 0x00FFFFFF;

  hw_info.display = SoC->Display_setup();

  hw_info.rf = RF_setup();

#if defined(DEBUG_UAT)
  Serial.print("SPI radio ID: ");
  Serial.println(hw_info.rf);
#endif

  if (hw_info.rf != RF_IC_NONE) {

    init_fec();

    if (settings->rf_protocol == RF_PROTOCOL_LEGACY) {
      hw_info.gnss = GNSS_setup();

#if defined(DEBUG_UAT)
      if (hw_info.gnss != GNSS_MODULE_NONE) {
        settings->nmea_g   = true;
        settings->nmea_out = NMEA_UART;
      }
#endif
    }

    Serial.print("Protocol: ");
    Serial.println(
      settings->rf_protocol == RF_PROTOCOL_LEGACY ? legacy_proto_desc.name :
      settings->rf_protocol == RF_PROTOCOL_OGNTP  ? ogntp_proto_desc.name  :
      settings->rf_protocol == RF_PROTOCOL_P3I    ? p3i_proto_desc.name    :
      settings->rf_protocol == RF_PROTOCOL_FANET  ? fanet_proto_desc.name  :
      "UNK"
    );

    Serial.print("GNSS: ");
    Serial.println(GNSS_name[hw_info.gnss]);
  }

  Battery_setup();

  /*
   * Display 'U' (UAT) on OLED for Rx only modes.
   * Indicate Tx protocol otherwise
   */
  ThisAircraft.protocol = settings->rf_protocol;

  Serial.println("Bridge mode.");

  myLink.begin(EasyLink_Phy_Custom);
  Serial.println("Listening...");

  SoC->WDT_setup();
}

void loop() {

  RF_loop();

  if (settings->rf_protocol == RF_PROTOCOL_LEGACY) {
    PickGNSSFix();
    GNSSTimeSync();
  }

  bool success = UAT_Receive();

  if (success) {

    int rs_errors;
    ThisAircraft.timestamp = now();

    int frame_type = correct_adsb_frame(rxPacket.payload, &rs_errors);

    if (frame_type != -1 &&
        uat978_decode((void *) rxPacket.payload, &ThisAircraft, &fo) ) {

#if defined(DEBUG_UAT)
      Serial.print(fo.addr, HEX);
      Serial.print(',');
      Serial.print(fo.aircraft_type, HEX);
      Serial.print(',');
      Serial.print(fo.latitude, 6);
      Serial.print(',');
      Serial.print(fo.longitude, 6);
      Serial.print(',');
      Serial.print(fo.altitude);
      Serial.print(',');
      Serial.print(fo.speed);
      Serial.print(',');
      Serial.print(fo.course);
      Serial.print(',');
      Serial.print(fo.vs);
      Serial.println();
      Serial.flush();
#endif

      if (settings->rf_protocol == RF_PROTOCOL_LEGACY) {
        /*
         * "Legacy" needs some accurate timing for proper operation
         */
        if (isValidFix()) {
          RF_Transmit(RF_Encode(&fo), false /* true */);
        }
      } else {
        RF_Transmit(RF_Encode(&fo), false /* true */);
      }
    } else {
#if defined(DEBUG_UAT)
      Serial.println("FEC error");
#endif
    }
  }

  // Show status info on tiny OLED display
  SoC->Display_loop();

  SoC->loop();

  Battery_loop();

  yield();
}

void shutdown(const char *msg)
{
  SoC->WDT_fini();

  if (settings->rf_protocol == RF_PROTOCOL_LEGACY) {
    GNSS_fini();
  }

  SoC->Display_fini(msg);

  RF_Shutdown();

  SoC_fini();
}
