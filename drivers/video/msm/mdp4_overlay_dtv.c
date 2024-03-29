/* Copyright (c) 2010, Code Aurora Forum. All rights reserved.
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
#include <linux/io.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/fb.h>
#include <asm/system.h>
#include <asm/mach-types.h>
#include <mach/hardware.h>

#include "mdp.h"
#include "msm_fb.h"
#include "mdp4.h"

#define DTV_BASE	0xD0000

/*#define DEBUG*/
#ifdef DEBUG
static void __mdp_outp(uint32 port, uint32 value)
{
	uint32 in_val;

	outpdw(port, value);
	in_val = inpdw(port);
	printk(KERN_INFO "MDP-DTV[%04x] => %08x [%08x]\n",
		port-(uint32)(MDP_BASE + DTV_BASE), value, in_val);
}

#undef MDP_OUTP
#define MDP_OUTP(port, value)	__mdp_outp((uint32)(port), (value))
#endif

static int first_pixel_start_x;
static int first_pixel_start_y;

static struct mdp4_overlay_pipe *dtv_pipe;

int mdp4_dtv_on(struct platform_device *pdev)
{
	int dtv_width;
	int dtv_height;
	int dtv_bpp;
	int dtv_border_clr;
	int dtv_underflow_clr;
	int dtv_hsync_skew;

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
	uint32 format;
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

	bpp = fbi->var.bits_per_pixel / 8;
	buf = (uint8 *) fbi->fix.smem_start;
	buf += fbi->var.xoffset * bpp +
		fbi->var.yoffset * fbi->fix.line_length;

	if (bpp == 2)
		format = MDP_RGB_565;
	else if (bpp == 3)
		format = MDP_RGB_888;
	else
		format = MDP_ARGB_8888;

	if (dtv_pipe == NULL) {
		ptype = mdp4_overlay_format2type(format);
		if (ptype < 0)
			printk(KERN_INFO "%s: format2type failed\n", __func__);
		pipe = mdp4_overlay_pipe_alloc(ptype, MDP4_MIXER1, 0);
		if (pipe == NULL) {
			printk(KERN_INFO "%s: pipe_alloc failed\n", __func__);
			return -EBUSY;
		}
		pipe->pipe_used++;
		pipe->mixer_stage  = MDP4_MIXER_STAGE_BASE;
		pipe->mixer_num  = MDP4_MIXER1;
		pipe->src_format = format;
		mdp4_overlay_panel_mode(pipe->mixer_num, MDP4_PANEL_DTV);
		ret = mdp4_overlay_format2pipe(pipe);
		if (ret < 0)
			printk(KERN_INFO "%s: format2type failed\n", __func__);

		dtv_pipe = pipe; /* keep it */
	} else {
		pipe = dtv_pipe;
	}

	/* MDP cmd block enable */
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	pipe->src_height = fbi->var.yres;
	pipe->src_width = fbi->var.xres;
	pipe->src_h = fbi->var.yres;
	pipe->src_w = fbi->var.xres;
	pipe->src_y = 0;
	pipe->src_x = 0;
	pipe->srcp0_addr = (uint32) buf;
	pipe->srcp0_ystride = fbi->fix.line_length;

	mdp4_overlay_dmae_xy(pipe);	/* dma_e */
	mdp4_overlay_dmae_cfg(mfd, 0);

	mdp4_overlay_rgb_setup(pipe);

	mdp4_mixer_stage_up(pipe);

	mdp4_overlayproc_cfg(pipe);

	/*
	 * DTV timing setting
	 */
	h_back_porch = var->left_margin;
	h_front_porch = var->right_margin;
	v_back_porch = var->upper_margin;
	v_front_porch = var->lower_margin;
	hsync_pulse_width = var->hsync_len;
	vsync_pulse_width = var->vsync_len;
	dtv_border_clr = mfd->panel_info.lcdc.border_clr;
	dtv_underflow_clr = mfd->panel_info.lcdc.underflow_clr;
	dtv_hsync_skew = mfd->panel_info.lcdc.hsync_skew;

	pr_info("%s: <ID=%d %dx%d (%d,%d,%d), (%d,%d,%d) %dMHz>\n", __func__,
		var->reserved[3], var->xres, var->yres,
		var->right_margin, var->hsync_len, var->left_margin,
		var->lower_margin, var->vsync_len, var->upper_margin,
		var->pixclock/1000/1000);

	dtv_width = var->xres;
	dtv_height = var->yres;
	dtv_bpp = mfd->panel_info.bpp;

	hsync_period =
	    hsync_pulse_width + h_back_porch + dtv_width + h_front_porch;
	hsync_ctrl = (hsync_period << 16) | hsync_pulse_width;
	hsync_start_x = hsync_pulse_width + h_back_porch;
	hsync_end_x = hsync_period - h_front_porch - 1;
	display_hctl = (hsync_end_x << 16) | hsync_start_x;

	vsync_period =
	    (vsync_pulse_width + v_back_porch + dtv_height +
	     v_front_porch) * hsync_period;
	display_v_start =
	    (vsync_pulse_width + v_back_porch) * hsync_period + dtv_hsync_skew;
	display_v_end =
	    vsync_period - (v_front_porch * hsync_period) + dtv_hsync_skew - 1;

	if (dtv_width != var->xres) {
		active_h_start = hsync_start_x + first_pixel_start_x;
		active_h_end = active_h_start + var->xres - 1;
		active_hctl =
		    ACTIVE_START_X_EN | (active_h_end << 16) | active_h_start;
	} else {
		active_hctl = 0;
	}

	if (dtv_height != var->yres) {
		active_v_start =
		    display_v_start + first_pixel_start_y * hsync_period;
		active_v_end = active_v_start + (var->yres) * hsync_period - 1;
		active_v_start |= ACTIVE_START_Y_EN;
	} else {
		active_v_start = 0;
		active_v_end = 0;
	}

	dtv_underflow_clr |= 0x80000000;	/* enable recovery */
	hsync_polarity = fbi->var.yres >= 720 ? 0 : 1;
	vsync_polarity = fbi->var.yres >= 720 ? 0 : 1;
	data_en_polarity = 0;

	ctrl_polarity =
	    (data_en_polarity << 2) | (vsync_polarity << 1) | (hsync_polarity);


	MDP_OUTP(MDP_BASE + DTV_BASE + 0x4, hsync_ctrl);
	MDP_OUTP(MDP_BASE + DTV_BASE + 0x8, vsync_period);
	MDP_OUTP(MDP_BASE + DTV_BASE + 0xc, vsync_pulse_width * hsync_period);
	MDP_OUTP(MDP_BASE + DTV_BASE + 0x18, display_hctl);
	MDP_OUTP(MDP_BASE + DTV_BASE + 0x1c, display_v_start);
	MDP_OUTP(MDP_BASE + DTV_BASE + 0x20, display_v_end);
	MDP_OUTP(MDP_BASE + DTV_BASE + 0x40, dtv_border_clr);
	MDP_OUTP(MDP_BASE + DTV_BASE + 0x44, dtv_underflow_clr);
	MDP_OUTP(MDP_BASE + DTV_BASE + 0x48, dtv_hsync_skew);
	MDP_OUTP(MDP_BASE + DTV_BASE + 0x50, ctrl_polarity);
	MDP_OUTP(MDP_BASE + DTV_BASE + 0x2c, active_hctl);
	MDP_OUTP(MDP_BASE + DTV_BASE + 0x30, active_v_start);
	MDP_OUTP(MDP_BASE + DTV_BASE + 0x38, active_v_end);

	/* Test pattern 8 x 8 pixel */
	/* MDP_OUTP(MDP_BASE + DTV_BASE + 0x4C, 0x80000808); */

	mdp_pipe_ctrl(MDP_OVERLAY1_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	/* MDP cmd block disable */
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	return ret;
}

int mdp4_dtv_off(struct platform_device *pdev)
{
	int ret = 0;

	/* MDP cmd block enable */
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	MDP_OUTP(MDP_BASE + DTV_BASE, 0);
	/* MDP cmd block disable */
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
	mdp_pipe_ctrl(MDP_OVERLAY1_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	ret = panel_next_off(pdev);

	/* delay to make sure the last frame finishes */
	msleep(100);

	pr_info("%s\n", __func__);

	/* dis-engage rgb2 from mixer1 */
	if (dtv_pipe)
		mdp4_mixer_stage_down(dtv_pipe);

	return ret;
}

static void mdp4_overlay_dtv_alloc_pipe(struct msm_fb_data_type *mfd,
		int32 ptype)
{
	int ret = 0;
	struct fb_info *fbi = mfd->fbi;
	struct mdp4_overlay_pipe *pipe;

	if (dtv_pipe != NULL)
		return;

	pr_debug("%s: ptype=%d\n", __func__, ptype);

	pipe = mdp4_overlay_pipe_alloc(ptype, MDP4_MIXER1);
	if (pipe == NULL) {
		pr_err("%s: pipe_alloc failed\n", __func__);
		return;
	}
	pipe->pipe_used++;
	pipe->mixer_stage = MDP4_MIXER_STAGE_BASE;
	pipe->mixer_num = MDP4_MIXER1;

	if (ptype == OVERLAY_TYPE_BF) {
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		/* LSP_BORDER_COLOR */
		MDP_OUTP(MDP_BASE + MDP4_OVERLAYPROC1_BASE + 0x5004,
			((0x0 & 0xFFF) << 16) |	/* 12-bit B */
			(0x0 & 0xFFF));		/* 12-bit G */
		/* MSP_BORDER_COLOR */
		MDP_OUTP(MDP_BASE + MDP4_OVERLAYPROC1_BASE + 0x5008,
			(0x0 & 0xFFF));		/* 12-bit R */
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
	} else {
		switch (mfd->ibuf.bpp) {
		case 2:
			pipe->src_format = MDP_RGB_565;
			break;
		case 3:
			pipe->src_format = MDP_RGB_888;
			break;
		case 4:
		default:
			if (hdmi_prim_display)
				pipe->src_format = MSMFB_DEFAULT_TYPE;
			else
				pipe->src_format = MDP_ARGB_8888;
			break;
		}
	}

	pipe->src_height = fbi->var.yres;
	pipe->src_width = fbi->var.xres;
	pipe->src_h = fbi->var.yres;
	pipe->src_w = fbi->var.xres;
	pipe->src_y = 0;
	pipe->src_x = 0;
	pipe->srcp0_ystride = fbi->fix.line_length;

	ret = mdp4_overlay_format2pipe(pipe);
	if (ret < 0)
		pr_warn("%s: format2type failed\n", __func__);

	mdp4_overlay_dmae_xy(pipe);	/* dma_e */
	mdp4_overlayproc_cfg(pipe);

	if (pipe->pipe_type == OVERLAY_TYPE_RGB) {
		pipe->srcp0_addr = (uint32) mfd->ibuf.buf;
		mdp4_overlay_rgb_setup(pipe);
	}

	mdp4_mixer_stage_up(pipe);
	mdp4_overlay_reg_flush(pipe, 1);

	dtv_pipe = pipe; /* keep it */
}

int mdp4_overlay_dtv_set(struct msm_fb_data_type *mfd,
			struct mdp4_overlay_pipe *pipe)
{
	if (dtv_pipe != NULL)
		return 0;

	if (pipe != NULL && pipe->mixer_stage == MDP4_MIXER_STAGE_BASE &&
			pipe->pipe_type == OVERLAY_TYPE_RGB)
		dtv_pipe = pipe; /* keep it */
	else if (!hdmi_prim_display && mdp4_overlay_borderfill_supported())
		mdp4_overlay_dtv_alloc_pipe(mfd, OVERLAY_TYPE_BF);
	else
		mdp4_overlay_dtv_alloc_pipe(mfd, OVERLAY_TYPE_RGB);
	if (dtv_pipe == NULL)
		return -ENODEV;
	return mdp4_dtv_start(mfd);
}

int mdp4_overlay_dtv_unset(struct msm_fb_data_type *mfd,
			struct mdp4_overlay_pipe *pipe)
{
	int result = 0;

	if (dtv_pipe == NULL)
		return result;

	pipe->flags &= ~MDP_OV_PLAY_NOWAIT;
	mdp4_overlay_reg_flush(pipe, 0);
	mdp4_overlay_dtv_ov_done_push(mfd, pipe);

	if (pipe->mixer_stage == MDP4_MIXER_STAGE_BASE &&
			pipe->pipe_type == OVERLAY_TYPE_RGB) {
		result = mdp4_dtv_stop(mfd);
		dtv_pipe = NULL;
	}
	return result;
}

static void mdp4_dtv_blt_ov_update(struct mdp4_overlay_pipe *pipe)
{
	uint32 off, addr;
	int bpp;
	char *overlay_base;

	if (pipe->blt_addr == 0)
		return;
#ifdef BLT_RGB565
	bpp = 2; /* overlay ouput is RGB565 */
#else
	bpp = 3; /* overlay ouput is RGB888 */
#endif
	off = (pipe->ov_cnt & 0x01) ?
		pipe->src_height * pipe->src_width * bpp : 0;

	addr = pipe->blt_addr + off;
	pr_debug("%s overlay addr 0x%x\n", __func__, addr);
	/* overlay 1 */
	overlay_base = MDP_BASE + MDP4_OVERLAYPROC1_BASE;/* 0x18000 */
	outpdw(overlay_base + 0x000c, addr);
	outpdw(overlay_base + 0x001c, addr);
}

static inline void mdp4_dtv_blt_dmae_update(struct mdp4_overlay_pipe *pipe)
{
	uint32 off, addr;
	int bpp;

	if (pipe->blt_addr == 0)
		return;

#ifdef BLT_RGB565
	bpp = 2; /* overlay ouput is RGB565 */
#else
	bpp = 3; /* overlay ouput is RGB888 */
#endif
	off =  (pipe->dmae_cnt & 0x01) ?
		pipe->src_height * pipe->src_width * bpp : 0;
	addr = pipe->blt_addr + off;
	MDP_OUTP(MDP_BASE + 0xb0008, addr);
}

static inline void mdp4_overlay_dtv_ov_kick_start(void)
{
	outpdw(MDP_BASE + 0x0008, 0);
}

static void mdp4_overlay_dtv_ov_start(struct msm_fb_data_type *mfd)
{
	unsigned long flag;

	/* enable irq */
	if (mfd->ov_start)
		return;

	if (!dtv_pipe) {
		pr_debug("%s: no mixer1 base layer pipe allocated!\n",
			 __func__);
		return;
	}

	if (dtv_pipe->blt_addr) {
		mdp4_dtv_blt_ov_update(dtv_pipe);
		dtv_pipe->ov_cnt++;
		mdp4_overlay_dtv_ov_kick_start();
	}

	spin_lock_irqsave(&mdp_spin_lock, flag);
	mdp_enable_irq(MDP_OVERLAY1_TERM);
	INIT_COMPLETION(dtv_pipe->comp);
	mfd->dma->waiting = TRUE;
	outp32(MDP_INTR_CLEAR, INTR_OVERLAY1_DONE);
	mdp_intr_mask |= INTR_OVERLAY1_DONE;
	outp32(MDP_INTR_ENABLE, mdp_intr_mask);
	spin_unlock_irqrestore(&mdp_spin_lock, flag);
	mfd->ov_start = true;
}

static void mdp4_overlay_dtv_wait4_ov_done(struct msm_fb_data_type *mfd,
	struct mdp4_overlay_pipe *pipe)
{
	u32 data = inpdw(MDP_BASE + DTV_BASE);

	if (mfd->ov_start)
		mfd->ov_start = false;
	else
		return;
	if (!(data & 0x1) || (pipe == NULL))
		return;
	if (!dtv_pipe) {
		pr_debug("%s: no mixer1 base layer pipe allocated!\n",
			 __func__);
		return;
	}

	wait_for_completion_timeout(&dtv_pipe->comp,
			msecs_to_jiffies(VSYNC_PERIOD*2));
	mdp_disable_irq(MDP_OVERLAY1_TERM);
}

void mdp4_overlay_dtv_start(void)
{
	if (!dtv_enabled) {
		mdp4_iommu_attach();
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		/* enable DTV block */
		MDP_OUTP(MDP_BASE + DTV_BASE, 1);
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
		dtv_enabled = 1;
	}
}

void mdp4_overlay_dtv_ov_done_push(struct msm_fb_data_type *mfd,
			struct mdp4_overlay_pipe *pipe)
{
	mdp4_overlay_reg_flush(pipe, 0);
	mdp4_overlay_dtv_ov_start(mfd);
	if (pipe->flags & MDP_OV_PLAY_NOWAIT)
		return;

	mdp4_overlay_dtv_wait4_ov_done(mfd, pipe);

	/* change mdp clk while mdp is idle` */
	mdp4_set_perf_level();
}

void mdp4_overlay_dtv_wait_for_ov(struct msm_fb_data_type *mfd,
	struct mdp4_overlay_pipe *pipe)
{
	mdp4_overlay_dtv_wait4_ov_done(mfd, pipe);
	mdp4_set_perf_level();
}

void mdp4_dma_e_done_dtv()
{
	if (!dtv_pipe)
		return;

	complete(&dtv_pipe->comp);
}

void mdp4_external_vsync_dtv()
{

	complete_all(&dtv_comp);
}

/*
 * mdp4_overlay1_done_dtv: called from isr
 */
void mdp4_overlay1_done_dtv()
{
	complete(&dtv_pipe->comp);
}

void mdp4_dtv_overlay(struct msm_fb_data_type *mfd)
{
	struct fb_info *fbi = mfd->fbi;
	uint8 *buf;
	int bpp;
	unsigned long flag;
	struct mdp4_overlay_pipe *pipe;

	if (!mfd->panel_power_on)
		return;

	/* no need to power on cmd block since it's lcdc mode */
	bpp = fbi->var.bits_per_pixel / 8;
	buf = (uint8 *) fbi->fix.smem_start;
	buf += fbi->var.xoffset * bpp +
		fbi->var.yoffset * fbi->fix.line_length;

	mutex_lock(&mfd->dma->ov_mutex);

	pipe = dtv_pipe;
	pipe->srcp0_addr = (uint32) buf;
	mdp4_overlay_rgb_setup(pipe);
	mdp4_overlay_reg_flush(pipe, 1); /* rgb2 and mixer1 */

	/* enable irq */
	spin_lock_irqsave(&mdp_spin_lock, flag);
	mdp_enable_irq(MDP_OVERLAY1_TERM);
	INIT_COMPLETION(dtv_pipe->comp);
	mfd->dma->waiting = TRUE;
	outp32(MDP_INTR_CLEAR, INTR_OVERLAY1_DONE);
	mdp_intr_mask |= INTR_OVERLAY1_DONE;
	outp32(MDP_INTR_ENABLE, mdp_intr_mask);
	spin_unlock_irqrestore(&mdp_spin_lock, flag);
	wait_for_completion_killable(&dtv_pipe->comp);
	mdp_disable_irq(MDP_OVERLAY1_TERM);

	mdp4_stat.kickoff_dtv++;
	mdp4_overlay_resource_release();
	mutex_unlock(&mfd->dma->ov_mutex);
}
