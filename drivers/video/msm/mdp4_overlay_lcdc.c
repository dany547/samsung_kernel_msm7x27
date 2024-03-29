/* Copyright (c) 2009-2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/hrtimer.h>
#include <linux/delay.h>
#include <mach/hardware.h>
#include <linux/io.h>

#include <asm/system.h>
#include <asm/mach-types.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>

#include <linux/fb.h>

#include "mdp.h"
#include "msm_fb.h"
#include "mdp4.h"

#ifdef CONFIG_FB_MSM_MDP40
#define LCDC_BASE	0xC0000
#else
#define LCDC_BASE	0xE0000
#endif

int first_pixel_start_x;
int first_pixel_start_y;

static struct mdp4_overlay_pipe *lcdc_pipe;
static struct completion lcdc_comp;

int mdp_lcdc_on(struct platform_device *pdev)
{
	int lcdc_width;
	int lcdc_height;
	int lcdc_bpp;
	int lcdc_border_clr;
	int lcdc_underflow_clr;
	int lcdc_hsync_skew;

	int hsync_period;
	int hsync_ctrl;
	int vsync_period;
	int display_hctl;
	int display_v_start;
	int display_v_end;
	int active_hctl;
	int active_h_start;
	int active_h_end;
	int active_v_start;
	int active_v_end;
	int ctrl_polarity;
	int h_back_porch;
	int h_front_porch;
	int v_back_porch;
	int v_front_porch;
	int hsync_pulse_width;
	int vsync_pulse_width;
	int hsync_polarity;
	int vsync_polarity;
	int data_en_polarity;
	int hsync_start_x;
	int hsync_end_x;
	uint8 *buf;
	int bpp, ptype;
	struct fb_info *fbi;
	struct fb_var_screeninfo *var;
	struct msm_fb_data_type *mfd;
	struct mdp4_overlay_pipe *pipe;
	int ret;

	mfd = (struct msm_fb_data_type *)platform_get_drvdata(pdev);

	if (!mfd)
		return -ENODEV;

	if (mfd->key != MFD_KEY)
		return -EINVAL;

	fbi = mfd->fbi;
	var = &fbi->var;

	/* MDP cmd block enable */
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	if (is_mdp4_hw_reset()) {
		mdp4_hw_init();
		outpdw(MDP_BASE + 0x0038, mdp4_display_intf);
	}

	bpp = fbi->var.bits_per_pixel / 8;
	buf = (uint8 *) fbi->fix.smem_start;
	buf += calc_fb_offset(mfd, fbi, bpp);

	if (lcdc_pipe == NULL) {
		ptype = mdp4_overlay_format2type(mfd->fb_imgType);
		if (ptype < 0)
			printk(KERN_INFO "%s: format2type failed\n", __func__);
		pipe = mdp4_overlay_pipe_alloc(ptype, MDP4_MIXER0, 0);
		if (pipe == NULL)
			printk(KERN_INFO "%s: pipe_alloc failed\n", __func__);
		pipe->pipe_used++;
		pipe->mixer_stage  = MDP4_MIXER_STAGE_BASE;
		pipe->mixer_num  = MDP4_MIXER0;
		pipe->src_format = mfd->fb_imgType;
		mdp4_overlay_panel_mode(pipe->mixer_num, MDP4_PANEL_LCDC);
		ret = mdp4_overlay_format2pipe(pipe);
		if (ret < 0)
			printk(KERN_INFO "%s: format2pipe failed\n", __func__);
		lcdc_pipe = pipe; /* keep it */
		init_completion(&lcdc_comp);
	} else {
		pipe = lcdc_pipe;
	}

	pipe->src_height = fbi->var.yres;
	pipe->src_width = fbi->var.xres;
	pipe->src_h = fbi->var.yres;
	pipe->src_w = fbi->var.xres;
	pipe->src_y = 0;
	pipe->src_x = 0;
	pipe->srcp0_addr = (uint32) buf;
	pipe->srcp0_ystride = fbi->fix.line_length;

	mdp4_overlay_dmap_xy(pipe);
	mdp4_overlay_dmap_cfg(mfd, 1);

	mdp4_overlay_rgb_setup(pipe);

	mdp4_mixer_stage_up(pipe);

	mdp4_overlayproc_cfg(pipe);

	/*
	 * LCDC timing setting
	 */
	h_back_porch = var->left_margin;
	h_front_porch = var->right_margin;
	v_back_porch = var->upper_margin;
	v_front_porch = var->lower_margin;
	hsync_pulse_width = var->hsync_len;
	vsync_pulse_width = var->vsync_len;
	lcdc_border_clr = mfd->panel_info.lcdc.border_clr;
	lcdc_underflow_clr = mfd->panel_info.lcdc.underflow_clr;
	lcdc_hsync_skew = mfd->panel_info.lcdc.hsync_skew;

	lcdc_width = var->xres;
	lcdc_height = var->yres;
	lcdc_bpp = mfd->panel_info.bpp;

	hsync_period =
	    hsync_pulse_width + h_back_porch + lcdc_width + h_front_porch;
	hsync_ctrl = (hsync_period << 16) | hsync_pulse_width;
	hsync_start_x = hsync_pulse_width + h_back_porch;
	hsync_end_x = hsync_period - h_front_porch - 1;
	display_hctl = (hsync_end_x << 16) | hsync_start_x;

	vsync_period =
	    (vsync_pulse_width + v_back_porch + lcdc_height +
	     v_front_porch) * hsync_period;
	display_v_start =
	    (vsync_pulse_width + v_back_porch) * hsync_period + lcdc_hsync_skew;
	display_v_end =
	    vsync_period - (v_front_porch * hsync_period) + lcdc_hsync_skew - 1;

	if (lcdc_width != var->xres) {
		active_h_start = hsync_start_x + first_pixel_start_x;
		active_h_end = active_h_start + var->xres - 1;
		active_hctl =
		    ACTIVE_START_X_EN | (active_h_end << 16) | active_h_start;
	} else {
		active_hctl = 0;
	}

	if (lcdc_height != var->yres) {
		active_v_start =
		    display_v_start + first_pixel_start_y * hsync_period;
		active_v_end = active_v_start + (var->yres) * hsync_period - 1;
		active_v_start |= ACTIVE_START_Y_EN;
	} else {
		active_v_start = 0;
		active_v_end = 0;
	}


#ifdef CONFIG_FB_MSM_MDP40
	hsync_polarity = 1;
	vsync_polarity = 1;
	lcdc_underflow_clr |= 0x80000000;	/* enable recovery */
#else
	hsync_polarity = 0;
	vsync_polarity = 0;
#endif
	data_en_polarity = 0;

	ctrl_polarity =
	    (data_en_polarity << 2) | (vsync_polarity << 1) | (hsync_polarity);

	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x4, hsync_ctrl);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x8, vsync_period);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0xc, vsync_pulse_width * hsync_period);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x10, display_hctl);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x14, display_v_start);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x18, display_v_end);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x28, lcdc_border_clr);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x2c, lcdc_underflow_clr);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x30, lcdc_hsync_skew);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x38, ctrl_polarity);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x1c, active_hctl);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x20, active_v_start);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x24, active_v_end);

	mdp4_overlay_reg_flush(pipe, 1);
#ifdef CONFIG_MSM_BUS_SCALING
	mdp_bus_scale_update_request(2);
#endif

	ret = panel_next_on(pdev);
	if (ret == 0)
		mdp_pipe_ctrl(MDP_OVERLAY0_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	/* MDP cmd block disable */
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	return ret;
}

int mdp_lcdc_off(struct platform_device *pdev)
{
	int ret = 0;

	/* MDP cmd block enable */
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	MDP_OUTP(MDP_BASE + LCDC_BASE, 0);
	/* MDP cmd block disable */
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
	mdp_pipe_ctrl(MDP_OVERLAY0_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	mdp_disable_irq(MDP_DMA2_TERM);	/* disable intr */

	ret = panel_next_off(pdev);

	/* delay to make sure the last frame finishes */
	msleep(16);

#ifdef LCDC_RGB_UNSTAGE
	/* dis-engage rgb0 from mixer0 */
	if (lcdc_pipe)
		mdp4_mixer_stage_down(lcdc_pipe);
#endif
#ifdef CONFIG_MSM_BUS_SCALING
	mdp_bus_scale_update_request(0);
#endif

	return ret;
}

int mdp4_lcdc_overlay_blt_offset(int *off)
{
	if (lcdc_pipe->blt_addr == 0) {
		*off = -1;
		return -EINVAL;
	}

	*off = 0;
	return 0;
}

void mdp4_lcdc_overlay_blt(ulong addr)
{
	unsigned long flag;

	spin_lock_irqsave(&mdp_spin_lock, flag);
	lcdc_pipe->blt_addr = addr;
	lcdc_pipe->blt_cnt = 0;
	spin_unlock_irqrestore(&mdp_spin_lock, flag);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	MDP_OUTP(MDP_BASE + LCDC_BASE, 0);	/* stop lcdc */
	msleep(50);
	mdp4_overlayproc_cfg(lcdc_pipe);
	mdp4_overlay_dmap_xy(lcdc_pipe);
	MDP_OUTP(MDP_BASE + LCDC_BASE, 1);	/* start lcdc */
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

void mdp4_overlay_lcdc_wait4vsync(struct msm_fb_data_type *mfd)
{
	unsigned long flag;

	 /* enable irq */
	spin_lock_irqsave(&mdp_spin_lock, flag);
	mdp_enable_irq(MDP_DMA2_TERM);	/* enable intr */
	INIT_COMPLETION(lcdc_comp);
	mfd->dma->waiting = TRUE;
	outp32(MDP_INTR_CLEAR, INTR_PRIMARY_VSYNC);
	mdp_intr_mask |= INTR_PRIMARY_VSYNC;
	outp32(MDP_INTR_ENABLE, mdp_intr_mask);
	spin_unlock_irqrestore(&mdp_spin_lock, flag);
	wait_for_completion_killable(&lcdc_comp);
	mdp_disable_irq(MDP_DMA2_TERM);
}

static void mdp4_overlay_lcdc_dma_busy_wait(struct msm_fb_data_type *mfd)
{
	unsigned long flag;
	int need_wait = 0;

	pr_debug("%s: start pid=%d\n", __func__, current->pid);

	spin_lock_irqsave(&mdp_spin_lock, flag);
	if (mfd->dma->busy == TRUE) {
		INIT_COMPLETION(mfd->dma->comp);
		need_wait++;
	}
	spin_unlock_irqrestore(&mdp_spin_lock, flag);

	if (need_wait) {
		/* wait until DMA finishes the current job */
		pr_debug("%s: pending pid=%d\n", __func__, current->pid);
		wait_for_completion(&mfd->dma->comp);
	}
	pr_debug("%s: done pid=%d\n", __func__, current->pid);
}

void mdp4_overlay_lcdc_start(void)
{
	if (!lcdc_enabled) {
		/* enable LCDC block */
		mdp4_iommu_attach();
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		MDP_OUTP(MDP_BASE + LCDC_BASE, 1);
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
		lcdc_enabled = 1;
	}
}

void mdp4_overlay_lcdc_vsync_push(struct msm_fb_data_type *mfd,
			struct mdp4_overlay_pipe *pipe)
{

	mdp4_overlay_reg_flush(pipe, 1);
	if (pipe->flags & MDP_OV_PLAY_NOWAIT)
		return;

	mdp4_overlay_lcdc_wait4vsync(mfd);
}

/*
 * mdp4_primary_vsync_lcdc: called from isr
 */
void mdp4_primary_vsync_lcdc(void)
{
	complete_all(&lcdc_comp);
}

/*
 * mdp4_overlay0_done_lcdc: called from isr
 */
void mdp4_overlay0_done_lcdc(void)
{
	complete_all(&lcdc_comp);
}

void mdp4_lcdc_overlay(struct msm_fb_data_type *mfd)
{
	struct fb_info *fbi = mfd->fbi;
	uint8 *buf;
	int bpp;
	struct mdp4_overlay_pipe *pipe;

	if (!mfd->panel_power_on)
		return;

	/* no need to power on cmd block since it's lcdc mode */
	bpp = fbi->var.bits_per_pixel / 8;
	buf = (uint8 *) fbi->fix.smem_start;
	buf += calc_fb_offset(mfd, fbi, bpp);

	mutex_lock(&mfd->dma->ov_mutex);

	pipe = lcdc_pipe;
	pipe->srcp0_addr = (uint32) buf;
	mdp4_overlay_rgb_setup(pipe);
	mdp4_overlay_vsync_push(mfd, pipe);
	mdp4_stat.kickoff_lcdc++;
	mdp4_overlay_resource_release();
	mutex_unlock(&mfd->dma->ov_mutex);
}
