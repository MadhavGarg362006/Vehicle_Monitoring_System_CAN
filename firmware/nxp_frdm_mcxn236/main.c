/*
 * VHMS - FRDM-MCXN236 CAN node (sender)
 *
 * Board : FRDM-MCXN236          SDK : MCUXpresso SDK 26.9.0
 * CAN   : FLEXCAN1, 500 kbps, classical CAN, standard 11-bit IDs
 *
 * Sends
 *   0x105  speed         uint16  km/h x10                (GPS, only with a fix)
 *   0x10A  acceleration  3 x int16, milli-g  (X,Y,Z)     (onboard FXLS8974)
 *   0x10B  position      2 x int32, degrees x 1e6 (lat, lon)   (GPS, only with a fix)
 *
 * GPS         : NMEA at 9600 baud on LPUART5 (P1_16 RXD, P1_17 TXD)
 * Accelerometer: I2C on LPI2C2 (P4_0 SDA, P4_1 SCL), address 0x18 or 0x19
 *
 * Status is printed once per second on the MCU-Link virtual COM port.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "fsl_common.h"
#include "fsl_clock.h"
#include "fsl_debug_console.h"
#include "fsl_flexcan.h"
#include "fsl_lpuart.h"
#include "fsl_lpi2c.h"
#include "fsl_port.h"

#include "board.h"
#include "app.h"
#include "vhms_logic.h"

/*******************************************************************************
 * CONFIGURATION
 ******************************************************************************/

#define CAN_ID_SPEED         0x105U
#define CAN_ID_ACCEL         0x10AU
#define CAN_ID_POSITION      0x10BU

#define CAN_BITRATE          500000U
#define SEND_INTERVAL_MS     1000U
#define TX_TIMEOUT_MS        100U      /* abort a frame nobody acknowledges */
#define TX_QUEUE_SIZE        8U

#define GPS_BAUDRATE         9600U
#define GPS_FIX_MAX_AGE_MS   5000U

/* FXLS8974 (register map from NXP's ISSDK header) */
#define ACCEL_ADDR_LOW       0x18U
#define ACCEL_ADDR_HIGH      0x19U
#define FXLS_REG_OUT_X_LSB   0x04U
#define FXLS_REG_WHO_AM_I    0x13U
#define FXLS_REG_SENS_CFG1   0x15U     /* bit0 ACTIVE, bits2:1 FSR (00 = +-2 g) */
#define FXLS_REG_SENS_CFG3   0x17U     /* bits7:4 wake ODR, bits3:0 sleep ODR   */
#define FXLS_WHO_AM_I_VALUE  0x86U
#define FXLS_CFG1_STANDBY    0x00U
#define FXLS_CFG1_ACTIVE_2G  0x01U
#define FXLS_CFG3_ODR_50HZ   0x66U     /* both ODR nibbles = 0x6 = 50 Hz */

/*******************************************************************************
 * TIME BASE (1 ms SysTick)
 ******************************************************************************/

static volatile uint32_t g_ms = 0U;

void SysTick_Handler(void)
{
    g_ms++;
}

/*******************************************************************************
 * CAN TRANSMIT QUEUE
 ******************************************************************************/

typedef struct
{
    uint16_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} tx_item_t;

static tx_item_t txQueue[TX_QUEUE_SIZE];
static uint8_t   txQHead = 0U;
static uint8_t   txQTail = 0U;

static flexcan_handle_t      flexcanHandle;
static flexcan_frame_t       txFrame;
static flexcan_mb_transfer_t txXfer;

static volatile bool txComplete = false;
static bool          txBusy = false;
static uint32_t      txStartMs = 0U;

static uint32_t txSentCount = 0U;
static uint32_t txTimeoutCount = 0U;
static uint32_t txDropCount = 0U;

static FLEXCAN_CALLBACK(flexcan_callback);

static FLEXCAN_CALLBACK(flexcan_callback)
{
    (void)base;
    (void)handle;
    (void)userData;

    if ((status == kStatus_FLEXCAN_TxIdle) && (result == TX_MCXN_MB))
    {
        txComplete = true;
    }
}

static void QueueTx(uint16_t id, const uint8_t *data, uint8_t dlc)
{
    uint8_t next = (uint8_t)((txQHead + 1U) % TX_QUEUE_SIZE);

    if (next == txQTail)
    {
        txDropCount++;
        return;
    }

    txQueue[txQHead].id  = id;
    txQueue[txQHead].dlc = dlc;
    memset(txQueue[txQHead].data, 0, sizeof(txQueue[txQHead].data));
    memcpy(txQueue[txQHead].data, data, dlc);

    txQHead = next;
}

static void PopTx(void)
{
    txQTail = (uint8_t)((txQTail + 1U) % TX_QUEUE_SIZE);
}

/* Start the oldest queued frame, or finish / time out the one in flight. */
static void ServiceTx(void)
{
    if (txBusy)
    {
        if (txComplete)
        {
            txComplete = false;
            txBusy = false;
            txSentCount++;
            PopTx();
        }
        else if ((uint32_t)(g_ms - txStartMs) > TX_TIMEOUT_MS)
        {
            FLEXCAN_TransferAbortSend(EXAMPLE_CAN, &flexcanHandle, TX_MCXN_MB);
            txBusy = false;
            txTimeoutCount++;
            PopTx();
        }
        return;
    }

    if (txQTail == txQHead)
    {
        return;
    }

    {
        const tx_item_t *it = &txQueue[txQTail];

        memset(&txFrame, 0, sizeof(txFrame));

        txFrame.id     = FLEXCAN_ID_STD(it->id);
        txFrame.format = (uint8_t)kFLEXCAN_FrameFormatStandard;
        txFrame.type   = (uint8_t)kFLEXCAN_FrameTypeData;
        txFrame.length = it->dlc;

        txFrame.dataByte0 = it->data[0];
        txFrame.dataByte1 = it->data[1];
        txFrame.dataByte2 = it->data[2];
        txFrame.dataByte3 = it->data[3];
        txFrame.dataByte4 = it->data[4];
        txFrame.dataByte5 = it->data[5];
        txFrame.dataByte6 = it->data[6];
        txFrame.dataByte7 = it->data[7];

        txXfer.mbIdx = TX_MCXN_MB;
        txXfer.frame = &txFrame;

        txComplete = false;

        if (FLEXCAN_TransferSendNonBlocking(EXAMPLE_CAN, &flexcanHandle, &txXfer) == kStatus_Success)
        {
            txBusy = true;
            txStartMs = g_ms;
        }
        else
        {
            txDropCount++;
            PopTx();
        }
    }
}

static void InitCan(void)
{
    flexcan_config_t flexcanConfig;
    flexcan_timing_config_t timingConfig;

    FLEXCAN_GetDefaultConfig(&flexcanConfig);

    flexcanConfig.bitRate = CAN_BITRATE;

#if defined(EXAMPLE_CAN_CLK_SOURCE)
    flexcanConfig.clkSrc = EXAMPLE_CAN_CLK_SOURCE;
#endif

#if (defined(USE_IMPROVED_TIMING_CONFIG) && USE_IMPROVED_TIMING_CONFIG)

    memset(&timingConfig, 0, sizeof(timingConfig));

    if (FLEXCAN_CalculateImprovedTimingValues(
            EXAMPLE_CAN,
            flexcanConfig.bitRate,
            EXAMPLE_CAN_CLK_FREQ,
            &timingConfig))
    {
        memcpy(&flexcanConfig.timingConfig, &timingConfig, sizeof(timingConfig));
    }
    else
    {
        PRINTF("WARNING: Using default CAN timing.\r\n");
    }

#endif

    FLEXCAN_Init(EXAMPLE_CAN, &flexcanConfig, EXAMPLE_CAN_CLK_FREQ);

    FLEXCAN_TransferCreateHandle(EXAMPLE_CAN, &flexcanHandle, flexcan_callback, NULL);

    FLEXCAN_SetTxMbConfig(EXAMPLE_CAN, TX_MCXN_MB, true);
}

/*******************************************************************************
 * GPS (NMEA over LPUART5, interrupt driven ring buffer)
 ******************************************************************************/

static lpuart_handle_t gpsHandle;
static uint8_t         gpsRing[256];

static nmea_parser_t nmea;
static gps_fix_t     gpsFix;

static uint32_t gpsBytes = 0U;
static uint32_t gpsSentences = 0U;
static uint32_t gpsLastFixMs = 0U;

static void InitGps(void)
{
    lpuart_config_t cfg;

    CLOCK_SetClkDiv(GPS_CLK_DIV, 1U);
    CLOCK_AttachClk(GPS_CLK_ATTACH);

    LPUART_GetDefaultConfig(&cfg);
    cfg.baudRate_Bps = GPS_BAUDRATE;
    cfg.enableTx = true;
    cfg.enableRx = true;

    if (LPUART_Init(GPS_UART, &cfg, GPS_CLK_FREQ) != kStatus_Success)
    {
        PRINTF("GPS: LPUART init failed\r\n");
        return;
    }

    LPUART_TransferCreateHandle(GPS_UART, &gpsHandle, NULL, NULL);
    LPUART_TransferStartRingBuffer(GPS_UART, &gpsHandle, gpsRing, sizeof(gpsRing));

    nmea_init(&nmea);
    memset(&gpsFix, 0, sizeof(gpsFix));
}

static void ProcessGps(void)
{
    uint8_t chunk[32];
    size_t avail = LPUART_TransferGetRxRingBufferLength(GPS_UART, &gpsHandle);

    while (avail > 0U)
    {
        lpuart_transfer_t xfer;
        size_t got = 0U;

        xfer.data = chunk;
        xfer.dataSize = (avail > sizeof(chunk)) ? sizeof(chunk) : avail;

        if (LPUART_TransferReceiveNonBlocking(GPS_UART, &gpsHandle, &xfer, &got) != kStatus_Success)
        {
            break;
        }
        if (got == 0U)
        {
            break;
        }

        for (size_t i = 0U; i < got; i++)
        {
            gpsBytes++;

            if (nmea_feed(&nmea, (char)chunk[i], &gpsFix))
            {
                gpsSentences++;

                if (gpsFix.valid)
                {
                    gpsLastFixMs = g_ms;
                }
            }
        }

        avail = LPUART_TransferGetRxRingBufferLength(GPS_UART, &gpsHandle);
    }
}

/*******************************************************************************
 * ACCELEROMETER (FXLS8974 over LPI2C2)
 ******************************************************************************/

static bool    accelOk = false;
static uint8_t accelAddr = 0U;
static uint32_t accelErrors = 0U;

static status_t I2cRead(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    lpi2c_master_transfer_t x;

    memset(&x, 0, sizeof(x));
    x.flags          = kLPI2C_TransferDefaultFlag;
    x.slaveAddress   = addr;
    x.direction      = kLPI2C_Read;
    x.subaddress     = reg;
    x.subaddressSize = 1U;
    x.data           = buf;
    x.dataSize       = len;

    return LPI2C_MasterTransferBlocking(ACCEL_I2C, &x);
}

static status_t I2cWrite(uint8_t addr, uint8_t reg, uint8_t value)
{
    lpi2c_master_transfer_t x;

    memset(&x, 0, sizeof(x));
    x.flags          = kLPI2C_TransferDefaultFlag;
    x.slaveAddress   = addr;
    x.direction      = kLPI2C_Write;
    x.subaddress     = reg;
    x.subaddressSize = 1U;
    x.data           = &value;
    x.dataSize       = 1U;

    return LPI2C_MasterTransferBlocking(ACCEL_I2C, &x);
}

static void InitAccel(void)
{
    lpi2c_master_config_t cfg;

    accelAddr = 0U;
    accelOk = false;
    const uint8_t addrs[2] = { ACCEL_ADDR_LOW, ACCEL_ADDR_HIGH };

    CLOCK_SetClkDiv(ACCEL_CLK_DIV, 1U);
    CLOCK_AttachClk(ACCEL_CLK_ATTACH);

    LPI2C_MasterGetDefaultConfig(&cfg);
    cfg.baudRate_Hz = 400000U;
    LPI2C_MasterInit(ACCEL_I2C, &cfg, ACCEL_CLK_FREQ);

    SDK_DelayAtLeastUs(10000U, SystemCoreClock);   /* sensor power-up time */

    for (uint8_t i = 0U; i < 2U; i++)
    {
        uint8_t id = 0U;
        status_t st = I2cRead(addrs[i], FXLS_REG_WHO_AM_I, &id, 1U);

        if (st == kStatus_Success)
        {
            PRINTF("Accel: address 0x%02X answered, WHO_AM_I = 0x%02X\r\n",
                   (unsigned int)addrs[i], (unsigned int)id);

            if (id == FXLS_WHO_AM_I_VALUE)
            {
                accelAddr = addrs[i];
                break;
            }
        }
    }

    if (accelAddr == 0U)
    {
        PRINTF("Accel: FXLS8974 not found (check Pins tool: P4_0 SDA / P4_1 SCL on FC2)\r\n");
        return;
    }

    /* standby -> set ODR -> active, +-2 g */
    if ((I2cWrite(accelAddr, FXLS_REG_SENS_CFG1, FXLS_CFG1_STANDBY) != kStatus_Success) ||
        (I2cWrite(accelAddr, FXLS_REG_SENS_CFG3, FXLS_CFG3_ODR_50HZ) != kStatus_Success) ||
        (I2cWrite(accelAddr, FXLS_REG_SENS_CFG1, FXLS_CFG1_ACTIVE_2G) != kStatus_Success))
    {
        PRINTF("Accel: configuration write failed\r\n");
        return;
    }

    SDK_DelayAtLeastUs(50000U, SystemCoreClock);   /* wait for the first sample */

    accelOk = true;
    PRINTF("Accel: FXLS8974 running at 50 Hz, +-2 g\r\n");
}

static bool ReadAccel(int16_t *x_mg, int16_t *y_mg, int16_t *z_mg)
{
    uint8_t b[6];

    if (I2cRead(accelAddr, FXLS_REG_OUT_X_LSB, b, 6U) != kStatus_Success)
    {
        return false;
    }

    /* little-endian, 12-bit value sign extended to 16 bits */
    *x_mg = accel_raw_to_mg((int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8)));
    *y_mg = accel_raw_to_mg((int16_t)((uint16_t)b[2] | ((uint16_t)b[3] << 8)));
    *z_mg = accel_raw_to_mg((int16_t)((uint16_t)b[4] | ((uint16_t)b[5] << 8)));

    return true;
}

/*******************************************************************************
 * PRINT HELPERS
 ******************************************************************************/

static void PrintSigned(int16_t v)
{
    /* the lite debug console prints %d of negative numbers as unsigned, so do the sign by hand */
    uint32_t a = (v < 0) ? (uint32_t)(-(int32_t)v) : (uint32_t)v;

    PRINTF("%s%u", (v < 0) ? "-" : "", (unsigned int)a);
}

static void PrintDeg(int32_t e6)
{
    uint32_t a = (e6 < 0) ? (uint32_t)(-(int64_t)e6) : (uint32_t)e6;

    PRINTF("%s%u.%06u",
           (e6 < 0) ? "-" : "",
           (unsigned int)(a / 1000000U),
           (unsigned int)(a % 1000000U));
}

/*******************************************************************************
 * PIN SETUP WITH AUTOMATIC MUX DETECTION
 *
 *   P1_16 (J2 pin 18 "SDA") = Flexcomm 5 P0 -> LPUART5 RXD  <- GPS TX
 *   P1_17 (J2 pin 20 "SCL") = Flexcomm 5 P1 -> LPUART5 TXD  -> GPS RX
 *   P4_0 = Flexcomm 2 P0 -> LPI2C2 SDA,  P4_1 = Flexcomm 2 P1 -> LPI2C2 SCL
 *
 * I could not find the pin-mode (kPORT_MuxAltN) numbers for these pins, so the
 * firmware tries Alt2..Alt7 at start-up and keeps the first one that works:
 *   - accelerometer: the one where the FXLS8974 answers WHO_AM_I
 *   - GPS: the one where valid NMEA (RMC, correct checksum) arrives
 * The chosen numbers are printed. Once known, put them in the two defines below
 * (VHMS_FORCE_MUX_xxx) to skip the search.
 ******************************************************************************/

#define VHMS_FORCE_MUX_FC5  2U    /* 0 = search, otherwise 2..7 */
#define VHMS_FORCE_MUX_FC2  2U
#define VHMS_MUX_FIRST      2U
#define VHMS_MUX_LAST       7U
#define GPS_SCAN_MS         3000U

static void SetVhmsPin(PORT_Type *port, uint32_t pin, uint32_t alt, bool pullUp, bool openDrain)
{
    const port_pin_config_t cfg = {
        pullUp ? kPORT_PullUp : kPORT_PullDisable,
        kPORT_LowPullResistor,
        kPORT_FastSlewRate,
        kPORT_PassiveFilterDisable,
        openDrain ? kPORT_OpenDrainEnable : kPORT_OpenDrainDisable,
        kPORT_LowDriveStrength,
        (port_mux_t)alt,
        kPORT_InputBufferEnable,
        kPORT_InputNormal,
        kPORT_UnlockRegister};

    PORT_SetPinConfig(port, pin, &cfg);
}

static void SetGpsMux(uint32_t alt)
{
    SetVhmsPin(PORT1, 16U, alt, true, false);    /* GPS TX -> MCU RX (pull-up = idle high) */
    SetVhmsPin(PORT1, 17U, alt, false, false);   /* MCU TX -> GPS RX */
}

static void SetAccelMux(uint32_t alt)
{
    SetVhmsPin(PORT4, 0U, alt, true, true);      /* SDA */
    SetVhmsPin(PORT4, 1U, alt, true, true);      /* SCL */
}

static void FindGpsMux(void)
{
    uint32_t first = (VHMS_FORCE_MUX_FC5 != 0U) ? VHMS_FORCE_MUX_FC5 : VHMS_MUX_FIRST;
    uint32_t last  = (VHMS_FORCE_MUX_FC5 != 0U) ? VHMS_FORCE_MUX_FC5 : VHMS_MUX_LAST;

    CLOCK_EnableClock(kCLOCK_Port1);

    for (uint32_t alt = first; alt <= last; alt++)
    {
        uint32_t t0 = g_ms;

        SetGpsMux(alt);
        gpsSentences = 0U;

        while ((uint32_t)(g_ms - t0) < GPS_SCAN_MS)
        {
            ProcessGps();
            if ((gpsSentences > 0U) || (VHMS_FORCE_MUX_FC5 != 0U))
            {
                break;
            }
        }

        if (gpsSentences > 0U)
        {
            PRINTF("GPS: NMEA found with kPORT_MuxAlt%u on P1_16/P1_17\r\n", (unsigned int)alt);
            return;
        }
    }

    /* nothing heard: keep Alt2 so the status line still shows the GPS counters */
    SetGpsMux(first);
    PRINTF("GPS: no NMEA heard on any pin mode (check wiring/power; GPS may need a sky view)\r\n");
}

static void FindAccelMux(void)
{
    uint32_t first = (VHMS_FORCE_MUX_FC2 != 0U) ? VHMS_FORCE_MUX_FC2 : VHMS_MUX_FIRST;
    uint32_t last  = (VHMS_FORCE_MUX_FC2 != 0U) ? VHMS_FORCE_MUX_FC2 : VHMS_MUX_LAST;

    CLOCK_EnableClock(kCLOCK_Port4);

    for (uint32_t alt = first; alt <= last; alt++)
    {
        SetAccelMux(alt);
        InitAccel();

        if (accelOk)
        {
            PRINTF("Accel: found with kPORT_MuxAlt%u on P4_0/P4_1\r\n", (unsigned int)alt);
            return;
        }
    }
}

/*******************************************************************************
 * MAIN
 ******************************************************************************/

int main(void)
{
    uint32_t lastSendMs;
    int16_t ax = 0, ay = 0, az = 0;

    BOARD_InitHardware();

    PRINTF("\r\n========================================\r\n");
    PRINTF("   VHMS - FRDM-MCXN236 SENDER NODE\r\n");
    PRINTF("========================================\r\n");
    PRINTF("CAN  : FLEXCAN1, 500 kbps, IDs 0x105 / 0x10A / 0x10B\r\n");

    if (SysTick_Config(SystemCoreClock / 1000U) != 0U)
    {
        PRINTF("SysTick setup failed\r\n");
        while (1)
        {
        }
    }

    InitCan();
    InitGps();
    FindGpsMux();
    FindAccelMux();

    PRINTF("Running. GPS frames are sent only while the receiver has a fix.\r\n\r\n");

    lastSendMs = g_ms;

    while (1)
    {
        ProcessGps();
        ServiceTx();

        if ((uint32_t)(g_ms - lastSendMs) >= SEND_INTERVAL_MS)
        {
            bool fixFresh;
            bool accelRead = false;
            uint8_t data[8];

            lastSendMs = g_ms;

            fixFresh = gpsFix.valid && ((uint32_t)(g_ms - gpsLastFixMs) < GPS_FIX_MAX_AGE_MS);

            if (fixFresh)
            {
                pack_speed(data, gpsFix.speed_kmh_x10);
                QueueTx(CAN_ID_SPEED, data, 2U);

                pack_position(data, gpsFix.lat_e6, gpsFix.lon_e6);
                QueueTx(CAN_ID_POSITION, data, 8U);
            }

            if (accelOk)
            {
                if (ReadAccel(&ax, &ay, &az))
                {
                    accelRead = true;
                    pack_accel(data, ax, ay, az);
                    QueueTx(CAN_ID_ACCEL, data, 6U);
                }
                else
                {
                    accelErrors++;
                }
            }

            /* one status line per second */
            PRINTF("GPS bytes=%u RMC=%u fix=%s",
                   (unsigned int)gpsBytes,
                   (unsigned int)gpsSentences,
                   fixFresh ? "YES" : "no");

            if (fixFresh)
            {
                PRINTF(" lat=");
                PrintDeg(gpsFix.lat_e6);
                PRINTF(" lon=");
                PrintDeg(gpsFix.lon_e6);
                PRINTF(" speed=%u.%u km/h",
                       (unsigned int)(gpsFix.speed_kmh_x10 / 10U),
                       (unsigned int)(gpsFix.speed_kmh_x10 % 10U));
            }

            if (accelRead)
            {
                PRINTF(" | accel mg X=");
                PrintSigned(ax);
                PRINTF(" Y=");
                PrintSigned(ay);
                PRINTF(" Z=");
                PrintSigned(az);
            }

            PRINTF(" | CAN sent=%u timeout=%u drop=%u\r\n",
                   (unsigned int)txSentCount,
                   (unsigned int)txTimeoutCount,
                   (unsigned int)txDropCount);
        }
    }
}
