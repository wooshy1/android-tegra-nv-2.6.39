/*
 * drivers/video/tegra/host/nvhost_cpuaccess.c
 *
 * Tegra Graphics Host Cpu Register Access
 *
 * Copyright (c) 2010, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "nvhost_cpuaccess.h"
#include "dev.h"
#include <linux/string.h>

int nvhost_cpuaccess_init(struct nvhost_cpuaccess *ctx,
			struct platform_device *pdev)
{
	struct nvhost_master *host = cpuaccess_to_dev(ctx);
	int i;

	for (i = 0; i < host->nb_modules; i++) {
		struct resource *mem;
		mem = platform_get_resource(pdev, IORESOURCE_MEM, i+1);
		if (!mem) {
			dev_err(&pdev->dev, "missing module memory resource\n");
			return -ENXIO;
		}
		ctx->reg_mem[i] = mem;
		ctx->regs[i] = ioremap(mem->start, resource_size(mem));
		if (!ctx->regs[i]) {
			dev_err(&pdev->dev, "failed to map module registers\n");
			return -ENXIO;
		}
	}

	return 0;
}

void nvhost_cpuaccess_deinit(struct nvhost_cpuaccess *ctx)
{
	struct nvhost_master *host = cpuaccess_to_dev(ctx);
	int i;

	for (i = 0; i < host->nb_modules; i++) {
		iounmap(ctx->regs[i]);
		release_resource(ctx->reg_mem[i]);
	}
}

int nvhost_mutex_try_lock(struct nvhost_cpuaccess *ctx, unsigned int idx)
{
	struct nvhost_master *dev = cpuaccess_to_dev(ctx);
	u32 reg;
	BUG_ON(!cpuaccess_op(ctx).mutex_try_lock);

	nvhost_module_busy(&dev->mod);
	reg = cpuaccess_op(ctx).mutex_try_lock(ctx, idx);
	if (reg) {
		nvhost_module_idle(&dev->mod);
		return -ERESTARTSYS;
	}
	atomic_inc(&ctx->lock_counts[idx]);
	return 0;
}

void nvhost_mutex_unlock(struct nvhost_cpuaccess *ctx, unsigned int idx)
{
	struct nvhost_master *dev = cpuaccess_to_dev(ctx);
	BUG_ON(!cpuaccess_op(ctx).mutex_unlock);

	cpuaccess_op(ctx).mutex_unlock(ctx, idx);
	nvhost_module_idle(&dev->mod);
	atomic_dec(&ctx->lock_counts[idx]);
}

void nvhost_read_module_regs(struct nvhost_cpuaccess *ctx, u32 module,
			u32 offset, size_t size, void *values)
{
	struct nvhost_master *dev = cpuaccess_to_dev(ctx);
	void __iomem *p = ctx->regs[module] + offset;
	u32 *out = (u32 *)values;
	BUG_ON(size & 3);
	size >>= 2;
	nvhost_module_busy(&dev->mod);
	while (size--) {
		*(out++) = readl(p);
		p += 4;
	}
	rmb();
	nvhost_module_idle(&dev->mod);
}

void nvhost_write_module_regs(struct nvhost_cpuaccess *ctx, u32 module,
			u32 offset, size_t size, const void *values)
{
	struct nvhost_master *dev = cpuaccess_to_dev(ctx);
	void __iomem *p = ctx->regs[module] + offset;
	const u32 *in = (const u32 *)values;
	BUG_ON(size & 3);
	size >>= 2;
	nvhost_module_busy(&dev->mod);
	while (size--) {
		writel(*(in++), p);
		p += 4;
	}
	wmb();
	nvhost_module_idle(&dev->mod);
}
