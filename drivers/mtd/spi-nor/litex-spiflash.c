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

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/spi-nor.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/jiffies.h>
#include <linux/litex.h>

#define SPIFLASH_BITBANG_OFFSET 0x0
#define SPIFLASH_BITBANG_SIZE 0x1
#define SPIFLASH_MISO_OFFSET 0x4
#define SPIFLASH_MISO_SIZE 0x1
#define SPIFLASH_BITBANG_EN_OFFSET 0x8
#define SPIFLASH_BITBANG_EN_SIZE 0x1

#define SPIFLASH_ENABLE 0x01
#define SPIFLASH_DISABLE 0x00
#define CLK_ENABLE 0x02
#define CS_ENABLE 0x04
#define MISO_MODE 0x08

#define WRITE_ENABLE 0x06
#define READ_STATUS_REGISTER 0x05
#define WORK_IN_PROGRESS 0x01
#define READ_FLAG_STATUS_REGISTER 0x70
#define PROGRAM_ERR 0x10
#define ERASE_BUSY 0x80
#define ERASE_ERR 0x20

#define TIMEOUT_ERASE_MS 3000
#define TIMEOUT_MS 50
#define BIT_SHIFT 7
#define ADDRESS_SIZE 3
#define DUMMY_CYCLES 8

struct spi {
	struct spi_nor nor;
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
};

static void cs(struct spi_nor *nor, u8 new_val)
{
	struct spi *spi = nor->priv;
	u8 curr_val = litex_read8(spi->base + SPIFLASH_BITBANG_OFFSET);
	u8 set_val = new_val == CS_ENABLE ?
		curr_val | new_val : curr_val & new_val;

	litex_write8(spi->base + SPIFLASH_BITBANG_OFFSET, set_val);
}

static void clk(struct spi_nor *nor, u8 new_val)
{
	struct spi *spi = nor->priv;
	u8 curr_val = litex_read8(spi->base + SPIFLASH_BITBANG_OFFSET);
	u8 set_val = new_val == CLK_ENABLE ?
		curr_val | new_val : curr_val & new_val;

	litex_write8(spi->base + SPIFLASH_BITBANG_OFFSET, set_val);
}

static u8 miso_read(struct spi_nor *nor)
{
	struct spi *spi = nor->priv;

	return litex_read8(spi->base + SPIFLASH_MISO_OFFSET) & 0x1;
}

static void mosi_set(bool mosi, struct spi_nor *nor)
{
	struct spi *spi = nor->priv;
	u8 curr_val = litex_read8(spi->base + SPIFLASH_BITBANG_OFFSET);
	u8 set_val = mosi ? curr_val | (0x01) : curr_val & (~0x01);

	litex_write8(spi->base + SPIFLASH_BITBANG_OFFSET, set_val);
}

static void enable(struct spi_nor *nor, u8 new_val)
{
	struct spi *spi = nor->priv;
	u8 curr_val = litex_read8(spi->base + SPIFLASH_BITBANG_OFFSET);
	u8 set_val = new_val == MISO_MODE ?
		curr_val | new_val : curr_val & new_val;

	litex_write8(spi->base + SPIFLASH_BITBANG_OFFSET, set_val);
}

static void initial_config(struct spi_nor *nor)
{
	struct spi *spi = nor->priv;

	clk(nor, ~CLK_ENABLE);
	cs(nor, CS_ENABLE);
	enable(nor, ~MISO_MODE);
	litex_write8(spi->base + SPIFLASH_BITBANG_EN_OFFSET, SPIFLASH_ENABLE);
}

static void dummy_cycles(struct spi_nor *nor, int n_cycles)
{
	int i;

	for (i = 0; i < n_cycles; i++) {
		clk(nor, ~CLK_ENABLE);
		clk(nor, CLK_ENABLE);
	}
}

static void spi_bitbang_send(struct spi_nor *nor, const u8 command)
{
	int i;
	u8 c = command;

	for (i = BIT_SHIFT; i >= 0; i--) {
		// Sending on MSB order
		mosi_set(c & 0x80, nor);
		c <<= 1;
		clk(nor, ~CLK_ENABLE);
		clk(nor, CLK_ENABLE);
	}
}

static u8 spi_bitbang_read(struct spi_nor *nor)
{
	int i;
	u8 byte = 0x00;

	for (i = BIT_SHIFT; i >= 0; i--) {
		clk(nor, ~CLK_ENABLE);
		clk(nor, CLK_ENABLE);
		byte |= (miso_read(nor) << i);
	}
	return byte;
}

static void write_command(struct spi_nor *nor, const u8 command)
{
	enable(nor, ~MISO_MODE);
	dummy_cycles(nor, DUMMY_CYCLES);
	cs(nor, ~CS_ENABLE);
	spi_bitbang_send(nor, command);
}

static void write_address(struct spi_nor *nor, const u32 addr32)
{
	int i;
	u8 *addr8 = (u8 *) &addr32;

	for (i = (ADDRESS_SIZE - 1); i >= 0; i--)
		spi_bitbang_send(nor, *(addr8 + i));
}

static void write_data(struct spi_nor *nor, const u8 *data, int len)
{
	int i;

	enable(nor, ~MISO_MODE);
	for (i = 0; i < len; i++)
		spi_bitbang_send(nor, data[i]);
}

static void read_data(struct spi_nor *nor, u8 *buffer, int len)
{
	int i;

	enable(nor, MISO_MODE);
	for (i = 0; i < len; i++)
		buffer[i] = spi_bitbang_read(nor);
}

static int spi_flash_nor_read_reg(struct spi_nor *nor, u8 opcode, u8 *buf,
				  size_t len)
{
	write_command(nor, opcode);
	read_data(nor, buf, len);
	cs(nor, CS_ENABLE);
	return 0;
}

static u8 read_status(struct spi_nor *nor, const u8 status_command)
{
	u8 status;

	spi_flash_nor_read_reg(nor, status_command, &status, 1);
	return status;
}

static int busy(struct spi_nor *nor, int time, u8 reg, u8 flag)
{
	unsigned long end = jiffies + msecs_to_jiffies(time);
	/* Wait until device is ready */
	while (read_status(nor, reg) & flag) {
		if (time_after(jiffies, end))
			return -ETIMEDOUT;
	};

	return 0;
}

static int spi_flash_nor_erase(struct spi_nor *nor, loff_t offs)
{
	write_command(nor, WRITE_ENABLE);
	cs(nor, CS_ENABLE);

	if(busy(nor, TIMEOUT_MS, READ_STATUS_REGISTER, WORK_IN_PROGRESS) != 0)
		return -ETIMEDOUT;

	write_command(nor, nor->erase_opcode);
	write_address(nor, offs);
	cs(nor, CS_ENABLE);

	if(busy(nor, TIMEOUT_ERASE_MS, READ_FLAG_STATUS_REGISTER, ERASE_BUSY) != 0)
		return -ETIMEDOUT;

	if(busy(nor, TIMEOUT_ERASE_MS, READ_STATUS_REGISTER, WORK_IN_PROGRESS) != 0)
		return -ETIMEDOUT;

	return (read_status(nor, READ_FLAG_STATUS_REGISTER) & ERASE_ERR);
}

static ssize_t spi_flash_nor_read(struct spi_nor *nor, loff_t from,
		size_t length, u8 *buffer)
{
	write_command(nor, nor->read_opcode);
	write_address(nor, from);
	read_data(nor, buffer, length);
	cs(nor, CS_ENABLE);

	return length;
}

static ssize_t spi_flash_nor_write(struct spi_nor *nor, loff_t to, size_t len,
			     const u8 *buf)
{
	/* Write WRITE ENABLE command*/
	write_command(nor, WRITE_ENABLE);
	cs(nor, CS_ENABLE);

	if(busy(nor, TIMEOUT_MS, READ_STATUS_REGISTER, WORK_IN_PROGRESS) != 0)
		return -ETIMEDOUT;

	write_command(nor, nor->program_opcode);
	write_address(nor, to);
	write_data(nor, buf, len);
	cs(nor, CS_ENABLE);

	if(busy(nor, TIMEOUT_MS, READ_STATUS_REGISTER, WORK_IN_PROGRESS) != 0)
		return -ETIMEDOUT;

	if (read_status(nor, READ_FLAG_STATUS_REGISTER)&PROGRAM_ERR)
		return -EINVAL;

	return len;
}

static ssize_t spi_flash_nor_write_reg(struct spi_nor *nor, u8 opcode,
				       const u8 *buf, size_t len)
{
	/* Write WRITE ENABLE command*/
	write_command(nor, WRITE_ENABLE);
	cs(nor, CS_ENABLE);

	if(busy(nor, TIMEOUT_MS, READ_STATUS_REGISTER, WORK_IN_PROGRESS) != 0)
		return -ETIMEDOUT;

	write_command(nor, opcode);
	write_data(nor, buf, len);
	cs(nor, CS_ENABLE);

	if(busy(nor, TIMEOUT_MS, READ_STATUS_REGISTER, WORK_IN_PROGRESS) != 0)
		return -ETIMEDOUT;

	return 0;
}

static const struct spi_nor_controller_ops litex_spi_controller_ops = {
	.read = spi_flash_nor_read,
	.write = spi_flash_nor_write,
	.read_reg = spi_flash_nor_read_reg,
	.write_reg = spi_flash_nor_write_reg,
	.erase = spi_flash_nor_erase,
};

static int litex_spi_flash_probe(struct platform_device *pdev)
{
	struct device_node *node;
	struct resource *res;
	int ret;
	struct spi *spi;
	struct spi_nor *nor;

	const struct spi_nor_hwcaps hwcaps = {
		.mask = SNOR_HWCAPS_READ |
			SNOR_HWCAPS_READ_FAST |
			SNOR_HWCAPS_PP,
	};

	if (!pdev->dev.of_node) {
		dev_err(&pdev->dev, "No DT found\n");
		return -EINVAL;
	}

	spi = devm_kzalloc(&pdev->dev, sizeof(*spi), GFP_KERNEL);
	if (!spi)
		return -ENOMEM;
	platform_set_drvdata(pdev, spi);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	spi->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(spi->base))
		return PTR_ERR(spi->base);

	spi->dev = &pdev->dev;

	/* Gets attached flash */
	node = of_get_next_available_child(pdev->dev.of_node, NULL);
	if (!node) {
		dev_err(&pdev->dev, "no SPI flash device to configure\n");
		ret = -ENODEV;
	}
	nor = &spi->nor;
	nor->dev = spi->dev;
	nor->priv = spi;
	spi_nor_set_flash_node(nor, node);
	/* Sets initial configuration of registers */
	initial_config(nor);
	/* Fill the hooks to spi nor */
	nor->controller_ops = &litex_spi_controller_ops;
	nor->mtd.name = "spi";

	ret = spi_nor_scan(nor, NULL, &hwcaps);
	if (ret) {
		dev_err(&pdev->dev, "SPI_NOR_SCAN FAILED\n");
		return ret;
	}

	ret = mtd_device_register(&nor->mtd, NULL, 0);
	if (ret) {
		dev_err(&pdev->dev, "Fail to register device\n");
		return ret;
	}

	return 0;
}

static int litex_spi_flash_remove(struct platform_device *pdev)
{
	struct spi *spi = platform_get_drvdata(pdev);
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	spi->base = devm_ioremap_resource(&pdev->dev, res);
	litex_write8(spi->base + SPIFLASH_BITBANG_EN_OFFSET, SPIFLASH_DISABLE);
	mtd_device_unregister(&spi->nor.mtd);

	return 0;
}

static const struct of_device_id litex_of_match[] = {
	{ .compatible = "litex,spiflash" },
	{}
};

MODULE_DEVICE_TABLE(of, litex_of_match);

static struct platform_driver litex_spi_flash_driver = {
	.probe	= litex_spi_flash_probe,
	.remove	= litex_spi_flash_remove,
	.driver	= {
		.name = "litex-spiflash",
		.of_match_table   = of_match_ptr(litex_of_match)
	},
};
module_platform_driver(litex_spi_flash_driver);

MODULE_DESCRIPTION("LiteX SPI Flash driver");
MODULE_AUTHOR("Antmicro <www.antmicro.com>");
