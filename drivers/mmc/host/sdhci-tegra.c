/*
 * Copyright (C) 2010 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>

#include <mach/gpio.h>
#include <mach/sdhci.h>

#include "sdhci.h"
#include "sdhci-pltfm.h"

#define SDHCI_VENDOR_CLOCK_CNTRL       0x100
#define SDHCI_VENDOR_CLOCK_CNTRL_PADPIPE_CLKEN_OVERRIDE	0x8

static void tegra_3x_sdhci_set_card_clock(struct sdhci_host *sdhci, unsigned int clock);

struct tegra_sdhci_hw_ops{
	/* Set the internal clk and card clk.*/
	void	(*set_card_clock)(struct sdhci_host *sdhci, unsigned int clock);
};

static struct tegra_sdhci_hw_ops tegra_2x_sdhci_ops = {
};

static struct tegra_sdhci_hw_ops tegra_3x_sdhci_ops = {
	.set_card_clock = tegra_3x_sdhci_set_card_clock,
};

struct tegra_sdhci_host {
	bool	clk_enabled;
	char	wp_gpio_name[32];
	char	cd_gpio_name[32];
	char	power_gpio_name[32];
	struct regulator *vdd_io_reg;
	struct regulator *vdd_slot_reg;
	/* Pointer to the chip specific HW ops */
	struct tegra_sdhci_hw_ops *hw_ops;
};

static u32 tegra_sdhci_readl(struct sdhci_host *host, int reg)
{
	u32 val;

	if (unlikely(reg == SDHCI_PRESENT_STATE)) {
		/* Use wp_gpio here instead? */
		val = readl(host->ioaddr + reg);
		return val | SDHCI_WRITE_PROTECT;
	}

	return readl(host->ioaddr + reg);
}

static u16 tegra_sdhci_readw(struct sdhci_host *host, int reg)
{
	if (unlikely(reg == SDHCI_HOST_VERSION)) {
		/* Erratum: Version register is invalid in HW. */
		return SDHCI_SPEC_200;
	}

	return readw(host->ioaddr + reg);
}

static void tegra_sdhci_writel(struct sdhci_host *host, u32 val, int reg)
{
	/* Seems like we're getting spurious timeout and crc errors, so
	 * disable signalling of them. In case of real errors software
	 * timers should take care of eventually detecting them.
	 */
	if (unlikely(reg == SDHCI_SIGNAL_ENABLE))
		val &= ~(SDHCI_INT_TIMEOUT|SDHCI_INT_CRC);

	writel(val, host->ioaddr + reg);

#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	if (unlikely(reg == SDHCI_INT_ENABLE)) {
		/* Erratum: Must enable block gap interrupt detection */
		u8 gap_ctrl = readb(host->ioaddr + SDHCI_BLOCK_GAP_CONTROL);
		if (val & SDHCI_INT_CARD_INT)
			gap_ctrl |= 0x8;
		else
			gap_ctrl &= ~0x8;
		writeb(gap_ctrl, host->ioaddr + SDHCI_BLOCK_GAP_CONTROL);
	}
#endif
}

static unsigned int tegra_sdhci_get_ro(struct sdhci_host *sdhci)
{
	struct platform_device *pdev = to_platform_device(mmc_dev(sdhci->mmc));
	struct tegra_sdhci_platform_data *plat;

	plat = pdev->dev.platform_data;

	if (!gpio_is_valid(plat->wp_gpio))
		return -1;

	return gpio_get_value(plat->wp_gpio);
}

static void sdhci_status_notify_cb(int card_present, void *dev_id)
{
	struct sdhci_host *sdhci = (struct sdhci_host *)dev_id;
	struct platform_device *pdev = to_platform_device(mmc_dev(sdhci->mmc));
	struct tegra_sdhci_platform_data *plat;
	unsigned int status, oldstat;

	pr_debug("%s: card_present %d\n", mmc_hostname(sdhci->mmc),
		card_present);

	plat = pdev->dev.platform_data;
	if (!plat->mmc_data.status) {
		mmc_detect_change(sdhci->mmc, 0);
		return;
	}

	status = plat->mmc_data.status(mmc_dev(sdhci->mmc));

	oldstat = plat->mmc_data.card_present;
	plat->mmc_data.card_present = status;
	if (status ^ oldstat) {
		pr_debug("%s: Slot status change detected (%d -> %d)\n",
			mmc_hostname(sdhci->mmc), oldstat, status);
		if (status && !plat->mmc_data.built_in)
			mmc_detect_change(sdhci->mmc, (5 * HZ) / 2);
		else
			mmc_detect_change(sdhci->mmc, 0);
	}
}

static irqreturn_t carddetect_irq(int irq, void *data)
{
	struct sdhci_host *sdhost = (struct sdhci_host *)data;

	tasklet_schedule(&sdhost->card_tasklet);
	return IRQ_HANDLED;
};

static int tegra_sdhci_8bit(struct sdhci_host *host, int bus_width)
{
	struct platform_device *pdev = to_platform_device(mmc_dev(host->mmc));
	struct tegra_sdhci_platform_data *plat;
	u32 ctrl;

	plat = pdev->dev.platform_data;

	ctrl = sdhci_readb(host, SDHCI_HOST_CONTROL);
	if (plat->is_8bit && bus_width == MMC_BUS_WIDTH_8) {
		ctrl &= ~SDHCI_CTRL_4BITBUS;
		ctrl |= SDHCI_CTRL_8BITBUS;
	} else {
		ctrl &= ~SDHCI_CTRL_8BITBUS;
		if (bus_width == MMC_BUS_WIDTH_4)
			ctrl |= SDHCI_CTRL_4BITBUS;
		else
			ctrl &= ~SDHCI_CTRL_4BITBUS;
	}
	sdhci_writeb(host, ctrl, SDHCI_HOST_CONTROL);
	return 0;
}

static void tegra_3x_sdhci_set_card_clock(struct sdhci_host *sdhci, unsigned int clock)
{
	int div;
	u16 clk;
	unsigned long timeout;
	u8 ctrl;

	if (clock && clock == sdhci->clock)
		return;

	sdhci_writew(sdhci, 0, SDHCI_CLOCK_CONTROL);

	if (clock == 0)
		goto out;

	if (sdhci->version >= SDHCI_SPEC_300) {
		/* Version 3.00 divisors must be a multiple of 2. */
		if (sdhci->max_clk <= clock) {
			div = 1;
		} else {
			for (div = 2; div < SDHCI_MAX_DIV_SPEC_300; div += 2) {
				if ((sdhci->max_clk / div) <= clock)
					break;
			}
		}
	} else {
		/* Version 2.00 divisors must be a power of 2. */
		for (div = 1; div < SDHCI_MAX_DIV_SPEC_200; div *= 2) {
			if ((sdhci->max_clk / div) <= clock)
				break;
		}
	}
	div >>= 1;

	/*
	 * Tegra3 sdmmc controller internal clock will not be stabilized when
	 * we use a clock divider value greater than 4. The WAR is as follows.
	 * - Enable PADPIPE_CLK_OVERRIDE in the vendr clk cntrl register.
	 * - Enable internal clock.
	 * - Wait for 5 usec and do a dummy write.
	 * - Poll for clk stable and disable PADPIPE_CLK_OVERRIDE.
	 */

	/* Enable PADPIPE clk override */
	ctrl = sdhci_readb(sdhci, SDHCI_VENDOR_CLOCK_CNTRL);
	ctrl |= SDHCI_VENDOR_CLOCK_CNTRL_PADPIPE_CLKEN_OVERRIDE;
	sdhci_writeb(sdhci, ctrl, SDHCI_VENDOR_CLOCK_CNTRL);

	clk = (div & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT;
	clk |= ((div & SDHCI_DIV_HI_MASK) >> SDHCI_DIV_MASK_LEN)
		<< SDHCI_DIVIDER_HI_SHIFT;
	clk |= SDHCI_CLOCK_INT_EN;
	sdhci_writew(sdhci, clk, SDHCI_CLOCK_CONTROL);

	/* Wait for 5 usec */
	udelay(5);

	/* Do a dummy write */
	ctrl = sdhci_readb(sdhci, SDHCI_CAPABILITIES);
	ctrl |= 1;
	sdhci_writeb(sdhci, ctrl, SDHCI_CAPABILITIES);

	/* Wait max 20 ms */
	timeout = 20;
	while (!((clk = sdhci_readw(sdhci, SDHCI_CLOCK_CONTROL))
		& SDHCI_CLOCK_INT_STABLE)) {
		if (timeout == 0) {
			dev_err(mmc_dev(sdhci->mmc), "Internal clock never stabilised\n");
			return;
		}
		timeout--;
		mdelay(1);
	}

	/* Disable PADPIPE clk override */
	ctrl = sdhci_readb(sdhci, SDHCI_VENDOR_CLOCK_CNTRL);
	ctrl &= ~SDHCI_VENDOR_CLOCK_CNTRL_PADPIPE_CLKEN_OVERRIDE;
	sdhci_writeb(sdhci, ctrl, SDHCI_VENDOR_CLOCK_CNTRL);

	clk |= SDHCI_CLOCK_CARD_EN;
	sdhci_writew(sdhci, clk, SDHCI_CLOCK_CONTROL);

out:
	sdhci->clock = clock;
}

static void tegra_sdhci_set_clock(struct sdhci_host *sdhci, unsigned int clock)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(sdhci);
	struct tegra_sdhci_host *tegra_host = pltfm_host->priv;

	pr_debug("%s %s %u enabled=%u\n", __func__,
		mmc_hostname(sdhci->mmc), clock, tegra_host->clk_enabled);

	if (clock) {
		if (!tegra_host->clk_enabled) {
			clk_enable(pltfm_host->clk);
			sdhci_writeb(sdhci, 1, SDHCI_VENDOR_CLOCK_CNTRL);
			tegra_host->clk_enabled = true;
		}
		if (tegra_host->hw_ops->set_card_clock)
			tegra_host->hw_ops->set_card_clock(sdhci, clock);
	} else if (!clock && tegra_host->clk_enabled) {
		if (tegra_host->hw_ops->set_card_clock)
			tegra_host->hw_ops->set_card_clock(sdhci, clock);
		sdhci_writeb(sdhci, 0, SDHCI_VENDOR_CLOCK_CNTRL);
		clk_disable(pltfm_host->clk);
		tegra_host->clk_enabled = false;
	}
}

static int tegra_sdhci_pltfm_init(struct sdhci_host *host,
				  struct sdhci_pltfm_data *pdata)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct platform_device *pdev = to_platform_device(mmc_dev(host->mmc));
	struct tegra_sdhci_platform_data *plat;
	struct tegra_sdhci_host *tegra_host;
	struct clk *clk;
	int rc;
	void __iomem *ioaddr_clk_rst;
	unsigned int val = 0;

	ioaddr_clk_rst = ioremap(0x60006300, 0x400);
	val = readl(ioaddr_clk_rst + 0xa0);
	val |= 0x68;
	writel(val, ioaddr_clk_rst + 0xa0);

	plat = pdev->dev.platform_data;
	if (plat == NULL) {
		dev_err(mmc_dev(host->mmc), "missing platform data\n");
		return -ENXIO;
	}

	tegra_host = kzalloc(sizeof(struct tegra_sdhci_host), GFP_KERNEL);
	if (tegra_host == NULL) {
		dev_err(mmc_dev(host->mmc), "failed to allocate tegra host\n");
		return -ENOMEM;
	}

#ifdef CONFIG_MMC_EMBEDDED_SDIO
	if (plat->mmc_data.embedded_sdio)
		mmc_set_embedded_sdio_data(host->mmc,
			&plat->mmc_data.embedded_sdio->cis,
			&plat->mmc_data.embedded_sdio->cccr,
			plat->mmc_data.embedded_sdio->funcs,
			plat->mmc_data.embedded_sdio->num_funcs);
#endif

	if (gpio_is_valid(plat->power_gpio)) {
		snprintf(tegra_host->power_gpio_name,sizeof(tegra_host->power_gpio_name),"sdhci%d_power",pdev->id);
		rc = gpio_request(plat->power_gpio, tegra_host->power_gpio_name);
		if (rc) {
			dev_err(mmc_dev(host->mmc),
				"failed to allocate power gpio\n");
			goto out;
		}
		tegra_gpio_enable(plat->power_gpio);
		gpio_direction_output(plat->power_gpio, 1);
	}

	if (gpio_is_valid(plat->cd_gpio)) {
		snprintf(tegra_host->cd_gpio_name,sizeof(tegra_host->cd_gpio_name),"sdhci%d_cd",pdev->id);
		rc = gpio_request(plat->cd_gpio, tegra_host->cd_gpio_name);
		if (rc) {
			dev_err(mmc_dev(host->mmc),
				"failed to allocate cd gpio\n");
			goto out_power;
		}
		tegra_gpio_enable(plat->cd_gpio);
		gpio_direction_input(plat->cd_gpio);

		rc = request_irq(gpio_to_irq(plat->cd_gpio), carddetect_irq,
				 IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
				 mmc_hostname(host->mmc), host);

		if (rc)	{
			dev_err(mmc_dev(host->mmc), "request irq error\n");
			goto out_cd;
		}

	} else if (plat->mmc_data.register_status_notify) {
		plat->mmc_data.register_status_notify(sdhci_status_notify_cb, host);
	}

	if (plat->mmc_data.status) {
		plat->mmc_data.card_present = plat->mmc_data.status(mmc_dev(host->mmc));
	}

	if (gpio_is_valid(plat->wp_gpio)) {
		snprintf(tegra_host->wp_gpio_name,sizeof(tegra_host->wp_gpio_name),"sdhci%d_wp",pdev->id);
		rc = gpio_request(plat->wp_gpio, tegra_host->wp_gpio_name);
		if (rc) {
			dev_err(mmc_dev(host->mmc),
				"failed to allocate wp gpio\n");
			goto out_irq;
		}
		tegra_gpio_enable(plat->wp_gpio);
		gpio_direction_input(plat->wp_gpio);
	}

	if (!plat->mmc_data.built_in && !plat->has_no_vreg) {
		tegra_host->vdd_io_reg = regulator_get(mmc_dev(host->mmc), "vddio_sdmmc");
		if (WARN_ON(IS_ERR_OR_NULL(tegra_host->vdd_io_reg))) {
			dev_err(mmc_dev(host->mmc), "%s regulator not found: %ld\n",
				"vddio_sdmmc", PTR_ERR(tegra_host->vdd_io_reg));
			tegra_host->vdd_io_reg = NULL;
		} else {
			rc = regulator_set_voltage(tegra_host->vdd_io_reg,
				3280000, 3320000);
			if (rc) {
				dev_err(mmc_dev(host->mmc), "%s regulator_set_voltage failed: %d",
					"vddio_sdmmc", rc);
			} else {
				regulator_enable(tegra_host->vdd_io_reg);
			}
		}

		tegra_host->vdd_slot_reg = regulator_get(mmc_dev(host->mmc), "vddio_sd_slot");
		if (WARN_ON(IS_ERR_OR_NULL(tegra_host->vdd_slot_reg))) {
			dev_err(mmc_dev(host->mmc), "%s regulator not found: %ld\n",
				"vddio_sd_slot", PTR_ERR(tegra_host->vdd_slot_reg));
			tegra_host->vdd_slot_reg = NULL;
		} else {
			regulator_enable(tegra_host->vdd_slot_reg);
		}
	}

	clk = clk_get(mmc_dev(host->mmc), NULL);
	if (IS_ERR(clk)) {
		dev_err(mmc_dev(host->mmc), "clk err\n");
		rc = PTR_ERR(clk);
		goto out_wp;
	}
	rc = clk_enable(clk);
	if (rc != 0)
		goto err_clkput;
	pltfm_host->clk = clk;
	pltfm_host->priv = tegra_host;
	tegra_host->clk_enabled = true;

	host->mmc->caps |= MMC_CAP_ERASE;
	if (plat->is_8bit)
		host->mmc->caps |= MMC_CAP_8_BIT_DATA;

	host->mmc->pm_caps = MMC_PM_KEEP_POWER | MMC_PM_IGNORE_PM_NOTIFY;
	if (plat->mmc_data.built_in) {
		host->mmc->caps |= MMC_CAP_NONREMOVABLE;
		host->mmc->pm_flags = MMC_PM_IGNORE_PM_NOTIFY;
	}

#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	tegra_host->hw_ops = &tegra_2x_sdhci_ops;
#else
	tegra_host->hw_ops = &tegra_3x_sdhci_ops;
#endif

	return 0;

err_clkput:
	clk_put(clk);

out_wp:
	if (gpio_is_valid(plat->wp_gpio)) {
		tegra_gpio_disable(plat->wp_gpio);
		gpio_free(plat->wp_gpio);
	}

out_irq:
	if (gpio_is_valid(plat->cd_gpio))
		free_irq(gpio_to_irq(plat->cd_gpio), host);
out_cd:
	if (gpio_is_valid(plat->cd_gpio)) {
		tegra_gpio_disable(plat->cd_gpio);
		gpio_free(plat->cd_gpio);
	}

out_power:
	if (gpio_is_valid(plat->power_gpio)) {
		tegra_gpio_disable(plat->power_gpio);
		gpio_free(plat->power_gpio);
	}

out:
	kfree(tegra_host);
	return rc;
}

static void tegra_sdhci_pltfm_exit(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct platform_device *pdev = to_platform_device(mmc_dev(host->mmc));
	struct tegra_sdhci_host *tegra_host = pltfm_host->priv;
	struct tegra_sdhci_platform_data *plat;

	plat = pdev->dev.platform_data;

	if (tegra_host->vdd_slot_reg) {
		regulator_disable(tegra_host->vdd_slot_reg);
		regulator_put(tegra_host->vdd_slot_reg);
	}

	if (tegra_host->vdd_io_reg) {
		regulator_disable(tegra_host->vdd_io_reg);
		regulator_put(tegra_host->vdd_io_reg);
	}

	if (gpio_is_valid(plat->wp_gpio)) {
		tegra_gpio_disable(plat->wp_gpio);
		gpio_free(plat->wp_gpio);
	}

	if (gpio_is_valid(plat->cd_gpio)) {
		free_irq(gpio_to_irq(plat->cd_gpio), host);
		tegra_gpio_disable(plat->cd_gpio);
		gpio_free(plat->cd_gpio);
	}

	if (gpio_is_valid(plat->power_gpio)) {
		tegra_gpio_disable(plat->power_gpio);
		gpio_free(plat->power_gpio);
	}

	if (tegra_host->clk_enabled)
		clk_disable(pltfm_host->clk);
	clk_put(pltfm_host->clk);

	kfree(tegra_host);
}

static int tegra_sdhci_suspend(struct sdhci_host *sdhci, pm_message_t state)
{
	tegra_sdhci_set_clock(sdhci, 0);

	return 0;
}

static int tegra_sdhci_resume(struct sdhci_host *sdhci)
{
	/* Setting the min identification clock of freq 400KHz */
	tegra_sdhci_set_clock(sdhci, 400000);

	return 0;
}

static struct sdhci_ops tegra_sdhci_ops = {
	.get_ro     = tegra_sdhci_get_ro,
	.read_l     = tegra_sdhci_readl,
	.read_w     = tegra_sdhci_readw,
	.write_l    = tegra_sdhci_writel,
	.platform_8bit_width = tegra_sdhci_8bit,
	.set_clock  = tegra_sdhci_set_clock,
	.suspend    = tegra_sdhci_suspend,
	.resume     = tegra_sdhci_resume,
};

struct sdhci_pltfm_data sdhci_tegra_pdata = {
	.quirks = SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
#ifndef CONFIG_ARCH_TEGRA_2x_SOC
		  SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK |
#endif
#ifdef CONFIG_ARCH_TEGRA_3x_SOC
		  SDHCI_QUIRK_NONSTANDARD_CLOCK |
#endif
		  SDHCI_QUIRK_SINGLE_POWER_WRITE |
		  SDHCI_QUIRK_NO_HISPD_BIT |
		  SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC,
	.ops  = &tegra_sdhci_ops,
	.init = tegra_sdhci_pltfm_init,
	.exit = tegra_sdhci_pltfm_exit,
};
