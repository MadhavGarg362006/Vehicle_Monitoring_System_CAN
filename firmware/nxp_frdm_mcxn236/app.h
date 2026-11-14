#ifndef _APP_H_
#define _APP_H_

/* ---------- FlexCAN1 (unchanged from the working project) ---------- */
#define EXAMPLE_CAN           CAN1
#define TX_MCXN_MB            2U
#define EXAMPLE_CAN_CLK_FREQ  CLOCK_GetFlexcanClkFreq(1U)
#define USE_IMPROVED_TIMING_CONFIG (1)

/*
 * ---------- GPS: LPUART5 on LP_FLEXCOMM5 ----------
 * Pins (set in the Pins tool): P1_16 = RXD  (header J2 pin 18, silkscreen SDA)
 *                              P1_17 = TXD  (header J2 pin 20, silkscreen SCL)
 */
#define GPS_UART              LPUART5
#define GPS_CLK_DIV           kCLOCK_DivFlexcom5Clk
#define GPS_CLK_ATTACH        kFRO12M_to_FLEXCOMM5
#define GPS_CLK_FREQ          CLOCK_GetLPFlexCommClkFreq(5U)

/*
 * ---------- Onboard FXLS8974 accelerometer: LPI2C2 on LP_FLEXCOMM2 ----------
 * Pins (set in the Pins tool): P4_0 = SDA, P4_1 = SCL
 */
#define ACCEL_I2C             LPI2C2
#define ACCEL_CLK_DIV         kCLOCK_DivFlexcom2Clk
#define ACCEL_CLK_ATTACH      kFRO12M_to_FLEXCOMM2
#define ACCEL_CLK_FREQ        CLOCK_GetLPFlexCommClkFreq(2U)

void BOARD_InitHardware(void);

#endif
