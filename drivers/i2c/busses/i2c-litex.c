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
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/bits.h>
#include <linux/litex.h>

#define REGISTER_SIZE           1
#define OFFSET_REG_W            0x0
#define OFFSET_REG_R            0x4

#define BITPOS_SCL              0
#define BITPOS_SDA_DIR          1
#define BITPOS_SDA_W            2
#define BITPOS_SDA_R            0

#define SETDIR_SDA_OUTPUT       1
#define SETDIR_SDA_INPUT        0

#define DRIVER_ALGO_BIT_UDELAY  20

struct litex_i2c {
	void __iomem              *reg_w;
	void __iomem              *reg_r;
	struct i2c_adapter        adapter;
	struct i2c_algo_bit_data  algo_data;
};

/* Helper functions */

static inline void litex_set_bit(void __iomem *mem, int bit, int val)
{
	u32 regv, new_regv;

	regv = litex_read8(mem);
	new_regv = (regv & ~BIT(bit)) | ((!!val) << bit);
	litex_write8(mem, new_regv);
}

static inline int litex_get_bit(void __iomem *mem, int bit)
{
	u32 regv;

	regv = litex_read8(mem);
	return !!(regv & BIT(bit));
}

/* API functions */

static void litex_i2c_setscl(void *data, int state)
{
	struct litex_i2c *i2c = (struct litex_i2c *) data;

	litex_set_bit(i2c->reg_w, BITPOS_SCL, state);
}

static void litex_i2c_setsda(void *data, int state)
{
	struct litex_i2c *i2c = (struct litex_i2c *) data;

	litex_set_bit(i2c->reg_w, BITPOS_SDA_DIR, SETDIR_SDA_OUTPUT);
	litex_set_bit(i2c->reg_w, BITPOS_SDA_W, state);
}

static int litex_i2c_getsda(void *data)
{
	struct litex_i2c *i2c = (struct litex_i2c *) data;

	litex_set_bit(i2c->reg_w, BITPOS_SDA_DIR, SETDIR_SDA_INPUT);
	return litex_get_bit(i2c->reg_r, BITPOS_SDA_R);
}

/* Driver functions */

static int litex_i2c_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	void __iomem *membase;
	struct litex_i2c *i2c_s;
	struct resource *res;

	if (!node)
		return -ENODEV;

	i2c_s = devm_kzalloc(&pdev->dev, sizeof(*i2c_s), GFP_KERNEL);
	if (!i2c_s)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EBUSY;

	membase = devm_of_iomap(&pdev->dev, node, 0, &res->end);
	if (IS_ERR_OR_NULL(membase))
		return -EIO;

	i2c_s->reg_w = membase + OFFSET_REG_W;
	i2c_s->reg_r = membase + OFFSET_REG_R;

	strncpy(i2c_s->adapter.name, "litex_i2c_adapter",
		sizeof(i2c_s->adapter.name));
	i2c_s->adapter.owner        = THIS_MODULE;
	i2c_s->adapter.algo_data    = &i2c_s->algo_data;
	i2c_s->adapter.dev.parent   = &pdev->dev;
	i2c_s->adapter.dev.of_node  = node;
	i2c_s->algo_data.data       = i2c_s;

	i2c_s->algo_data.setsda  = litex_i2c_setsda;
	i2c_s->algo_data.setscl  = litex_i2c_setscl;
	i2c_s->algo_data.getsda  = litex_i2c_getsda;
	i2c_s->algo_data.getscl  = NULL;
	i2c_s->algo_data.udelay  = DRIVER_ALGO_BIT_UDELAY;
	i2c_s->algo_data.timeout = HZ;

	platform_set_drvdata(pdev, i2c_s);
	return i2c_bit_add_bus(&i2c_s->adapter);
}

static const struct of_device_id litex_of_match[] = {
	{.compatible = "litex,i2c"},
	{},
};

MODULE_DEVICE_TABLE(of, litex_of_match);

static struct platform_driver litex_i2c_driver = {
	.driver		= {
		.name		= "litex-i2c",
		.of_match_table = of_match_ptr(litex_of_match)
	},
	.probe		= litex_i2c_probe,
};

module_platform_driver(litex_i2c_driver);

MODULE_DESCRIPTION("LiteX bitbang I2C Bus driver");
MODULE_AUTHOR("Antmicro <www.antmicro.com>");
