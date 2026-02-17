#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/ktime.h>

static struct gpio_desc *gpio_desc_irq;
static int irq_number;

// Iterrupt routine reocrding thee timestapmms
static irqreturn_t gpio_irq_handler(int irq, void *dev_id)
{
    u64 time_ns = ktime_get_real_ns();
    pr_info("GPIO_16_IRQ:%llu\n", time_ns);
    return IRQ_HANDLED;
}

// Initialise module
static int gpio_irq_init(struct platform_device *pdev)
{
    int ret;
    
    // Request GPIO descriptor named "gpio-irq" (gpio16_irq.dts)
    gpio_desc_irq = devm_gpiod_get(&pdev->dev, "gpio-irq", GPIOD_IN);
    if (IS_ERR(gpio_desc_irq)) {
        dev_err(&pdev->dev, "Failed to get GPIO 16 descriptor\n");
        return PTR_ERR(gpio_desc_irq);
    }

    // Map GPIO descriptor to IRQ number
    irq_number = gpiod_to_irq(gpio_desc_irq);
    dev_info(&pdev->dev, "GPIO IRQ number: %d\n", irq_number);
    if (irq_number < 0) {
        dev_err(&pdev->dev, "Failed to get IRQ number\n");
        return irq_number;
    }

    // Register interrupt handler for the GPIO IRQ
    ret = devm_request_irq(&pdev->dev, irq_number, gpio_irq_handler,
                           IRQF_TRIGGER_FALLING, "gpio_irq_handler", NULL);
    if (ret) {
        dev_err(&pdev->dev, "Failed to request IRQ %d\n", irq_number);
        return ret;
    }

    dev_info(&pdev->dev, "GPIO IRQ module loaded (IRQ %d)\n", irq_number);
    // Provide affinity hint to run irq on core 2
    irq_set_affinity_hint(irq_number, cpumask_of(2));

    return 0;
}

static int gpio_irq_remove(struct platform_device *pdev)
{
    dev_info(&pdev->dev, "GPIO IRQ module unloaded\n");
    return 0;
}

static const struct of_device_id gpio_irq_of_match[] = {
    { .compatible = "custom,gpioirq" },
    {},
};
MODULE_DEVICE_TABLE(of, gpio_irq_of_match);

static struct platform_driver gpio_irq_driver = {
    .probe = gpio_irq_init,
    .remove = gpio_irq_remove,
    .driver = {
        .name = "gpio_irq_driver",
        .of_match_table = gpio_irq_of_match,
    },
};

module_platform_driver(gpio_irq_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Francois Provost");
MODULE_DESCRIPTION("GPIO 16 IRQ logger with timestamp");