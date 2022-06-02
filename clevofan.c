#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/suspend.h>

#define FAN_DUTY_CMD                          0x99
#define FAN_PORT_AUTO_MODE                    0xFF

#define EC_TICKS_PER_MINUTE     1966080
#define CPU_FAN_SPEED_OFFSET_0  0xD0
#define CPU_FAN_SPEED_OFFSET_1  0xD1
#define GPU_FAN_SPEED_OFFSET_0  0xD2
#define GPU_FAN_SPEED_OFFSET_1  0xD3
#define GPU_FAN2_SPEED_OFFSET_0  0xD4
#define GPU_FAN2_SPEED_OFFSET_1  0xD5

#define EC_SC 0x66
#define EC_DATA 0x62
#define IBF 1
#define OBF 0

#define MODVERS "1.0"

static int force_match = 0;
static uint8_t fan_count;
static uint8_t pwm_curr_value[3] = { -1, -1, -1 };
static uint8_t fan_auto[3] =       {  1,  1,  1 };

static const struct dmi_system_id clevo_dmi[] = 
{    
    { .matches = { DMI_MATCH(DMI_BOARD_NAME, "W35_37ET"), }, },
    { .matches = { DMI_MATCH(DMI_BOARD_NAME, "W350SS"), }, },
    { .matches = { DMI_MATCH(DMI_BOARD_NAME, "P170SM-A"), }, },
    { .matches = { DMI_MATCH(DMI_BOARD_NAME, "P65xHP"), }, },
    {}
};
MODULE_DEVICE_TABLE(dmi, clevo_dmi);

static uint8_t get_fan_count(void)
{
    if( dmi_match(DMI_BOARD_NAME, "W35_37ET") ||          //mainboards with 1 fan
        dmi_match(DMI_BOARD_NAME, "W350SS"  )  )
        return 1;
        
    else if( dmi_match(DMI_BOARD_NAME, "P170SM"))         //mainboards with 2 fans
        return 2;
    
    else if( dmi_match(DMI_BOARD_NAME, "XXXXXXXX") ||     //mainboards with 3 fans
             dmi_match(DMI_BOARD_NAME, "XXXXXXXX") )        
        return 3;
        
        else return 1;
}

static int ec_io_wait(const uint32_t port, const uint32_t flag, const char value) {
    uint8_t data = inb(port);
    int i = 0;
    while ((((data >> flag) & 0x1) != value) && (i++ < 100)) {
        udelay(1000);
        data = inb(port);
    }
    if (i >= 100) {
        pr_warn("wait_ec error on port 0x%x, data=0x%x, flag=0x%x, value=0x%x\n",
                port, data, flag, value);
        return -EIO;
    }
    return 0;
}

static int ec_io_do(const uint32_t cmd, const uint32_t port, const uint8_t value) {
    ec_io_wait(EC_SC, IBF, 0);
    outb(cmd, EC_SC);

    ec_io_wait(EC_SC, IBF, 0);
    outb(port, EC_DATA);

    ec_io_wait(EC_SC, IBF, 0);
    outb(value, EC_DATA);

    return ec_io_wait(EC_SC, IBF, 0);
}

static int fan_set_pwm(uint8_t value, uint8_t index)
{
    int ret;
    ret = ec_io_do(FAN_DUTY_CMD, index+1, value);
    if(ret != 0) 
        return ret;
    pwm_curr_value[index] = value;
    fan_auto[index] = 0;
    return 0;
}

static int fan_auto_mode(uint8_t index)
{
    if(fan_auto[index] == 0) {
        int ret;
        ret = ec_io_do(FAN_DUTY_CMD, index+1, 0); //seems put fan off before setting auto mode is necessary
        if(ret != 0) 
            return ret;
        mdelay(100); //value found with tests
        ret = ec_io_do(FAN_DUTY_CMD, FAN_PORT_AUTO_MODE, index+1);
        if(ret != 0) 
            return ret;
        fan_auto[index] = 1;
        pwm_curr_value[index] = -1;
    }
    return 0;
}

static umode_t clevo_hwmon_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr, int channel)
{
    if(type == hwmon_fan) {
        switch (attr) {
        case hwmon_fan_input:
        case hwmon_fan_label:
            return S_IRUGO;
        default:
            return 0;
        }
    }
    else if(type == hwmon_pwm) {
        switch (attr) {
        case hwmon_pwm_input:
        case hwmon_pwm_enable: 
            return (S_IRUGO | S_IWUSR);
        default:
            return 0;
        }
    }
    return -EINVAL;
}

static int clevo_hwmon_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel, long *val)
{
    if(type == hwmon_fan)
    {
        u8 fan_data[2];
        int ec_ticks_per_rotation = 0;
        if(channel == 0) {
            ec_read(CPU_FAN_SPEED_OFFSET_0, &fan_data[0]);
            ec_read(CPU_FAN_SPEED_OFFSET_1, &fan_data[1]);
        }
        else if(channel == 1) {
            ec_read(GPU_FAN_SPEED_OFFSET_0, &fan_data[0]);
            ec_read(GPU_FAN_SPEED_OFFSET_1, &fan_data[1]);
        }
        else if(channel == 2) {
            ec_read(GPU_FAN2_SPEED_OFFSET_0, &fan_data[0]);
            ec_read(GPU_FAN2_SPEED_OFFSET_1, &fan_data[1]);
        }
        ec_ticks_per_rotation = (fan_data[0]<<8)|(fan_data[1]);
        if (ec_ticks_per_rotation == 0)
            *val = 0;
        else
            *val = (EC_TICKS_PER_MINUTE/ec_ticks_per_rotation);
        return 0;
    }
    else if(type == hwmon_pwm)
    {
        if(attr == hwmon_pwm_input) {
            *val = pwm_curr_value[channel];
            return 0;
        }
        else if(attr == hwmon_pwm_enable) {
            if(fan_auto[channel] == 1) 
                *val = 2;
            else 
                *val = 1;
            return 0;
        }
        else return -EOPNOTSUPP;
    }
    return -EOPNOTSUPP;
}

static int clevo_hwmon_read_label(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel, const char **str)
{
    if(type == hwmon_fan && attr == hwmon_fan_label) {
        if     (channel == 0) *str = "CPU Fan";
        else if(channel == 1) *str = "GPU Fan 1";
        else if(channel == 2) *str = "GPU Fan 2";
        return 0;
    }
    else return -EOPNOTSUPP;
}

static int clevo_hwmon_write(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel, long val)
{
    if(type == hwmon_pwm) 
    {
        if(attr == hwmon_pwm_input) 
        {
            if(fan_auto[channel] == 0)
                return fan_set_pwm(val, channel);
            else return -EOPNOTSUPP;
        }
        else if(attr == hwmon_pwm_enable)
        {
             if(val == 1)             
                 fan_auto[channel] = 0;
             else if(val == 2 || val == 0) 
                return fan_auto_mode(channel);
            return 0;
        }
        else return -EOPNOTSUPP;
    }
    return -EOPNOTSUPP;
}

static int clevo_pm_handler(struct notifier_block *nbp, unsigned long event_type, void *p) 
{
    switch (event_type) {
        case PM_POST_HIBERNATION:
        case PM_POST_SUSPEND:
        case PM_POST_RESTORE:
        {
            int8_t i;
            for(i=0; i<fan_count;i++) {
                if(fan_auto[i] == 0) 
                    fan_set_pwm(pwm_curr_value[i], i);
            }
        }
    }
    return 0;
}

static struct notifier_block nb = {
    .notifier_call = &clevo_pm_handler
};

static const struct hwmon_ops clevo_hwmon_ops = {
    .is_visible = clevo_hwmon_is_visible,
    .read = clevo_hwmon_read,
    .read_string = clevo_hwmon_read_label,
    .write = clevo_hwmon_write,
};

static const struct hwmon_channel_info *clevo_hwmon_info[] = {
    HWMON_CHANNEL_INFO(fan, 
        HWMON_F_LABEL | HWMON_F_INPUT),
    HWMON_CHANNEL_INFO(pwm, 
        HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
    NULL
};
static const struct hwmon_channel_info *clevo_hwmon_info2[] = {
    HWMON_CHANNEL_INFO(fan, 
        HWMON_F_LABEL | HWMON_F_INPUT,
        HWMON_F_LABEL | HWMON_F_INPUT),
    HWMON_CHANNEL_INFO(pwm, 
        HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
        HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
    NULL
};
static const struct hwmon_channel_info *clevo_hwmon_info3[] = {
    HWMON_CHANNEL_INFO(fan, 
        HWMON_F_LABEL | HWMON_F_INPUT,
        HWMON_F_LABEL | HWMON_F_INPUT,
        HWMON_F_LABEL | HWMON_F_INPUT),
    HWMON_CHANNEL_INFO(pwm, 
        HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
        HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
        HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
    NULL
};
static const struct hwmon_chip_info clevo_hwmon_chip_info = {
    .ops = &clevo_hwmon_ops,
    .info = clevo_hwmon_info,
};
static const struct hwmon_chip_info clevo_hwmon_chip_info2 = {
    .ops = &clevo_hwmon_ops,
    .info = clevo_hwmon_info2,
};
static const struct hwmon_chip_info clevo_hwmon_chip_info3 = {
    .ops = &clevo_hwmon_ops,
    .info = clevo_hwmon_info3,
};

static struct platform_driver clevo_platdrv = {
    .driver = {
        .name = "clevofan",
    },
};

static int __init clevo_platform_probe(struct platform_device *pdev)
{
    struct device *hwmon_dev;
    fan_count = force_match;
    if(fan_count == 0)
        fan_count = get_fan_count();
    pr_info("assuming %d FAN(s) to control\n", fan_count);
    if(fan_count == 1)
        hwmon_dev = 
        devm_hwmon_device_register_with_info(&pdev->dev, 
        dmi_get_system_info(DMI_BOARD_NAME), NULL, &clevo_hwmon_chip_info, NULL);

    else if(fan_count == 2)
        hwmon_dev = 
        devm_hwmon_device_register_with_info(&pdev->dev, 
        dmi_get_system_info(DMI_BOARD_NAME), NULL, &clevo_hwmon_chip_info2, NULL);

    else if(fan_count == 3)
        hwmon_dev = 
        devm_hwmon_device_register_with_info(&pdev->dev, 
        dmi_get_system_info(DMI_BOARD_NAME), NULL, &clevo_hwmon_chip_info3, NULL);
        
    return PTR_ERR_OR_ZERO(hwmon_dev);
}

static struct platform_device *clevo_platdvc;

static int __init clevofan_init(void)
{
    acpi_handle ec_handle;
    
    if(strncmp(dmi_get_system_info(DMI_BOARD_VENDOR), "CLEVO CO.", 9) != 0)
        return -ENODEV;
    if (!dmi_first_match(clevo_dmi) && force_match == 0)
        return -ENODEV;
    
    ec_handle = ec_get_handle();
    if (!ec_handle)
        return -ENODEV;
    
    pr_info("Found CLEVO %s, creating hwmon interfaces\n", dmi_get_system_info(DMI_BOARD_NAME));
    clevo_platdvc = platform_create_bundle(&clevo_platdrv, clevo_platform_probe, NULL, 0, NULL, 0);
    register_pm_notifier(&nb);

    return PTR_ERR_OR_ZERO(clevo_platdvc);
}

static void __exit clevofan_exit(void)
{
    uint8_t i;
    for(i=0; i<fan_count;i++) {
        if(fan_auto[i] == 0) {
            fan_auto_mode(i);
            pr_info("Setting Fan auto mode(FAN_%d)\n", i);
        }
    }
    pr_info("exiting module\n");
    platform_device_unregister(clevo_platdvc);
    platform_driver_unregister(&clevo_platdrv);
    unregister_pm_notifier(&nb);
}

module_init(clevofan_init);
module_exit(clevofan_exit);

MODULE_AUTHOR("simopil");
MODULE_DESCRIPTION("Fan control module for Clevo mainboards");
MODULE_LICENSE("GPL");
MODULE_VERSION(MODVERS);

MODULE_PARM_DESC(force_match, "Force loading despite not-matching mainboard - set number of fans");
module_param(force_match, int, 0600);
