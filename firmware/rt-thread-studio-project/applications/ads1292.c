#include "ads1292.h"
#include <stdlib.h>

#define DBG_TAG "ads1292"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static struct rt_spi_device *ads1292_spi_dev = RT_NULL;
static struct rt_semaphore ads1292_drdy_sem;
static rt_thread_t ads1292_thread = RT_NULL;
static rt_bool_t ads1292_thread_running = RT_FALSE;
static rt_bool_t ads1292_irq_attached = RT_FALSE;
static volatile rt_bool_t ads1292_recovering = RT_FALSE;

static const rt_uint16_t ads1292_spi_modes[] = {
    RT_SPI_MODE_0,
    RT_SPI_MODE_1,
    RT_SPI_MODE_2,
    RT_SPI_MODE_3,
};

rt_uint8_t ADS1292_REG[12];
volatile rt_uint8_t ads1292_recive_flag = 0;
volatile rt_uint8_t ads1292_Cache[9];

static volatile ads1292_sample_cb_t ads1292_sample_cb = RT_NULL;

ads1292_sample_cb_t ads1292_register_sample_cb(ads1292_sample_cb_t cb)
{
    ads1292_sample_cb_t prev;
    rt_base_t level = rt_hw_interrupt_disable();
    prev = ads1292_sample_cb;
    ads1292_sample_cb = cb;
    rt_hw_interrupt_enable(level);
    return prev;
}

static rt_int32_t ads1292_decode_signed24(const rt_uint8_t *p)
{
    rt_uint32_t raw = ((rt_uint32_t)p[0] << 16) |
                      ((rt_uint32_t)p[1] << 8)  |
                      ((rt_uint32_t)p[2]);
    rt_int32_t v = (rt_int32_t)raw;
    v <<= 8;
    v >>= 8;
    return v;
}

ADS1292_CONFIG1 Ads1292_Config1 = {ADS1292_DATA_RATE};
ADS1292_CONFIG2 Ads1292_Config2 = {
    ADS1292_PDB_LOFF_COMP,
    ADS1292_PDB_REFBUF,
    ADS1292_VREF,
    ADS1292_CLK_EN,
    ADS1292_INT_TEST
};
ADS1292_CHSET Ads1292_Ch1set = {
    ADS1292_CH1_POWER,
    ADS1292_CH1_GAIN,
    ADS1292_CH1_MUX
};
ADS1292_CHSET Ads1292_Ch2set = {
    ADS1292_CH2_POWER,
    ADS1292_CH2_GAIN,
    ADS1292_CH2_MUX
};
ADS1292_RLD_SENS Ads1292_Rld_Sens = {
    ADS1292_PDB_RLD,
    ADS1292_RLD_LOFF_SENSE,
    ADS1292_RLD2N,
    ADS1292_RLD2P,
    ADS1292_RLD1N,
    ADS1292_RLD1P
};
ADS1292_LOFF_SENS Ads1292_Loff_Sens = {
    ADS1292_FLIP2,
    ADS1292_FLIP1,
    ADS1292_LOFF2N,
    ADS1292_LOFF2P,
    ADS1292_LOFF1N,
    ADS1292_LOFF1P
};
ADS1292_RESP1 Ads1292_Resp1 = {
    ADS1292_RESP_DEMOD_EN1,
    ADS1292_RESP_MOD_EN,
    ADS1292_RESP_PH,
    ADS1292_RESP_CTRL
};
ADS1292_RESP2 Ads1292_Resp2 = {
    ADS1292_CALIB,
    ADS1292_FREQ,
    ADS1292_RLDREF_INT
};

static void ads1292_delay_us(rt_uint32_t us)
{
    rt_hw_us_delay(us);
}

static void ads1292_delay_ms(rt_uint32_t ms)
{
    rt_thread_mdelay(ms);
}

static void ads1292_delay_s(rt_uint32_t s)
{
    rt_thread_mdelay(s * 1000);
}

static void ads1292_hard_reset(void)
{
    rt_pin_write(ADS1292_PIN_RESET, PIN_LOW);
    ads1292_delay_ms(10);
    rt_pin_write(ADS1292_PIN_RESET, PIN_HIGH);
    ads1292_delay_ms(100);
}

static rt_err_t ads1292_spi_transfer_no_cs(const void *send_buf,
                                           void *recv_buf,
                                           rt_size_t length)
{
    struct rt_spi_message msg;

    if (ads1292_spi_dev == RT_NULL)
    {
        return -RT_ENOSYS;
    }

    msg.send_buf = send_buf;
    msg.recv_buf = recv_buf;
    msg.length = length;
    msg.cs_take = 0;
    msg.cs_release = 0;
    msg.next = RT_NULL;

    return rt_spi_transfer_message(ads1292_spi_dev, &msg) == RT_NULL ? RT_EOK : -RT_EIO;
}

static rt_err_t ads1292_spi_sendrecv8_no_cs(rt_uint8_t tx, rt_uint8_t *rx)
{
    rt_uint8_t temp = 0;
    rt_err_t result;

    result = ads1292_spi_transfer_no_cs(&tx, &temp, 1);
    if (rx != RT_NULL)
    {
        *rx = temp;
    }

    return result;
}

static rt_err_t ads1292_configure_spi(rt_uint8_t mode, rt_uint32_t max_hz)
{
    rt_err_t result;
    struct rt_spi_configuration cfg;

    if (ads1292_spi_dev == RT_NULL)
    {
        return -RT_ENOSYS;
    }

    if (mode >= sizeof(ads1292_spi_modes) / sizeof(ads1292_spi_modes[0]))
    {
        return -RT_EINVAL;
    }

    cfg.data_width = 8;
    cfg.mode = RT_SPI_MASTER | ads1292_spi_modes[mode] | RT_SPI_MSB;
    cfg.reserved = 0;
    cfg.max_hz = max_hz;

    result = rt_spi_configure(ads1292_spi_dev, &cfg);
    if (result != RT_EOK)
    {
        LOG_E("configure %s failed: %d", ADS1292_SPI_DEVICE_NAME, result);
    }

    return result;
}

static void ads1292_drdy_irq(void *args)
{
    RT_UNUSED(args);
    rt_sem_release(&ads1292_drdy_sem);
}

static void ads1292_data_thread_entry(void *parameter)
{
    RT_UNUSED(parameter);

    while (ads1292_thread_running)
    {
        if (rt_sem_take(&ads1292_drdy_sem, RT_WAITING_FOREVER) != RT_EOK)
        {
            continue;
        }

        if (!ads1292_thread_running)
        {
            break;
        }

        if (ads1292_recovering)
        {
            continue;
        }

        if (rt_pin_read(ADS1292_PIN_DRDY) == PIN_LOW)
        {
            rt_uint8_t local[9];
            ADS1292_Read_Data(local);

            /* Make the raw frame visible to any legacy poll-based consumer. */
            rt_memcpy((void *)ads1292_Cache, local, sizeof(local));
            ads1292_recive_flag = 1;

            /* Deliver decoded channels to the registered consumer (e.g. emg_pipeline). */
            ads1292_sample_cb_t cb = ads1292_sample_cb;
            if (cb != RT_NULL)
            {
                rt_int32_t ch1 = ads1292_decode_signed24(&local[3]);
                rt_int32_t emg = ads1292_decode_signed24(&local[6]);
                cb(ch1, emg);
            }
        }
    }

    ads1292_thread = RT_NULL;
    rt_sem_detach(&ads1292_drdy_sem);
}

static rt_err_t ads1292_attach_spi_device(void)
{
    rt_err_t result;

    ads1292_spi_dev = (struct rt_spi_device *)rt_device_find(ADS1292_SPI_DEVICE_NAME);
    if (ads1292_spi_dev == RT_NULL)
    {
        result = rt_hw_spi_device_attach(ADS1292_SPI_BUS_NAME,
                                         ADS1292_SPI_DEVICE_NAME,
                                         ADS1292_CS_GPIO_PORT,
                                         ADS1292_CS_GPIO_PIN);
        if (result != RT_EOK)
        {
            LOG_E("attach %s to %s failed: %d",
                  ADS1292_SPI_DEVICE_NAME,
                  ADS1292_SPI_BUS_NAME,
                  result);
            return result;
        }

        ads1292_spi_dev = (struct rt_spi_device *)rt_device_find(ADS1292_SPI_DEVICE_NAME);
    }

    if (ads1292_spi_dev == RT_NULL)
    {
        LOG_E("can't find spi device %s", ADS1292_SPI_DEVICE_NAME);
        return -RT_ENOSYS;
    }

    return ads1292_configure_spi(ADS1292_SPI_MODE, ADS1292_SPI_MAX_HZ);
}

static rt_err_t ads1292_start_drdy_thread(void)
{
    rt_err_t result;

    if (ads1292_thread != RT_NULL)
    {
        return RT_EOK;
    }

    result = rt_sem_init(&ads1292_drdy_sem, "adsdrdy", 0, RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        return result;
    }

    ads1292_thread_running = RT_TRUE;
    ads1292_thread = rt_thread_create("ads1292",
                                      ads1292_data_thread_entry,
                                      RT_NULL,
                                      1024,
                                      12,
                                      10);
    if (ads1292_thread == RT_NULL)
    {
        ads1292_thread_running = RT_FALSE;
        rt_sem_detach(&ads1292_drdy_sem);
        return -RT_ENOMEM;
    }

    rt_thread_startup(ads1292_thread);
    return RT_EOK;
}

rt_err_t ads1292_init(void)
{
    rt_err_t result;

    result = ads1292_attach_spi_device();
    if (result != RT_EOK)
    {
        return result;
    }

    rt_pin_mode(ADS1292_PIN_RESET, PIN_MODE_OUTPUT);
    rt_pin_mode(ADS1292_PIN_START, PIN_MODE_OUTPUT);
    rt_pin_mode(ADS1292_PIN_DRDY, PIN_MODE_INPUT_PULLUP);
    rt_pin_write(ADS1292_PIN_RESET, PIN_HIGH);
    rt_pin_write(ADS1292_PIN_START, PIN_LOW);
    ads1292_hard_reset();

    result = ads1292_start_drdy_thread();
    if (result != RT_EOK)
    {
        LOG_E("start drdy thread failed: %d", result);
        return result;
    }

    if (!ads1292_irq_attached)
    {
        result = rt_pin_attach_irq(ADS1292_PIN_DRDY,
                                   PIN_IRQ_MODE_FALLING,
                                   ads1292_drdy_irq,
                                   RT_NULL);
        if (result != RT_EOK)
        {
            LOG_E("attach drdy irq failed: %d", result);
            ads1292_deinit();
            return result;
        }
        ads1292_irq_attached = RT_TRUE;
    }

    rt_pin_irq_enable(ADS1292_PIN_DRDY, PIN_IRQ_DISABLE);
    ads1292_recive_flag = 0;
    rt_memset((void *)ads1292_Cache, 0, sizeof(ads1292_Cache));

    ADS1292_PowerOnInit();
    {
        rt_uint8_t id = 0;

        ADS1292_WR_REGS(ADS1292_CMD_RREG | ADS1292_REG_ID, 1, &id);
        LOG_I("device id: 0x%02x", id);
    }

    {
        rt_uint8_t retry = 0;

        while (Set_ADS1292_Collect(0))
        {
            LOG_E("register setup failed");
            ads1292_delay_s(1);

#if ADS1292_INIT_RETRY_MAX > 0
            retry++;
            if (retry >= ADS1292_INIT_RETRY_MAX)
            {
                ads1292_deinit();
                return -RT_ERROR;
            }
#endif
        }
    }

    LOG_I("register setup ok");
    ads1292_delay_s(1);
    rt_pin_irq_enable(ADS1292_PIN_DRDY, PIN_IRQ_ENABLE);

    return RT_EOK;
}

rt_err_t ads1292_recover(void)
{
    rt_err_t result = RT_EOK;
    rt_uint8_t retry = 0;

    if (ads1292_spi_dev == RT_NULL)
    {
        return -RT_ERROR;
    }

    if (ads1292_recovering)
    {
        return -RT_EBUSY;
    }

    ads1292_recovering = RT_TRUE;
    LOG_W("recovering ADS1292R sampling");

    rt_pin_irq_enable(ADS1292_PIN_DRDY, PIN_IRQ_DISABLE);
    rt_thread_mdelay(5);

    ads1292_recive_flag = 0;
    rt_memset((void *)ads1292_Cache, 0, sizeof(ads1292_Cache));

    rt_pin_write(ADS1292_PIN_START, PIN_LOW);
    ads1292_hard_reset();
    ADS1292_PowerOnInit();

    while (Set_ADS1292_Collect(0))
    {
        retry++;
        LOG_E("recover register setup failed, retry=%u", retry);
        if (retry >= ADS1292_INIT_RETRY_MAX)
        {
            result = -RT_ERROR;
            break;
        }
        ads1292_delay_ms(100);
    }

    if (result == RT_EOK)
    {
        rt_pin_irq_enable(ADS1292_PIN_DRDY, PIN_IRQ_ENABLE);
        LOG_W("ADS1292R sampling recovered");
    }

    ads1292_recovering = RT_FALSE;
    return result;
}

rt_err_t ads1292_deinit(void)
{
    if (ads1292_irq_attached)
    {
        rt_pin_irq_enable(ADS1292_PIN_DRDY, PIN_IRQ_DISABLE);
        rt_pin_detach_irq(ADS1292_PIN_DRDY);
        ads1292_irq_attached = RT_FALSE;
    }

    if (ads1292_thread != RT_NULL)
    {
        ads1292_thread_running = RT_FALSE;
        rt_sem_release(&ads1292_drdy_sem);
    }

    return RT_EOK;
}

rt_uint8_t ADS1292_SPI(rt_uint8_t com)
{
    rt_uint8_t rx = 0;

    if (ads1292_spi_dev == RT_NULL)
    {
        return 0;
    }

    if (rt_spi_transfer(ads1292_spi_dev, &com, &rx, 1) != 1)
    {
        return 0;
    }

    return rx;
}

rt_uint8_t ADS1292_Read_Data(rt_uint8_t *data)
{
    static const rt_uint8_t tx_dummy[9] = {0};

    if (ads1292_spi_dev == RT_NULL || data == RT_NULL)
    {
        return 1;
    }

    return rt_spi_transfer(ads1292_spi_dev, tx_dummy, data, 9) == 9 ? 0 : 1;
}

void ADS1292_Send_CMD(rt_uint8_t data)
{
    if (ads1292_spi_dev == RT_NULL)
    {
        return;
    }

    if (rt_spi_take_bus(ads1292_spi_dev) != RT_EOK)
    {
        return;
    }

    if (rt_spi_take(ads1292_spi_dev) != RT_EOK)
    {
        rt_spi_release_bus(ads1292_spi_dev);
        return;
    }
    ads1292_delay_us(100);
    ads1292_spi_sendrecv8_no_cs(data, RT_NULL);
    ads1292_delay_us(100);
    rt_spi_release(ads1292_spi_dev);
    rt_spi_release_bus(ads1292_spi_dev);
}

void ADS1292_WR_REGS(rt_uint8_t reg, rt_uint8_t len, rt_uint8_t *data)
{
    rt_uint8_t i;
    rt_uint8_t count = len - 1;

    if (ads1292_spi_dev == RT_NULL || data == RT_NULL || len == 0)
    {
        return;
    }

    if (rt_spi_take_bus(ads1292_spi_dev) != RT_EOK)
    {
        return;
    }

    if (rt_spi_take(ads1292_spi_dev) != RT_EOK)
    {
        rt_spi_release_bus(ads1292_spi_dev);
        return;
    }
    ads1292_delay_us(100);
    ads1292_spi_sendrecv8_no_cs(reg, RT_NULL);
    ads1292_delay_us(100);
    ads1292_spi_sendrecv8_no_cs(count, RT_NULL);

    if (reg & ADS1292_CMD_WREG)
    {
        for (i = 0; i < len; i++)
        {
            ads1292_delay_us(100);
            ads1292_spi_sendrecv8_no_cs(data[i], RT_NULL);
        }
    }
    else
    {
        for (i = 0; i < len; i++)
        {
            ads1292_delay_us(100);
            ads1292_spi_sendrecv8_no_cs(0x00, &data[i]);
        }
    }

    ads1292_delay_us(100);
    rt_spi_release(ads1292_spi_dev);
    rt_spi_release_bus(ads1292_spi_dev);
}

void ADS1292_SET_REGBUFF(void)
{
    ADS1292_REG[ADS1292_REG_ID] = ADS1292_DEVICE;

    ADS1292_REG[ADS1292_REG_CONFIG1] = 0x00;
    ADS1292_REG[ADS1292_REG_CONFIG1] |= Ads1292_Config1.Data_Rate;

    ADS1292_REG[ADS1292_REG_CONFIG2] = 0x00;
    ADS1292_REG[ADS1292_REG_CONFIG2] |= Ads1292_Config2.Pdb_Loff_Comp << 6;
    ADS1292_REG[ADS1292_REG_CONFIG2] |= Ads1292_Config2.Pdb_Refbuf << 5;
    ADS1292_REG[ADS1292_REG_CONFIG2] |= Ads1292_Config2.Vref << 4;
    ADS1292_REG[ADS1292_REG_CONFIG2] |= Ads1292_Config2.Clk_EN << 3;
    ADS1292_REG[ADS1292_REG_CONFIG2] |= Ads1292_Config2.Int_Test << 1;
    ADS1292_REG[ADS1292_REG_CONFIG2] |= 0x81;

    ADS1292_REG[ADS1292_REG_LOFF] = 0x10;

    ADS1292_REG[ADS1292_REG_CH1SET] = 0x00;
    ADS1292_REG[ADS1292_REG_CH1SET] |= Ads1292_Ch1set.PD << 7;
    ADS1292_REG[ADS1292_REG_CH1SET] |= Ads1292_Ch1set.GAIN << 4;
    ADS1292_REG[ADS1292_REG_CH1SET] |= Ads1292_Ch1set.MUX;

    ADS1292_REG[ADS1292_REG_CH2SET] = 0x00;
    ADS1292_REG[ADS1292_REG_CH2SET] |= Ads1292_Ch2set.PD << 7;
    ADS1292_REG[ADS1292_REG_CH2SET] |= Ads1292_Ch2set.GAIN << 4;
    ADS1292_REG[ADS1292_REG_CH2SET] |= Ads1292_Ch2set.MUX;

    ADS1292_REG[ADS1292_REG_RLD_SENS] = 0x00;
    ADS1292_REG[ADS1292_REG_RLD_SENS] |= Ads1292_Rld_Sens.Pdb_Rld << 5;
    ADS1292_REG[ADS1292_REG_RLD_SENS] |= Ads1292_Rld_Sens.Rld_Loff_Sense << 4;
    ADS1292_REG[ADS1292_REG_RLD_SENS] |= Ads1292_Rld_Sens.Rld2N << 3;
    ADS1292_REG[ADS1292_REG_RLD_SENS] |= Ads1292_Rld_Sens.Rld2P << 2;
    ADS1292_REG[ADS1292_REG_RLD_SENS] |= Ads1292_Rld_Sens.Rld1N << 1;
    ADS1292_REG[ADS1292_REG_RLD_SENS] |= Ads1292_Rld_Sens.Rld1P;
    ADS1292_REG[ADS1292_REG_RLD_SENS] |= 0xC0;

    ADS1292_REG[ADS1292_REG_LOFF_SENS] = 0x00;
    ADS1292_REG[ADS1292_REG_LOFF_SENS] |= Ads1292_Loff_Sens.Flip2 << 5;
    ADS1292_REG[ADS1292_REG_LOFF_SENS] |= Ads1292_Loff_Sens.Flip1 << 4;
    ADS1292_REG[ADS1292_REG_LOFF_SENS] |= Ads1292_Loff_Sens.Loff2N << 3;
    ADS1292_REG[ADS1292_REG_LOFF_SENS] |= Ads1292_Loff_Sens.Loff2P << 2;
    ADS1292_REG[ADS1292_REG_LOFF_SENS] |= Ads1292_Loff_Sens.Loff1N << 1;
    ADS1292_REG[ADS1292_REG_LOFF_SENS] |= Ads1292_Loff_Sens.Loff1P;

    ADS1292_REG[ADS1292_REG_LOFF_STAT] = 0x00;

    ADS1292_REG[ADS1292_REG_RESP1] = 0x00;
    ADS1292_REG[ADS1292_REG_RESP1] |= Ads1292_Resp1.RESP_DemodEN << 7;
    ADS1292_REG[ADS1292_REG_RESP1] |= Ads1292_Resp1.RESP_modEN << 6;
    ADS1292_REG[ADS1292_REG_RESP1] |= Ads1292_Resp1.RESP_ph << 2;
    ADS1292_REG[ADS1292_REG_RESP1] |= Ads1292_Resp1.RESP_Ctrl;
    ADS1292_REG[ADS1292_REG_RESP1] |= 0x02;

    ADS1292_REG[ADS1292_REG_RESP2] = 0x00;
    ADS1292_REG[ADS1292_REG_RESP2] |= Ads1292_Resp2.Calib << 7;
    ADS1292_REG[ADS1292_REG_RESP2] |= Ads1292_Resp2.freq << 2;
    ADS1292_REG[ADS1292_REG_RESP2] |= Ads1292_Resp2.Rldref_Int << 1;
    ADS1292_REG[ADS1292_REG_RESP2] |= 0x01;

    ADS1292_REG[ADS1292_REG_GPIO] = 0x0C;
}

rt_uint8_t ADS1292_WRITE_REGBUFF(void)
{
    rt_uint8_t i;
    rt_uint8_t res = 0;
    rt_uint8_t reg_cache[12];

    ADS1292_SET_REGBUFF();
    ADS1292_WR_REGS(ADS1292_CMD_WREG | ADS1292_REG_CONFIG1, 11, ADS1292_REG + 1);
    ads1292_delay_ms(10);
    ADS1292_WR_REGS(ADS1292_CMD_RREG | ADS1292_REG_ID, 12, reg_cache);
    ads1292_delay_ms(10);

    for (i = 0; i < 12; i++)
    {
        if (ADS1292_REG[i] != reg_cache[i])
        {
            if (i != ADS1292_REG_ID &&
                i != ADS1292_REG_LOFF_STAT &&
                i != ADS1292_REG_GPIO)
            {
                res = 1;
                LOG_E("reg[%d] expect 0x%02x, read 0x%02x", i, ADS1292_REG[i], reg_cache[i]);
            }
        }
    }

    return res;
}

void ADS1292_PowerOnInit(void)
{
    ADS1292_Send_CMD(ADS1292_CMD_SDATAC);
    ads1292_delay_ms(100);
    ADS1292_Send_CMD(ADS1292_CMD_RESET);
    ads1292_delay_s(1);
    ADS1292_Send_CMD(ADS1292_CMD_SDATAC);
    ads1292_delay_ms(100);
}

rt_uint8_t ADS1292_Single_Test(void)
{
    rt_uint8_t res = 0;

    Ads1292_Config2.Int_Test = ADS1292_INT_TEST_ON;
    Ads1292_Ch1set.MUX = ADS1292_MUX_TEST_SIGNAL;
    Ads1292_Ch2set.MUX = ADS1292_MUX_TEST_SIGNAL;

    if (ADS1292_WRITE_REGBUFF())
    {
        res = 1;
    }

    ads1292_delay_ms(10);
    return res;
}

rt_uint8_t ADS1292_Noise_Test(void)
{
    rt_uint8_t res = 0;

    Ads1292_Config2.Int_Test = ADS1292_INT_TEST_OFF;
    Ads1292_Ch1set.MUX = ADS1292_MUX_INPUT_SHORTED;
    Ads1292_Ch2set.MUX = ADS1292_MUX_INPUT_SHORTED;

    if (ADS1292_WRITE_REGBUFF())
    {
        res = 1;
    }

    ads1292_delay_ms(10);
    return res;
}

rt_uint8_t ADS1292_Single_Read(void)
{
    rt_uint8_t res = 0;

    Ads1292_Config2.Int_Test = ADS1292_INT_TEST_OFF;
    Ads1292_Ch1set.MUX = ADS1292_MUX_NORMAL_INPUT;
    Ads1292_Ch2set.MUX = ADS1292_MUX_NORMAL_INPUT;

    if (ADS1292_WRITE_REGBUFF())
    {
        res = 1;
    }

    ads1292_delay_ms(10);
    return res;
}

rt_uint8_t Set_ADS1292_Collect(rt_uint8_t mode)
{
    rt_uint8_t res = 1;

    ads1292_delay_ms(10);

    switch (mode)
    {
    case 0:
        res = ADS1292_Single_Read();
        break;
    case 1:
        res = ADS1292_Single_Test();
        break;
    case 2:
        res = ADS1292_Noise_Test();
        break;
    default:
        return 1;
    }

    if (res)
    {
        return 1;
    }

    ADS1292_Send_CMD(ADS1292_CMD_RDATAC);
    ads1292_delay_ms(10);
    ADS1292_Send_CMD(ADS1292_CMD_START);
    ads1292_delay_ms(10);

    return 0;
}

rt_int32_t get_volt(rt_uint32_t num)
{
    rt_int32_t temp;

    temp = (rt_int32_t)num;
    temp <<= 8;
    temp >>= 8;

    return temp;
}

void ADS1292_val_init(float *data, float *a, float *b)
{
    rt_uint32_t i;
    float max_val;
    float min_val;

    if (data == RT_NULL || a == RT_NULL || b == RT_NULL)
    {
        return;
    }

    max_val = data[0];
    min_val = data[0];

    for (i = 1; i < ADS1292_INIT_SAMPLE_NUM; i++)
    {
        if (data[i] > max_val)
        {
            max_val = data[i];
        }
        if (data[i] < min_val)
        {
            min_val = data[i];
        }
    }

    if (max_val == min_val)
    {
        *a = 0.0f;
        *b = 0.0f;
        return;
    }

    *a = 180.0f / (max_val - min_val);
    *b = 220.0f - (*a) * max_val;
}

void Get_val_init_data(float *data, float *data2)
{
    rt_uint32_t i = 0;

    if (data == RT_NULL || data2 == RT_NULL)
    {
        return;
    }

    while (i < ADS1292_INIT_SAMPLE_NUM)
    {
        if (ads1292_recive_flag)
        {
            rt_uint8_t cache[9];
            rt_uint32_t channel0;
            rt_uint32_t channel1;

            rt_enter_critical();
            rt_memcpy(cache, (const void *)ads1292_Cache, sizeof(cache));
            ads1292_recive_flag = 0;
            rt_exit_critical();

            channel0 = ((rt_uint32_t)cache[3] << 16) |
                       ((rt_uint32_t)cache[4] << 8) |
                       ((rt_uint32_t)cache[5]);
            channel1 = ((rt_uint32_t)cache[6] << 16) |
                       ((rt_uint32_t)cache[7] << 8) |
                       ((rt_uint32_t)cache[8]);

            data[i] = (float)get_volt(channel1);
            data2[i] = (float)get_volt(channel0);
            i++;
        }
        else
        {
            rt_thread_mdelay(1);
        }
    }
}

int ads1292_probe(int argc, char **argv)
{
    rt_uint8_t id = 0;
    rt_uint8_t mode = ADS1292_SPI_MODE;
    rt_uint32_t max_hz = ADS1292_SPI_MAX_HZ;
    rt_err_t result;

    if (argc > 1)
    {
        mode = (rt_uint8_t)atoi(argv[1]);
    }

    if (argc > 2)
    {
        max_hz = (rt_uint32_t)atoi(argv[2]) * 1000;
    }

    rt_pin_mode(ADS1292_PIN_RESET, PIN_MODE_OUTPUT);
    rt_pin_mode(ADS1292_PIN_START, PIN_MODE_OUTPUT);
    rt_pin_mode(ADS1292_PIN_DRDY, PIN_MODE_INPUT_PULLUP);
    rt_pin_write(ADS1292_PIN_START, PIN_LOW);

    result = ads1292_attach_spi_device();
    if (result == RT_EOK)
    {
        result = ads1292_configure_spi(mode, max_hz);
    }

    if (result != RT_EOK)
    {
        rt_kprintf("ads1292 probe configure failed: %d\n", result);
        return result;
    }

    ads1292_hard_reset();
    ADS1292_PowerOnInit();
    ADS1292_WR_REGS(ADS1292_CMD_RREG | ADS1292_REG_ID, 1, &id);

    rt_kprintf("ads1292 probe: mode=%d, hz=%lu, id=0x%02x\n", mode, max_hz, id);
    return RT_EOK;
}
MSH_CMD_EXPORT(ads1292_probe, probe ADS1292 id: ads1292_probe [spi_mode 0-3] [khz]);
