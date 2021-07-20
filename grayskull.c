#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <asm/io.h>

#include "module.h"
#include "grayskull.h"
#include "ttkmd_arc_if.h"

#define REG_IOMAP_BAR	0
#define REG_IOMAP_START 0x1FC00000	// Starting at PCI TLB config registers
#define REG_IOMAP_LEN   0x00400000	// Covering entire system register space

#define PCI_TLB_BAR 	0
#define PCI_TLB_START	0
#define PCI_TLB_LEN	(1u << 20)	// Map just TLB 0, it's 1MB.

#define PCI_TLB_CONFIG_OFFSET	(0x1FC00000 - REG_IOMAP_START)
#define ARC_ICCM_MEMORY_OFFSET	(0x1FE00000 - REG_IOMAP_START)
#define ARC_CSM_MEMORY_OFFSET	(0x1FE80000 - REG_IOMAP_START)
#define ARC_ROM_MEMORY_OFFSET	(0x1FF00000 - REG_IOMAP_START)
#define RESET_UNIT_REG_OFFSET	(0x1FF30000 - REG_IOMAP_START)

#define TTKMD_ARC_IF_OFFSET 0x77000

#define SCRATCH_REG(n) (0x60 + (n)*sizeof(u32))	/* byte offset */

#define POST_CODE_REG SCRATCH_REG(0)
#define POST_CODE_MASK ((u32)0x3FFF)
#define POST_CODE_ARC_SLEEP 2
#define POST_CODE_ARC_L2 0xC0DE0000
#define POST_CODE_ARC_L2_MASK 0xFFFF0000

#define SCRATCH_5_ARC_BOOTROM_DONE 0x60
#define SCRATCH_5_ARC_L2_DONE 0x0

#define ARC_MISC_CNTL_REG 0x100
#define ARC_MISC_CNTL_RESET_MASK (1 << 12)
#define ARC_MISC_CNTL_IRQ0_MASK (1 << 16)
#define ARC_UDMIAXI_REGION_REG 0x10C
#define ARC_UDMIAXI_REGION_ICCM(n) (0x3 * (n))
#define ARC_UDMIAXI_REGION_CSM 0x10


#define GPIO_PAD_VAL_REG 0x1B8
#define GPIO_ARC_SPI_BOOTROM_EN_MASK (1 << 12)


// Scratch register 5 is used for the firmware message protocol.
// Write 0xAA00 | message_id into scratch register 5, wait for message_id to appear.
// After reading the message, the firmware will immediately reset SR5 to 0 and write message_id when done.
// Appearance of any other value indicates a conflict with another message.
#define GS_FW_MESSAGE_PRESENT 0xAA00

#define GS_FW_MSG_GO_LONG_IDLE 0x54
#define GS_FW_MSG_SHUTDOWN 0x55
#define GS_FW_MSG_ASTATE0 0xA0
#define GS_FW_MSG_ASTATE1 0xA1
#define GS_FW_MSG_ASTATE3 0xA3
#define GS_FW_MSG_ASTATE5 0xA5

#define GS_ARC_L2_FW_NAME "tenstorrent_gs_arc_l2_fw.bin"
#define GS_ARC_L2_FW_SIZE_BYTES 0xF000
#define GS_WATCHDOG_FW_NAME "tenstorrent_gs_wdg_fw.bin"
#define GS_WATCHDOG_FW_SIZE_BYTES 0x1000
#define GS_WATCHDOG_FW_CORE_ID 3

static bool is_hardware_hung(u8 __iomem *reset_unit_regs) {
	return (ioread32(reset_unit_regs + SCRATCH_REG(6)) == 0xFFFFFFFF);
}

int wait_reg32_with_timeout(u8 __iomem* reset_unit_regs, u8 __iomem* reg,
			    u32 expected_val, u32 timeout_us) {
	// Scale poll_period for around 100 polls, and at least 10 us
	u32 poll_period_us = max((u32)10, timeout_us / 100);

	ktime_t end_time = ktime_add_us(ktime_get(), timeout_us);

	while (1) {
		u32 read_val = ioread32(reg);
		if (read_val == expected_val)
			return 0;

		if (read_val == 0xFFFFFFFFu && is_hardware_hung(reset_unit_regs))
			return -2;

		if (ktime_after(ktime_get(), end_time))
			return -1;

		usleep_range(poll_period_us, 2 * poll_period_us);
	}
}

bool grayskull_send_arc_fw_message(u8 __iomem* reset_unit_regs, u8 message_id, u32 timeout_us) {
	void __iomem *scratch_reg_5 = reset_unit_regs + SCRATCH_REG(5);
	void __iomem *arc_misc_cntl_reg = reset_unit_regs + ARC_MISC_CNTL_REG;
	u32 arc_misc_cntl;

	iowrite32(GS_FW_MESSAGE_PRESENT | message_id, scratch_reg_5);

	// Trigger IRQ to ARC
	arc_misc_cntl = ioread32(arc_misc_cntl_reg);
	iowrite32(arc_misc_cntl | ARC_MISC_CNTL_IRQ0_MASK, arc_misc_cntl_reg);

	if (wait_reg32_with_timeout(reset_unit_regs, scratch_reg_5, message_id, timeout_us) < 0) {
		printk(KERN_WARNING "Tenstorrent FW message timeout: %08X.\n", (unsigned int)message_id);
		return false;
	} else {
		return true;
	}
}

static bool arc_l2_is_running(u8 __iomem* reset_unit_regs) {
	u32 post_code = ioread32(reset_unit_regs + POST_CODE_REG);
	return ((post_code & POST_CODE_ARC_L2_MASK) == POST_CODE_ARC_L2);
}

static int grayskull_load_arc_fw(struct grayskull_device *gs_dev) {
	const struct firmware *firmware;
	int ret = 0;
	u32 reset_vector;
	u8 __iomem* reset_unit_regs = gs_dev->reset_unit_regs;
	u8 __iomem* fw_target_mem = gs_dev->reg_iomap + ARC_CSM_MEMORY_OFFSET;
	u8 __iomem* reset_vec_target_mem = gs_dev->reg_iomap + ARC_ROM_MEMORY_OFFSET;

	ret = request_firmware(&firmware, GS_ARC_L2_FW_NAME, &gs_dev->tt.pdev->dev);
	if (ret)
		goto grayskull_load_arc_fw_cleanup;

	if (firmware->size != GS_ARC_L2_FW_SIZE_BYTES) {
		ret = -EINVAL;
		goto grayskull_load_arc_fw_cleanup;
	}

	iowrite32(ARC_UDMIAXI_REGION_CSM, reset_unit_regs + ARC_UDMIAXI_REGION_REG);
	memcpy_toio(fw_target_mem, firmware->data, GS_ARC_L2_FW_SIZE_BYTES);
	reset_vector = le32_to_cpu(*(u32 *)firmware->data);
	iowrite32(reset_vector, reset_vec_target_mem);

grayskull_load_arc_fw_cleanup:
	release_firmware(firmware);
	return ret;
}

static int grayskull_load_watchdog_fw(struct grayskull_device *gs_dev) {
	const struct firmware *firmware;
	int ret = 0;
	u32 reset_vector;
	u8 __iomem* reset_unit_regs = gs_dev->reset_unit_regs;
	u8 __iomem* fw_target_mem = gs_dev->reg_iomap + ARC_ICCM_MEMORY_OFFSET;

	ret = request_firmware(&firmware, GS_WATCHDOG_FW_NAME, &gs_dev->tt.pdev->dev);
	if (ret)
		goto grayskull_load_watchdog_fw_cleanup;

	if (firmware->size != GS_WATCHDOG_FW_SIZE_BYTES) {
		ret = -EINVAL;
		goto grayskull_load_watchdog_fw_cleanup;
	}

	iowrite32(ARC_UDMIAXI_REGION_ICCM(GS_WATCHDOG_FW_CORE_ID),
		reset_unit_regs + ARC_UDMIAXI_REGION_REG);
	memcpy_toio(fw_target_mem, firmware->data, GS_WATCHDOG_FW_SIZE_BYTES);
	// Reset vector needs to be passed to FW through ttkmd_arc_if
	reset_vector = le32_to_cpu(*(u32 *)firmware->data);
	gs_dev->tt.watchdog_fw_reset_vec = reset_vector;
	iowrite32(ARC_UDMIAXI_REGION_CSM, reset_unit_regs + ARC_UDMIAXI_REGION_REG);

grayskull_load_watchdog_fw_cleanup:
	release_firmware(firmware);
	return ret;
}

static int grayskull_populate_arc_if(struct grayskull_device *gs_dev) {
	ttkmd_arc_if_u *ttkmd_arc_if = kzalloc(sizeof(ttkmd_arc_if_u), GFP_KERNEL);
	u8 __iomem* reset_unit_regs = gs_dev->reset_unit_regs;
	u8 __iomem* device_ttkmd_arc_if = gs_dev->reg_iomap + ARC_CSM_MEMORY_OFFSET + TTKMD_ARC_IF_OFFSET;

	if (ttkmd_arc_if == NULL)
		return -ENOMEM;

	// ARC is little-endian. Convert to little-endian so we can use memcpy_toio
	ttkmd_arc_if->f.magic_number[0] = cpu_to_le32(TTKMD_ARC_MAGIC_NUMBER_0);
	ttkmd_arc_if->f.magic_number[1] = cpu_to_le32(TTKMD_ARC_MAGIC_NUMBER_1);
	ttkmd_arc_if->f.version = cpu_to_le32(TTKMD_ARC_IF_VERSION);
	ttkmd_arc_if->f.stage2_init = arc_fw_stage2_init;
	ttkmd_arc_if->f.ddr_train_en = ddr_train_en;
	ttkmd_arc_if->f.ddr_freq_ovr = cpu_to_le32(ddr_frequency_override);
	ttkmd_arc_if->f.aiclk_ppm_en = aiclk_ppm_en;
	ttkmd_arc_if->f.aiclk_ppm_ovr = cpu_to_le32(aiclk_fmax_override);
	ttkmd_arc_if->f.feature_disable_ovr = cpu_to_le32(arc_fw_feat_dis_override);
	ttkmd_arc_if->f.watchdog_fw_en = watchdog_fw_en;
	ttkmd_arc_if->f.watchdog_fw_load = !watchdog_fw_override;
	ttkmd_arc_if->f.watchdog_fw_reset_vec =
		cpu_to_le32(gs_dev->tt.watchdog_fw_reset_vec);

	iowrite32(ARC_UDMIAXI_REGION_CSM, reset_unit_regs + ARC_UDMIAXI_REGION_REG);
	memcpy_toio(device_ttkmd_arc_if, ttkmd_arc_if, sizeof(ttkmd_arc_if_u));

	kfree(ttkmd_arc_if);
	return 0;
}

static int toggle_arc_reset(u8 __iomem* reset_unit_regs) {
	u32 arc_misc_cntl;
	arc_misc_cntl = ioread32(reset_unit_regs + ARC_MISC_CNTL_REG);
	iowrite32(arc_misc_cntl | ARC_MISC_CNTL_RESET_MASK,
			reset_unit_regs + ARC_MISC_CNTL_REG);
	udelay(1);
	iowrite32(arc_misc_cntl & ~ARC_MISC_CNTL_RESET_MASK,
			reset_unit_regs + ARC_MISC_CNTL_REG);
	return 0;
}

static int grayskull_arc_init(struct grayskull_device *gs_dev) {
	void __iomem *reset_unit_regs = gs_dev->reset_unit_regs;
	u32 gpio_val;
	int ret;

	if (!arc_fw_init) {
		pr_info("ARC initialization skipped.\n");
		return 0;
	}

	gpio_val = ioread32(reset_unit_regs + GPIO_PAD_VAL_REG);
	if ((gpio_val & GPIO_ARC_SPI_BOOTROM_EN_MASK) == GPIO_ARC_SPI_BOOTROM_EN_MASK) {
		ret = wait_reg32_with_timeout(reset_unit_regs, reset_unit_regs + SCRATCH_REG(5),
						SCRATCH_5_ARC_BOOTROM_DONE, 1000);
		if (ret) {
			pr_warn("Timeout waiting for SPI bootrom init done.\n");
			goto grayskull_arc_init_err;
		}
	} else {
		pr_warn("SPI bootrom not enabled.\n");
		goto grayskull_arc_init_err;
	}

	if (arc_fw_override) {
		if (grayskull_load_arc_fw(gs_dev)) {
			pr_warn("ARC FW Override unsuccessful.\n");
			goto grayskull_arc_init_err;
		}
	}

	if (watchdog_fw_override) {
		if (grayskull_load_watchdog_fw(gs_dev)) {
			pr_warn("Watchdog FW Override unsuccessful.\n");
			goto grayskull_arc_init_err;
		}
	}

	if (grayskull_populate_arc_if(gs_dev)) {
		pr_warn("Driver to ARC table init failed.\n");
		goto grayskull_arc_init_err;
	}

	if (toggle_arc_reset(reset_unit_regs))
		goto grayskull_arc_init_err;

	if (wait_reg32_with_timeout(reset_unit_regs, reset_unit_regs + SCRATCH_REG(5),
					SCRATCH_5_ARC_L2_DONE, 5000000))
		goto grayskull_arc_init_err;

	pr_info("ARC initialization done.\n");
	return 0;

grayskull_arc_init_err:
	pr_warn("ARC initialization failed.\n");
	return -1;
}

// This is shared with wormhole.
bool grayskull_shutdown_firmware(u8 __iomem* reset_unit_regs) {
	if (is_hardware_hung(reset_unit_regs))
		return false;

	if (!grayskull_send_arc_fw_message(reset_unit_regs, GS_FW_MSG_ASTATE3, 5000))
		return false;
	return true;
}

bool grayskull_init(struct tenstorrent_device *tt_dev) {
	struct grayskull_device *gs_dev = tt_dev_to_gs_dev(tt_dev);

	gs_dev->reg_iomap = pci_iomap_range(gs_dev->tt.pdev, 0, REG_IOMAP_START, REG_IOMAP_LEN);
	gs_dev->pci_tlb = pci_iomap_range(gs_dev->tt.pdev, PCI_TLB_BAR, PCI_TLB_START, PCI_TLB_LEN);

	if (gs_dev->reg_iomap == NULL || gs_dev->pci_tlb == NULL) {
		if (gs_dev->reg_iomap != NULL)
			pci_iounmap(gs_dev->tt.pdev, gs_dev->reg_iomap);

		if (gs_dev->pci_tlb != NULL)
			pci_iounmap(gs_dev->tt.pdev, gs_dev->pci_tlb);
		return false;
	}

	gs_dev->reset_unit_regs = gs_dev->reg_iomap + RESET_UNIT_REG_OFFSET;

	return true;
}

bool grayskull_init_hardware(struct tenstorrent_device *tt_dev) {
	struct grayskull_device *gs_dev = tt_dev_to_gs_dev(tt_dev);

	if (arc_l2_is_running(gs_dev->reset_unit_regs)) {
		grayskull_send_arc_fw_message(gs_dev->reset_unit_regs, GS_FW_MSG_ASTATE0, 5000);
		return true;
	}

	return 0 == grayskull_arc_init(gs_dev);
}

void grayskull_cleanup(struct tenstorrent_device *tt_dev) {
	struct grayskull_device *gs_dev = tt_dev_to_gs_dev(tt_dev);

	if (gs_dev->reset_unit_regs != NULL)
		grayskull_shutdown_firmware(gs_dev->reset_unit_regs);

	if (gs_dev->reg_iomap != NULL)
		pci_iounmap(gs_dev->tt.pdev, gs_dev->reg_iomap);

	if (gs_dev->pci_tlb != NULL)
		pci_iounmap(gs_dev->tt.pdev, gs_dev->pci_tlb);
}

static void grayskull_last_release_handler(struct tenstorrent_device *tt_dev) {
	struct grayskull_device *gs_dev = tt_dev_to_gs_dev(tt_dev);
	grayskull_send_arc_fw_message(gs_dev->reset_unit_regs,
					GS_FW_MSG_GO_LONG_IDLE,
					2000);
}

struct tenstorrent_device_class grayskull_class = {
	.name = "Grayskull",
	.instance_size = sizeof(struct grayskull_device),
	.init_device = grayskull_init,
	.init_hardware = grayskull_init_hardware,
	.cleanup_device = grayskull_cleanup,
	.last_release_cb = grayskull_last_release_handler,
};
