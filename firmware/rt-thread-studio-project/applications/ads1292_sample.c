#include "ads1292.h"

static rt_thread_t ads1292_sample_thread = RT_NULL;

static void ads1292_sample_entry(void *parameter)
{
    RT_UNUSED(parameter);

    if (ads1292_init() != RT_EOK)
    {
        rt_kprintf("ads1292 init failed\n");
        ads1292_sample_thread = RT_NULL;
        return;
    }

    while (1)
    {
        if (ads1292_recive_flag)
        {
            rt_uint8_t data[9];
            rt_uint32_t breath_raw;
            rt_uint32_t ecg_raw;
            rt_int32_t breath;
            rt_int32_t ecg;

            rt_enter_critical();
            rt_memcpy(data, (const void *)ads1292_Cache, sizeof(data));
            ads1292_recive_flag = 0;
            rt_exit_critical();

            breath_raw = ((rt_uint32_t)data[3] << 16) |
                         ((rt_uint32_t)data[4] << 8) |
                         ((rt_uint32_t)data[5]);
            ecg_raw = ((rt_uint32_t)data[6] << 16) |
                      ((rt_uint32_t)data[7] << 8) |
                      ((rt_uint32_t)data[8]);

            breath = get_volt(breath_raw);
            ecg = get_volt(ecg_raw);

            rt_kprintf("breath=%ld, ecg=%ld\n", breath, ecg);
        }

        rt_thread_mdelay(1);
    }
}

int ads1292_sample_start(void)
{
    if (ads1292_sample_thread != RT_NULL)
    {
        rt_kprintf("ads1292 sample already started\n");
        return RT_EOK;
    }

    ads1292_sample_thread = rt_thread_create("adsdemo",
                                             ads1292_sample_entry,
                                             RT_NULL,
                                             2048,
                                             20,
                                             10);
    if (ads1292_sample_thread == RT_NULL)
    {
        rt_kprintf("create ads1292 sample thread failed\n");
        return -RT_ENOMEM;
    }

    rt_thread_startup(ads1292_sample_thread);
    return RT_EOK;
}
MSH_CMD_EXPORT(ads1292_sample_start, start ADS1292 sample on spi2);
