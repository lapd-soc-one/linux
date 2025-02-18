// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Antmicro <www.antmicro.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/gpio/driver.h>
#include <linux/gpio.h>
#include <linux/of_irq.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/litex.h>

#define GPIO_PINS_MAX   32
#define LITEX_GPIO_VALUE_OFFSET 0x0
#define LITEX_GPIO_MODE_OFFSET 0x4
#define LITEX_GPIO_EDGE_OFFSET 0x8
#define LITEX_GPIO_PENDING_OFFSET 0x10
#define LITEX_GPIO_ENABLE_OFFSET 0x14

struct litex_gpio {
	void __iomem *membase;
	int port_direction;
	int reg_span;
	struct gpio_chip chip;
	struct irq_chip ichip;
	spinlock_t gpio_lock;
	unsigned int irq_number;
};

/* Helper functions */

static inline u32 litex_gpio_get_reg(struct litex_gpio *gpio_s, int reg_offset)
{
	return _litex_get_reg(gpio_s->membase + reg_offset, gpio_s->reg_span);
}

static inline void litex_gpio_set_reg(struct litex_gpio *gpio_s, int reg_offset,
				      u32 value)
{
	_litex_set_reg(gpio_s->membase + reg_offset, gpio_s->reg_span, value);
}

/* API functions */

static int litex_gpio_get_value(struct gpio_chip *chip, unsigned int offset)
{
	struct litex_gpio *gpio_s = gpiochip_get_data(chip);
	u32 regv;

	if (offset >= chip->ngpio)
		return -EINVAL;

	regv = _litex_get_reg(gpio_s->membase, gpio_s->reg_span);
	return !!(regv & BIT(offset));
}

static int litex_gpio_get_multiple(struct gpio_chip *chip, unsigned long *mask,
				   unsigned long *bits)
{
	struct litex_gpio *gpio_s = gpiochip_get_data(chip);
	u32 regv;

	if (*mask >= (1 << chip->ngpio))
		return -EINVAL;

	regv = _litex_get_reg(gpio_s->membase, gpio_s->reg_span);
	*bits = (regv & *mask);
	return 0;
}

static void litex_gpio_set_value(struct gpio_chip *chip, unsigned int offset,
				 int val)
{
	struct litex_gpio *gpio_s = gpiochip_get_data(chip);
	u32 regv, new_regv;

	if (offset >= chip->ngpio)
		return;

	regv = _litex_get_reg(gpio_s->membase, gpio_s->reg_span);
	new_regv = (regv & ~BIT(offset)) | (!!val << offset);
	_litex_set_reg(gpio_s->membase, gpio_s->reg_span, new_regv);
}

static void litex_gpio_set_multiple(struct gpio_chip *chip, unsigned long *mask,
				    unsigned long *bits)
{
	struct litex_gpio *gpio_s = gpiochip_get_data(chip);
	u32 regv, new_regv;

	if (*mask >= (1 << chip->ngpio))
		return;

	regv = _litex_get_reg(gpio_s->membase, gpio_s->reg_span);
	new_regv = (regv & ~(*mask)) | (*bits);
	_litex_set_reg(gpio_s->membase, gpio_s->reg_span, new_regv);
}

static int litex_gpio_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	struct litex_gpio *gpio_s = gpiochip_get_data(chip);

	return gpio_s->port_direction;
}

static int litex_gpio_direction_input(struct gpio_chip *chip,
				      unsigned int offset)
{
	struct litex_gpio *gpio_s = gpiochip_get_data(chip);

	if (gpio_s->port_direction != GPIOF_DIR_IN)
		return -ENOTSUPP;
	else
		return 0;
}

static int litex_gpio_direction_output(struct gpio_chip *chip,
				       unsigned int offset, int value)
{
	struct litex_gpio *gpio_s = gpiochip_get_data(chip);

	if (gpio_s->port_direction != GPIOF_DIR_OUT)
		return -ENOTSUPP;
	else
		return 0;
}

static void litex_gpio_irq_unmask(struct irq_data *idata)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(idata);
	struct litex_gpio *gpio_s = gpiochip_get_data(chip);
	int offset = irqd_to_hwirq(idata) % GPIO_PINS_MAX;
	unsigned long flags;
	u32 bit = BIT(offset);
	u32 enable;

	spin_lock_irqsave(&gpio_s->gpio_lock, flags);

	/* Clear any sticky pending interrupts */
	litex_gpio_set_reg(gpio_s, LITEX_GPIO_PENDING_OFFSET, bit);
	enable = litex_gpio_get_reg(gpio_s, LITEX_GPIO_ENABLE_OFFSET);
	enable |= bit;
	litex_gpio_set_reg(gpio_s, LITEX_GPIO_ENABLE_OFFSET, enable);

	spin_unlock_irqrestore(&gpio_s->gpio_lock, flags);
}

static void litex_gpio_irq_mask(struct irq_data *idata)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(idata);
	struct litex_gpio *gpio_s = gpiochip_get_data(chip);
	int offset = irqd_to_hwirq(idata) % GPIO_PINS_MAX;
	unsigned long flags;
	u32 bit = BIT(offset);
	u32 enable;

	spin_lock_irqsave(&gpio_s->gpio_lock, flags);

	enable = litex_gpio_get_reg(gpio_s, LITEX_GPIO_ENABLE_OFFSET);
	enable &= ~bit;
	litex_gpio_set_reg(gpio_s, LITEX_GPIO_ENABLE_OFFSET, enable);

	spin_unlock_irqrestore(&gpio_s->gpio_lock, flags);
}

static int litex_gpio_irq_set_type(struct irq_data *idata, unsigned int type)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(idata);
	struct litex_gpio *gpio_s = gpiochip_get_data(chip);
	int offset = irqd_to_hwirq(idata) % GPIO_PINS_MAX;
	unsigned long flags;
	u32 bit = BIT(offset);
	u32 mode, edge;

	spin_lock_irqsave(&gpio_s->gpio_lock, flags);

	mode = litex_gpio_get_reg(gpio_s, LITEX_GPIO_MODE_OFFSET);
	edge = litex_gpio_get_reg(gpio_s, LITEX_GPIO_EDGE_OFFSET);

	switch (type & IRQ_TYPE_SENSE_MASK) {
		case IRQ_TYPE_NONE:
			break;

		case IRQ_TYPE_EDGE_RISING:
            mode &= ~bit;
			edge &= ~bit;
			break;

		case IRQ_TYPE_EDGE_FALLING:
            mode &= ~bit;
			edge |= bit;
			break;

        case IRQ_TYPE_EDGE_BOTH:
			mode |= bit;
            break;

		default:
			return -EINVAL;
	}
	litex_gpio_set_reg(gpio_s, LITEX_GPIO_MODE_OFFSET, mode);
	litex_gpio_set_reg(gpio_s, LITEX_GPIO_EDGE_OFFSET, edge);

	spin_unlock_irqrestore(&gpio_s->gpio_lock, flags);

	return 0;
}

static void litex_gpio_irq_eoi(struct irq_data *idata)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(idata);
	struct litex_gpio *gpio_s = gpiochip_get_data(chip);
	int offset = irqd_to_hwirq(idata) % GPIO_PINS_MAX;
	u32 bit = BIT(offset);
	unsigned long flags;

	spin_lock_irqsave(&gpio_s->gpio_lock, flags);

	/* Clear all pending interrupts */
	litex_gpio_set_reg(gpio_s, LITEX_GPIO_PENDING_OFFSET, bit);

	spin_unlock_irqrestore(&gpio_s->gpio_lock, flags);

	irq_chip_eoi_parent(idata);
}

static int litex_gpio_irq_set_affinity(struct irq_data *idata,
					const struct cpumask *dest,
					bool force)
{
	if (idata->parent_data)
		return irq_chip_set_affinity_parent(idata, dest, force);

	return -EINVAL;
}

static int litex_gpio_child_to_parent_hwirq(struct gpio_chip *chip,
					     unsigned int child,
					     unsigned int child_type,
					     unsigned int *parent,
					     unsigned int *parent_type)
{
	*parent = chip->irq.child_offset_to_irq(chip, child);
	*parent_type = child_type;

	return 0;
}

static void litex_gpio_irq(struct irq_desc *desc)
{
	struct litex_gpio *gpio_s = irq_desc_get_handler_data(desc);
	struct irq_domain *domain = gpio_s->chip.irq.domain;
	struct irq_chip *ichip = irq_desc_get_chip(desc);
	u32 enabled;
	u32 pending;
	unsigned long int interrupts_to_handle;
	unsigned int pin, irq;

	chained_irq_enter(ichip, desc);

	enabled = litex_gpio_get_reg(gpio_s, LITEX_GPIO_ENABLE_OFFSET);
	pending = litex_gpio_get_reg(gpio_s, LITEX_GPIO_PENDING_OFFSET);
	interrupts_to_handle = pending & enabled;

	for_each_set_bit(pin, &interrupts_to_handle, GPIO_PINS_MAX) {
		irq = irq_find_mapping(domain, pin);
		if (WARN_ON(irq == 0))
			continue;

		generic_handle_irq(irq);
	}

	chained_irq_exit(ichip, desc);
}

/* Driver functions */

static int litex_gpio_init_irq(struct platform_device *pdev,
			       struct litex_gpio *gpio_s)
{
	struct device_node *node = pdev->dev.of_node;
	struct device_node *irq_parent = of_irq_find_parent(node);
	struct irq_domain *parent_domain;
	struct gpio_irq_chip *gichip;

	if (!irq_parent) {
		dev_info(&pdev->dev, "no IRQ parent node\n");
		return 0;
	}

	parent_domain = irq_find_host(irq_parent);
	if (!parent_domain) {
		dev_err(&pdev->dev, "no IRQ parent domain\n");
		return -ENODEV;
	}

	gpio_s->irq_number = platform_get_irq(pdev, 0);

	/* Disable all GPIO interrupts before enabling parent interrupts */
	litex_gpio_set_reg(gpio_s, LITEX_GPIO_ENABLE_OFFSET, 0);

	gpio_s->ichip.name = pdev->name;
	gpio_s->ichip.irq_unmask = litex_gpio_irq_unmask;
	gpio_s->ichip.irq_mask = litex_gpio_irq_mask;
	gpio_s->ichip.irq_set_type = litex_gpio_irq_set_type;
	gpio_s->ichip.irq_eoi = litex_gpio_irq_eoi;
	gpio_s->ichip.irq_set_affinity = litex_gpio_irq_set_affinity;

	gichip = &gpio_s->chip.irq;
	gichip->chip = &gpio_s->ichip;
	gichip->fwnode = of_node_to_fwnode(node);
	gichip->parent_domain = parent_domain;
	gichip->child_to_parent_hwirq = litex_gpio_child_to_parent_hwirq;
	gichip->handler = handle_bad_irq;
	gichip->default_type = IRQ_TYPE_NONE;
	gichip->parent_handler = litex_gpio_irq;
	gichip->parent_handler_data = gpio_s;
	gichip->num_parents = 1;
	gichip->parents = &gpio_s->irq_number;

	return 0;
}

static int litex_gpio_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct litex_gpio *gpio_s;
	struct resource *res;
	int ret_i;

	int dt_ngpio;
	const char *dt_direction;

	if (!node)
		return -ENODEV;

	gpio_s = devm_kzalloc(&pdev->dev, sizeof(*gpio_s), GFP_KERNEL);
	if (!gpio_s)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EBUSY;

	gpio_s->membase = devm_of_iomap(&pdev->dev, node, 0, &res->end);
	if (IS_ERR_OR_NULL(gpio_s->membase))
		return -EIO;

	spin_lock_init(&gpio_s->gpio_lock);

	ret_i = of_property_read_u32(node, "litex,ngpio", &dt_ngpio);
	if (ret_i < 0) {
		dev_err(&pdev->dev, "No litex,ngpio entry in the dts file\n");
		return -ENODEV;
	}
	if (dt_ngpio >= GPIO_PINS_MAX) {
		dev_err(&pdev->dev,
			"LiteX GPIO driver cannot use more than %d pins\n",
			GPIO_PINS_MAX);
		return -EINVAL;
	}

	ret_i = of_property_read_string(node, "litex,direction",
					      &dt_direction);
	if (ret_i < 0) {
		dev_err(&pdev->dev, "No litex,direction entry in the dts file\n");
		return -ENODEV;
	}

	if (!strcmp(dt_direction, "in"))
		gpio_s->port_direction = GPIOF_DIR_IN;
	else if (!strcmp(dt_direction, "out"))
		gpio_s->port_direction = GPIOF_DIR_OUT;
	else
		return -ENODEV;

	/* Assign API functions */

	gpio_s->chip.label             = "litex_gpio";
	gpio_s->chip.owner             = THIS_MODULE;
	gpio_s->chip.get               = litex_gpio_get_value;
	gpio_s->chip.get_multiple      = litex_gpio_get_multiple;
	gpio_s->chip.set               = litex_gpio_set_value;
	gpio_s->chip.set_multiple      = litex_gpio_set_multiple;
	gpio_s->chip.get_direction     = litex_gpio_get_direction;
	gpio_s->chip.direction_input   = litex_gpio_direction_input;
	gpio_s->chip.direction_output  = litex_gpio_direction_output;
	gpio_s->chip.parent            = &pdev->dev;
	gpio_s->chip.base              = -1;
	gpio_s->chip.ngpio             = dt_ngpio;
	gpio_s->chip.can_sleep         = false;

	gpio_s->reg_span = (dt_ngpio + LITEX_SUBREG_SIZE_BIT - 1) /
			   LITEX_SUBREG_SIZE_BIT;

	if (gpio_s->port_direction == GPIOF_DIR_IN) {
		ret_i = litex_gpio_init_irq(pdev, gpio_s);
		if (ret_i < 0)
			return ret_i;
	}

	platform_set_drvdata(pdev, gpio_s);
	return devm_gpiochip_add_data(&pdev->dev, &gpio_s->chip, gpio_s);
}

static const struct of_device_id litex_of_match[] = {
	{.compatible = "litex,gpio"},
	{},
};

MODULE_DEVICE_TABLE(of, litex_of_match);

static struct platform_driver litex_gpio_driver = {
	.driver = {
		.name             = "litex-gpio",
		.of_match_table   = of_match_ptr(litex_of_match)
	},
	.probe  = litex_gpio_probe,
};

module_platform_driver(litex_gpio_driver);

MODULE_DESCRIPTION("LiteX gpio driver");
MODULE_AUTHOR("Antmicro <www.antmicro.com>");
MODULE_LICENSE("GPL v2");
