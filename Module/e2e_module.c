/*
 * ============================================================
 *  Kernel Module: GY521 Motion Detection + LED + Phototransistor
 * ============================================================
 *
 *  This platform driver performs:
 *  1. I2C communication with GY521 (MPU6050) sensor
 *  2. Gyroscope polling in a high-frequency kernel thread
 *  3. Motion detection using filtered gyroscope magnitude
 *  4. LED control based on motion detection
 *  5. Phototransistor interrupt handling
 *  6. Correlation between motion and optical events
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/hrtimer.h>
#include <linux/math64.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>

/* ---------------- I2C & MPU6050 CONFIG ---------------- */

#define I2C_BUS    1                    // I2C bus number

/* MPU6050 default address and registers */
#define MPU_ADDR             0x68
#define MPU_REG_SMPLRT_DIV   0x19
#define MPU_REG_CONFIG       0x1A
#define MPU_REG_GYRO_CONFIG  0x1B
#define MPU_REG_ACCEL_CONFIG 0x1C
#define MPU_REG_PWR_MGMT_1   0x6B
#define MPU_REG_WHO_AM_I     0x75
#define MPU_REG_ACCEL_XOUT_H 0x3B
#define MPU_REG_PWR_MGMT_2   0x6C

/* Motion detection registers (some duplicate macros for clarity) */
#define MPU_REG_MOT_THR      0x1F
#define MPU_REG_MOT_DUR      0x20

/* Interrupt configuration */
#define MPU_REG_INT_PIN_CFG  0x37
#define MPU_REG_INT_ENABLE   0x38
#define MPU_REG_INT_STATUS   0x3A

/* ---------------- MOTION / LED PARAMETERS ---------------- */

#define ACTIVE_REQUIRED 1     // Consecutive active samples to confirm motion
#define STILL_REQUIRED 10000  // Consecutive still samples to confirm stop
#define ERR_REQUIRED      20  // Consecutive read errors to force stop
#define ALPHA_PERCENT     5   // Low-pass filter alpha percentage
#define MOTION_THRESHOLD 25   // Motion threshold in deg/s
#define LED_BLINK_TIME (33 * 1000000) // LED blink duration (ns)

/* ---------------- FIFO FOR PHOTOTRANSISTOR EVENTS ---------------- */

DEFINE_KFIFO(event_fifo, u64, 128);  // 128 events deep

/* ---------------- GLOBAL VARIABLES ---------------- */

static struct i2c_adapter *gy521_i2c_adapter;
static struct i2c_client  *gy521_client;
static struct task_struct *poll_thread;

static struct gpio_desc *gpio_desc_pt;   // Phototransistor GPIO
static int irq_number_pt;                // IRQ number
static int irq_det_num;                  // Count of phototransistor detections
static int gy_det_num;                   // Count of gyro detections

static struct gpio_desc *gpio_desc_led;  // LED GPIO

u64 i2c_start, i2c_end;
static bool start_led = false;

/* ---------------- GY521 READ (ACCEL/ GYRO) ---------------- */

/**
 * gy521_read_xyz() - reads raw gyro X/Y/Z values
 * @gx: pointer to store X-axis
 * @gy: pointer to store Y-axis
 * @gz: pointer to store Z-axis
 *
 * Returns 0 on success, negative error on failure
 */
static int gy521_read_xyz(int *gx, int *gy, int *gz)
{
    u8 buf[6];

    /* Read 6 bytes starting at GYRO_XOUT_H (0x43) */
    int ret = i2c_smbus_read_i2c_block_data(gy521_client, 0x43, 6, buf);
    if (ret < 0)
        return ret;

    /* Combine MSB and LSB for signed 16-bit values */
    *gx = (s16)((buf[0] << 8) | buf[1]);
    *gy = (s16)((buf[2] << 8) | buf[3]);
    *gz = (s16)((buf[4] << 8) | buf[5]);

    return 0;
}

/* ---------------- POLLING THREAD (GYRO-BASED) ---------------- */

/**
 * gy521_poll_thread() - kernel thread for high-frequency gyro polling
 *
 * Implements:
 *  - 100 µs interval polling
 *  - Low-pass filtering
 *  - Motion detection
 *  - LED blink
 *  - Phototransistor correlation
 */
static int gy521_poll_thread(void *data)
{
    /* Pin thread to CPU 2 for deterministic timing */
    pr_info("Initializing Thread");
    set_cpus_allowed_ptr(current, cpumask_of(2));

    int x, y, z;                  // raw gyro values
    int fx = 0, fy = 0, fz = 0;   // filtered values
    int magnitude_raw;
    int magnitude_dps;

    bool moving = false;          // motion state
    bool has_blinked = false;     // LED blink state

    int active_count = 0;
    int still_count  = 0;
    int err_count   = 0;

    u64 candidate_start = 0;
    u64 candidate_stop  = 0;
    u64 candidate_err   = 0;
    u64 led_on = 0;
    u64 thread_time = 0;
    u64 avg_i2c = 0;
    u64 avg_sleep = 0;
    u64 avg_thread = 0;
    u64 cnt_avg = 0;

    irq_det_num = 0;
    gy_det_num = 0;

    int c_stop_cnt = 0;
    int c_start_cnt = 0;

    /* 100 µs interval for HRTIMER */
    ktime_t interval = ktime_set(0, 100 * 1000);

    pr_info("gy521 polling thread started\n");

    /* Initialize filter with first reading */
    if (gy521_read_xyz(&fx, &fy, &fz) < 0)
        fx = fy = fz = 0;

    while (!kthread_should_stop()) {
        bool pr_time = false;
        thread_time = ktime_get_real_ns();

        u64 i2c_start, i2c_end;
        i2c_start = ktime_get_real_ns();

        /* Read gyro values */
        if (gy521_read_xyz(&x, &y, &z) == 0) {
            i2c_end = ktime_get_real_ns();

            /* Apply low-pass filter to reduce noise */
            fx = ((100 - ALPHA_PERCENT) * fx + ALPHA_PERCENT * x) / 100;
            fy = ((100 - ALPHA_PERCENT) * fy + ALPHA_PERCENT * y) / 100;
            fz = ((100 - ALPHA_PERCENT) * fz + ALPHA_PERCENT * z) / 100;

            /* Compute vector magnitude */
            magnitude_raw = int_sqrt(
                (u32)(fx * fx + fy * fy + fz * fz)
            );

            magnitude_dps = magnitude_raw / 131; // convert to deg/s

            if (magnitude_dps > MOTION_THRESHOLD) {
                /* Motion detected */
                if (active_count < 1)
                {
                    candidate_start = ktime_get_real_ns();
                    c_start_cnt++;
                }

                active_count++;

                /* Confirm motion start */
                if (!moving && active_count >= ACTIVE_REQUIRED) {
                    moving = true;
                    start_led = true;
                    led_on = ktime_get_real_ns();
                    pr_info("LED_ON: %llu\n", led_on);

                    if (gpio_desc_led)
                        gpiod_set_value(gpio_desc_led, 1);

                    pr_info("c_start_cnt: %d\n", c_start_cnt);
                    c_start_cnt = 0;
                }

                /* Turn off LED after blink duration */
                u64 led_dur = ktime_get_real_ns();
                if(start_led && (led_dur - led_on >= LED_BLINK_TIME) && !has_blinked)
                {
                    pr_info("LED_OFF: %llu\n", led_dur);
                    if (gpio_desc_led)
                        gpiod_set_value(gpio_desc_led, 0);
                    has_blinked = true;
                }

                still_count = 0;

            } else {
                /* No motion detected */

                if (still_count == 0)
                {
                    candidate_stop = ktime_get_real_ns();
                    c_stop_cnt++;
                }

                if (still_count >= STILL_REQUIRED)
                    active_count = 0;

                if (moving && still_count >= STILL_REQUIRED) {
                    moving = false;
                    start_led = false;
                    pr_time = true;

                    pr_info("c_stop_cnt: %d\n", c_stop_cnt);
                    c_stop_cnt = 0;

                    /* Process phototransistor events from FIFO */
                    unsigned int count = kfifo_len(&event_fifo);
                    if (count > 1) {
                        u64 pt_event;
                        pr_info("Multiple phototransistor events occured \n");
                        pr_info("GPIO_16_IRQ: MULTIPLE\n");
                    } else if (count == 1) {
                        pr_info("single phototransistor events occured \n");
                        u64 pt_event;
                        kfifo_out(&event_fifo, &pt_event, 1);
                        irq_det_num++;
                        pr_info("%d - GPIO_16_IRQ: %llu\n", irq_det_num, pt_event);
                    } else {
                        pr_info("GPIO_16_IRQ: NONE \n");
                    }

                    gy_det_num++;
                    pr_info("%d - MOTION: %llu;%llu\n", gy_det_num, candidate_start, candidate_stop);

                    has_blinked = false;
                    kfifo_reset(&event_fifo);
                }
                still_count++;
            }

        } else {
            /* ---------------- READ ERROR ---------------- */
            if (still_count == 0)
                candidate_err = ktime_get_real_ns();

            err_count++;

            if (moving && err_count >= ERR_REQUIRED) {
                moving = false;
                pr_info("WHEEL_STOP (read error): %llu\n", candidate_err);
            }

            active_count = 0;
        }

        /* ---------------- SLEEP FOR INTERVAL ---------------- */
        thread_time = ktime_get_real_ns() - thread_time;
        u64 before, after;
        before = ktime_get_real_ns();
        schedule_hrtimeout(&interval, HRTIMER_MODE_REL);
        after = ktime_get_real_ns();

        if(pr_time)
        {
            pr_info("Slept: %llu ns\n", after - before);
            pr_info("Thread_time:%llu ns\n", thread_time);
            pr_info("i2c_lag: %llu ns\n", i2c_end - i2c_start);

            avg_i2c += (i2c_end - i2c_start);
            avg_sleep += (after - before);
            avg_thread += thread_time;
            cnt_avg++;
        }
    }

    /* Compute average metrics */
    if(cnt_avg > 0)
    {
        avg_i2c /= (u64)cnt_avg;
        avg_sleep /= (u64)cnt_avg;
        avg_thread /= (u64)cnt_avg;
    }

    pr_info("Terminating Thread - AVG_THREAD: %llu | AVG_SLEEP: %llu | AVG_i2C: %llu \n", avg_thread, avg_sleep, avg_i2c);

    return 0;
}

/* ---------------- GY521 CONFIGURATION ---------------- */

/* Configure MPU6050 registers with default settings */
static int gy521_configure(void)
{
    int ret;

    ret = i2c_smbus_write_byte_data(gy521_client, MPU_REG_PWR_MGMT_1, 0x00);
    if (ret < 0) return ret;

    ret = i2c_smbus_write_byte_data(gy521_client, MPU_REG_CONFIG, 0x00);
    if (ret < 0) return ret;

    ret = i2c_smbus_write_byte_data(gy521_client, MPU_REG_SMPLRT_DIV, 0x00);
    if (ret < 0) return ret;

    ret = i2c_smbus_write_byte_data(gy521_client, MPU_REG_GYRO_CONFIG, 0x00);
    if (ret < 0) return ret;

    ret = i2c_smbus_write_byte_data(gy521_client, MPU_REG_ACCEL_CONFIG, 0x00);
    if (ret < 0) return ret;

    return 0;
}

/* Attempt to attach to MPU6050 at given address */
static struct i2c_client *gy521_try_addr(u16 addr)
{
    struct i2c_client *client = i2c_new_dummy_device(gy521_i2c_adapter, addr);
    s32 devid;

    if (IS_ERR(client))
        return client;

    devid = i2c_smbus_read_byte_data(client, MPU_REG_WHO_AM_I);
    if (devid != 0x68 && devid != 0x70) {
        i2c_unregister_device(client);
        return ERR_PTR(-ENODEV);
    }

    return client;
}

/* ---------------- GPIO INTERRUPT HANDLER ---------------- */

/* Capture phototransistor timestamp in FIFO */
static irqreturn_t gpio_irq_handler_pt(int irq, void *dev_id)
{
    u64 monotonic_ns = ktime_get_real_ns();
    kfifo_in(&event_fifo, &monotonic_ns, 1);
    return IRQ_HANDLED;
}

/* ---------------- PLATFORM DRIVER PROBE ---------------- */

static int gpioirq_probe(struct platform_device *pdev)
{
    int ret;

    pr_info("gpioirq_probe: Configure irq\n");

    /* Acquire phototransistor GPIO descriptor */
    gpio_desc_pt = devm_gpiod_get(&pdev->dev, "gpio-irq", GPIOD_IN);
    if (IS_ERR(gpio_desc_pt)) {
        dev_err(&pdev->dev, "Failed to get GPIO descriptor for gpio 16\n");
        return PTR_ERR(gpio_desc_pt);
    }

    irq_number_pt = gpiod_to_irq(gpio_desc_pt);
    dev_info(&pdev->dev, "GPIO IRQ number: %d\n", irq_number_pt);
    if (irq_number_pt < 0) {
        dev_err(&pdev->dev, "Failed to get IRQ number for gpio 16\n");
        return irq_number_pt;
    }

    ret = devm_request_irq(&pdev->dev, irq_number_pt, gpio_irq_handler_pt,
                           IRQF_TRIGGER_FALLING, "gpio_irq_handler_pt", NULL);
    if (ret) {
        dev_err(&pdev->dev, "Failed to request IRQ %d for gpio 16\n", irq_number_pt);
        return ret;
    }

    dev_info(&pdev->dev, "GPIO IRQ module loaded (IRQ %d)\n", irq_number_pt);

    pr_info("gpioirq_probe: starting\n");

    /* Acquire LED GPIO */
    gpio_desc_led = devm_gpiod_get(&pdev->dev, "led", GPIOD_OUT_LOW);
    if (IS_ERR(gpio_desc_led)) {
        dev_err(&pdev->dev, "Failed to get LED GPIO\n");
        return PTR_ERR(gpio_desc_led);
    }

    gpiod_set_value(gpio_desc_led, 0);

    /* Acquire I2C adapter */
    pr_info("probe: before i2c_get_adapter (bus=%d)\n", I2C_BUS);
    gy521_i2c_adapter = i2c_get_adapter(I2C_BUS);
    pr_info("probe: after i2c_get_adapter (%p)\n", gy521_i2c_adapter);

    if (!gy521_i2c_adapter) {
        dev_err(&pdev->dev, "Failed to get I2C adapter %d\n", I2C_BUS);
        return -ENODEV;
    }

    /* Probe MPU6050 */
    pr_info("probe: before gy521_try_addr\n");
    gy521_client = gy521_try_addr(MPU_ADDR);
    pr_info("probe: after gy521_try_addr (%p)\n", gy521_client);

    if (IS_ERR(gy521_client)) {
        ret = PTR_ERR(gy521_client);
        i2c_put_adapter(gy521_i2c_adapter);
        return ret;
    }

    /* Configure MPU6050 */
    pr_info("probe: before gy521_configure\n");
    ret = gy521_configure();
    pr_info("probe: after gy521_configure (%d)\n", ret);
    if (ret < 0) {
        i2c_unregister_device(gy521_client);
        i2c_put_adapter(gy521_i2c_adapter);
        return ret;
    }

    /* Start polling thread */
    pr_info("probe: before kthread_run\n");
    poll_thread = kthread_run(gy521_poll_thread, NULL, "gy521_poll");
    pr_info("probe: after kthread_run (%p)\n", poll_thread);
    if (IS_ERR(poll_thread)) {
        ret = PTR_ERR(poll_thread);
        i2c_unregister_device(gy521_client);
        i2c_put_adapter(gy521_i2c_adapter);
        return ret;
    }

    pr_info("gpioirq_probe: done\n");
    return 0;
}

/* ---------------- PLATFORM DRIVER REMOVE ---------------- */

static int gpioirq_remove(struct platform_device *pdev)
{
    if (poll_thread)
        kthread_stop(poll_thread);

    if (gy521_client)
        i2c_unregister_device(gy521_client);

    if (gy521_i2c_adapter)
        i2c_put_adapter(gy521_i2c_adapter);

    return 0;
}

/* ---------------- DEVICE TREE MATCH TABLE ---------------- */

static const struct of_device_id gpioirq_of_match[] = {
    { .compatible = "custom,gpioirq" },
    { }
};
MODULE_DEVICE_TABLE(of, gpioirq_of_match);

/* ---------------- PLATFORM DRIVER STRUCT ---------------- */

static struct platform_driver gpioirq_driver = {
    .probe  = gpioirq_probe,
    .remove = gpioirq_remove,
    .driver = {
        .name = "gpioirq",
        .of_match_table = gpioirq_of_match,
    },
};

module_platform_driver(gpioirq_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GY521 polling + LED via DTS platform driver");