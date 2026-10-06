#ifndef __ADS1292_RTTHREAD_H__
#define __ADS1292_RTTHREAD_H__

#include <rtthread.h>
#include <rtdevice.h>
#include "drv_common.h"
#include "drv_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Original vendor wiring:
 * RESET -> PB10
 * START -> PB11
 * DRDY  -> PA8
 * CS    -> PB12
 * SCK   -> PB13, MISO -> PB14, MOSI -> PB15, driven by RT-Thread spi2 bus.
 */
#ifndef ADS1292_SPI_BUS_NAME
#define ADS1292_SPI_BUS_NAME            "spi2"
#endif

#ifndef ADS1292_SPI_DEVICE_NAME
#define ADS1292_SPI_DEVICE_NAME         "ads1292"
#endif

#ifndef ADS1292_SPI_MAX_HZ
#define ADS1292_SPI_MAX_HZ              (1 * 1000 * 1000)
#endif

#ifndef ADS1292_SPI_MODE
#define ADS1292_SPI_MODE                1
#endif

#ifndef ADS1292_INIT_RETRY_MAX
#define ADS1292_INIT_RETRY_MAX          5
#endif

#ifndef ADS1292_PIN_RESET
#define ADS1292_PIN_RESET               GET_PIN(B, 10)
#endif

#ifndef ADS1292_PIN_START
#define ADS1292_PIN_START               GET_PIN(B, 11)
#endif

#ifndef ADS1292_PIN_DRDY
#define ADS1292_PIN_DRDY                GET_PIN(A, 8)
#endif

#ifndef ADS1292_PIN_CS
#define ADS1292_PIN_CS                  GET_PIN(B, 12)
#endif

#ifndef ADS1292_CS_GPIO_PORT
#define ADS1292_CS_GPIO_PORT            GPIOB
#endif

#ifndef ADS1292_CS_GPIO_PIN
#define ADS1292_CS_GPIO_PIN             GPIO_PIN_12
#endif

#define ADS1292_INIT_SAMPLE_NUM         2000

extern volatile rt_uint8_t ads1292_Cache[9];
extern volatile rt_uint8_t ads1292_recive_flag;

typedef void (*ads1292_sample_cb_t)(rt_int32_t ch1, rt_int32_t emg);

rt_err_t ads1292_init(void);
rt_err_t ads1292_deinit(void);

/* Register a callback invoked from the DRDY worker thread after each SPI read.
 * The callback runs in thread context (not ISR) so floating-point and blocking
 * inter-thread sync (mutex, mailbox) are allowed but must complete quickly:
 * SPI period is 2 ms at 500 SPS, so keep the callback well below that.
 *
 * Pass NULL to unregister.  Returns the previously registered callback.
 */
ads1292_sample_cb_t ads1292_register_sample_cb(ads1292_sample_cb_t cb);
rt_err_t ads1292_recover(void);

void ADS1292_PowerOnInit(void);
rt_uint8_t ADS1292_SPI(rt_uint8_t com);
void ADS1292_Send_CMD(rt_uint8_t data);
void ADS1292_WR_REGS(rt_uint8_t reg, rt_uint8_t len, rt_uint8_t *data);
rt_uint8_t ADS1292_Read_Data(rt_uint8_t *data);
void ADS1292_SET_REGBUFF(void);
rt_uint8_t ADS1292_WRITE_REGBUFF(void);
rt_uint8_t ADS1292_Noise_Test(void);
rt_uint8_t ADS1292_Single_Test(void);
rt_uint8_t ADS1292_Single_Read(void);
rt_uint8_t Set_ADS1292_Collect(rt_uint8_t mode);

rt_int32_t get_volt(rt_uint32_t num);
void ADS1292_val_init(float *data, float *a, float *b);
void Get_val_init_data(float *data, float *data2);

/* Keep a vendor-style alias for code that already calls ADS1292_Init(). */
#define ADS1292_Init()                  ads1292_init()
#define Val_Init_Num                    ADS1292_INIT_SAMPLE_NUM

/* ADS1292R commands. Prefixed names avoid collisions with HAL/RT-Thread symbols. */
#define ADS1292_CMD_WAKEUP              0x02
#define ADS1292_CMD_STANDBY             0x04
#define ADS1292_CMD_RESET               0x06
#define ADS1292_CMD_START               0x08
#define ADS1292_CMD_STOP                0x0A
#define ADS1292_CMD_OFFSETCAL           0x1A
#define ADS1292_CMD_RDATAC              0x10
#define ADS1292_CMD_SDATAC              0x11
#define ADS1292_CMD_RDATA               0x12
#define ADS1292_CMD_RREG                0x20
#define ADS1292_CMD_WREG                0x40

/* Register addresses. */
#define ADS1292_REG_ID                  0
#define ADS1292_REG_CONFIG1             1
#define ADS1292_REG_CONFIG2             2
#define ADS1292_REG_LOFF                3
#define ADS1292_REG_CH1SET              4
#define ADS1292_REG_CH2SET              5
#define ADS1292_REG_RLD_SENS            6
#define ADS1292_REG_LOFF_SENS           7
#define ADS1292_REG_LOFF_STAT           8
#define ADS1292_REG_RESP1               9
#define ADS1292_REG_RESP2               10
#define ADS1292_REG_GPIO                11

/* Device IDs. */
#define ADS1292_DEVICE_ID_ADS1292       0x53
#define ADS1292_DEVICE_ID_ADS1292R      0x73

/* CONFIG1. */
#define ADS1292_DATA_RATE_125SPS        0x00
#define ADS1292_DATA_RATE_250SPS        0x01
#define ADS1292_DATA_RATE_500SPS        0x02
#define ADS1292_DATA_RATE_1KSPS         0x03
#define ADS1292_DATA_RATE_2KSPS         0x04
#define ADS1292_DATA_RATE_4KSPS         0x05
#define ADS1292_DATA_RATE_8KSPS         0x06

/* CONFIG2. */
#define ADS1292_PDB_LOFF_COMP_OFF       0
#define ADS1292_PDB_LOFF_COMP_ON        1
#define ADS1292_PDB_REFBUF_OFF          0
#define ADS1292_PDB_REFBUF_ON           1
#define ADS1292_VREF_242V               0
#define ADS1292_VREF_4V                 1
#define ADS1292_CLK_EN_OFF              0
#define ADS1292_CLK_EN_ON               1
#define ADS1292_INT_TEST_OFF            0
#define ADS1292_INT_TEST_ON             1

/* Channel settings. */
#define ADS1292_PD_ON                   0
#define ADS1292_PD_OFF                  1
#define ADS1292_GAIN_6                  0
#define ADS1292_GAIN_1                  1
#define ADS1292_GAIN_2                  2
#define ADS1292_GAIN_3                  3
#define ADS1292_GAIN_4                  4
#define ADS1292_GAIN_8                  5
#define ADS1292_GAIN_12                 6
#define ADS1292_MUX_NORMAL_INPUT        0
#define ADS1292_MUX_INPUT_SHORTED       1
#define ADS1292_MUX_TEST_SIGNAL         5
#define ADS1292_MUX_RLD_DRP             6
#define ADS1292_MUX_RLD_DRM             7
#define ADS1292_MUX_RLD_DRPM            8
#define ADS1292_MUX_RSP_IN3P            9

/* RLD/lead-off/respiration settings. */
#define ADS1292_PDB_RLD_OFF             0
#define ADS1292_PDB_RLD_ON              1
#define ADS1292_RLD_LOFF_SENSE_OFF      0
#define ADS1292_RLD_LOFF_SENSE_ON       1
#define ADS1292_RLD_CHANNEL_OFF         0
#define ADS1292_RLD_CHANNEL_ON          1
#define ADS1292_FLIP2_OFF               0
#define ADS1292_FLIP2_ON                1
#define ADS1292_FLIP1_OFF               0
#define ADS1292_FLIP1_ON                1
#define ADS1292_LOFF_CHANNEL_OFF        0
#define ADS1292_LOFF_CHANNEL_ON         1
#define ADS1292_RESP_DEMOD_OFF          0
#define ADS1292_RESP_DEMOD_ON           1
#define ADS1292_RESP_MOD_OFF            0
#define ADS1292_RESP_MOD_ON             1
#define ADS1292_RESP_CTRL_CLK_INTERNAL  0
#define ADS1292_RESP_CTRL_CLK_EXTERNAL  1
#define ADS1292_CALIB_OFF               0
#define ADS1292_CALIB_ON                1
#define ADS1292_FREQ_32K                0
#define ADS1292_FREQ_64K                1
#define ADS1292_RLDREF_INT_EXTERN       0
#define ADS1292_RLDREF_INT_INTERNALLY   1

/* Defaults matching the vendor demo. */
#define ADS1292_DEVICE                  ADS1292_DEVICE_ID_ADS1292R
#define ADS1292_DATA_RATE               ADS1292_DATA_RATE_500SPS
#define ADS1292_PDB_LOFF_COMP           ADS1292_PDB_LOFF_COMP_ON
#define ADS1292_PDB_REFBUF              ADS1292_PDB_REFBUF_ON
#define ADS1292_VREF                    ADS1292_VREF_242V
#define ADS1292_CLK_EN                  ADS1292_CLK_EN_OFF
#define ADS1292_INT_TEST                ADS1292_INT_TEST_OFF
#define ADS1292_CH1_POWER               ADS1292_PD_ON
#define ADS1292_CH1_GAIN                ADS1292_GAIN_2
#define ADS1292_CH1_MUX                 ADS1292_MUX_NORMAL_INPUT
#define ADS1292_CH2_POWER               ADS1292_PD_ON
#define ADS1292_CH2_GAIN                ADS1292_GAIN_6
#define ADS1292_CH2_MUX                 ADS1292_MUX_NORMAL_INPUT
#define ADS1292_PDB_RLD                 ADS1292_PDB_RLD_ON
#define ADS1292_RLD_LOFF_SENSE          ADS1292_RLD_LOFF_SENSE_OFF
#define ADS1292_RLD2N                   ADS1292_RLD_CHANNEL_ON
#define ADS1292_RLD2P                   ADS1292_RLD_CHANNEL_ON
#define ADS1292_RLD1N                   ADS1292_RLD_CHANNEL_OFF
#define ADS1292_RLD1P                   ADS1292_RLD_CHANNEL_OFF
#define ADS1292_FLIP2                   ADS1292_FLIP2_OFF
#define ADS1292_FLIP1                   ADS1292_FLIP1_OFF
#define ADS1292_LOFF2N                  ADS1292_RLD_CHANNEL_ON
#define ADS1292_LOFF2P                  ADS1292_RLD_CHANNEL_ON
#define ADS1292_LOFF1N                  ADS1292_RLD_CHANNEL_OFF
#define ADS1292_LOFF1P                  ADS1292_RLD_CHANNEL_OFF
#define ADS1292_RESP_DEMOD_EN1          ADS1292_RESP_DEMOD_ON
#define ADS1292_RESP_MOD_EN             ADS1292_RESP_MOD_ON
#define ADS1292_RESP_PH                 0x0D
#define ADS1292_RESP_CTRL               ADS1292_RESP_CTRL_CLK_INTERNAL
#define ADS1292_CALIB                   ADS1292_CALIB_OFF
#define ADS1292_FREQ                    ADS1292_FREQ_32K
#define ADS1292_RLDREF_INT              ADS1292_RLDREF_INT_INTERNALLY

typedef struct
{
    rt_uint8_t Data_Rate;
} ADS1292_CONFIG1;

typedef struct
{
    rt_uint8_t Pdb_Loff_Comp;
    rt_uint8_t Pdb_Refbuf;
    rt_uint8_t Vref;
    rt_uint8_t Clk_EN;
    rt_uint8_t Int_Test;
} ADS1292_CONFIG2;

typedef struct
{
    rt_uint8_t PD;
    rt_uint8_t GAIN;
    rt_uint8_t MUX;
} ADS1292_CHSET;

typedef struct
{
    rt_uint8_t Pdb_Rld;
    rt_uint8_t Rld_Loff_Sense;
    rt_uint8_t Rld2N;
    rt_uint8_t Rld2P;
    rt_uint8_t Rld1N;
    rt_uint8_t Rld1P;
} ADS1292_RLD_SENS;

typedef struct
{
    rt_uint8_t Flip2;
    rt_uint8_t Flip1;
    rt_uint8_t Loff2N;
    rt_uint8_t Loff2P;
    rt_uint8_t Loff1N;
    rt_uint8_t Loff1P;
} ADS1292_LOFF_SENS;

typedef struct
{
    rt_uint8_t RESP_DemodEN;
    rt_uint8_t RESP_modEN;
    rt_uint8_t RESP_ph;
    rt_uint8_t RESP_Ctrl;
} ADS1292_RESP1;

typedef struct
{
    rt_uint8_t Calib;
    rt_uint8_t freq;
    rt_uint8_t Rldref_Int;
} ADS1292_RESP2;

#ifdef __cplusplus
}
#endif

#endif
