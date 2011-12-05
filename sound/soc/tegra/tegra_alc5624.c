/*
 * tegra_soc_alc5624.c  --  SoC audio for tegra (glue logic)
 *
 * (c) 2010-2011 Nvidia Graphics Pvt. Ltd.
 *  http://www.nvidia.com
 * (C) 2011 Eduardo Jos� Tagle <ejtagle@tutopia.com>
 *
 * Copyright 2007 Wolfson Microelectronics PLC.
 * Author: Graeme Gregory
 *         graeme.gregory@wolfsonmicro.com or linux@wolfsonmicro.com
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

/* Shuttle uses MIC2 as the mic input */
 
#define DEBUG
#define FORCE_AUTO_SPK_HP_SWITCH 1 /* Force autoswitch between headphone output and Speaker output */
 
#include <asm/mach-types.h>

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#ifdef CONFIG_SWITCH
#include <linux/switch.h>
#endif
#include <linux/clk.h>
#include <linux/tegra_audio.h>
#include <linux/delay.h>

#include <mach/clk.h>
#include <mach/shuttle_audio.h>

#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/jack.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include <sound/alc5624.h>

#include "tegra_pcm.h"
#include "tegra_asoc_utils.h"

#ifdef USE_ORG_DAS
#include <mach/tegra_das.h>
#endif

#define DRV_NAME "tegra-snd-alc5624"

/* possible audio sources */
enum shuttle_audio_device {
	SHUTTLE_AUDIO_DEVICE_NONE	   = 0,		/* no device */
	SHUTTLE_AUDIO_DEVICE_BLUETOOTH = 0x01,	/* bluetooth */
	SHUTTLE_AUDIO_DEVICE_VOICE	   = 0x02,	/* cell phone audio */
	SHUTTLE_AUDIO_DEVICE_HIFI	   = 0x04,	/* normal speaker/headphone audio */
	
	SHUTTLE_AUDIO_DEVICE_MAX	   = 0x07	/* all audio sources */
};

struct tegra_alc5624 {
	struct tegra_asoc_utils_data util_data;
#ifdef CONFIG_SWITCH
	int jack_status;
#endif
	
	struct snd_soc_jack   tegra_jack;		/* jack detection */
	int 				  play_device;		/* Playback devices bitmask */
	int 				  capture_device;	/* Capture devices bitmask */
	bool				  is_call_mode;		/* if we are in a call mode */
	int					  gpio_hp_det;		/* GPIO used to detect Headphone plugged in */
#ifndef USE_ORG_DAS	
	int					  hifi_codec_datafmt;/* HiFi codec data format */
	bool				  hifi_codec_master;/* If Hifi codec is master */
	int					  bt_codec_datafmt;	/* Bluetooth codec data format */
	bool				  bt_codec_master;	/* If bt codec is master */
#endif
};

/* mclk required for each sampling frequency */
static const struct {
	unsigned int mclk;
	unsigned short srate;
} clocktab[] = {
	/* 8k */
	{ 8192000,  8000},
	{12288000,  8000},
	{24576000,  8000},

	/* 11.025k */
	{11289600, 11025},
	{16934400, 11025},
	{22579200, 11025},

	/* 16k */
	{12288000, 16000},
	{16384000, 16000},
	{24576000, 16000},

	/* 22.05k */	
	{11289600, 22050},
	{16934400, 22050},
	{22579200, 22050},

	/* 32k */
	{12288000, 32000},
	{16384000, 32000},
	{24576000, 32000},

	/* 44.1k */
	{11289600, 44100},
	{22579200, 44100},

	/* 48k */
	{12288000, 48000},
	{24576000, 48000},
};


/* --------- Digital audio interfase ------------ */

static int tegra_hifi_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai 	= rtd->codec_dai;
	struct snd_soc_dai *cpu_dai 	= rtd->cpu_dai;
	struct snd_soc_codec *codec		= rtd->codec;
	struct snd_soc_card* card		= codec->card;
	struct tegra_alc5624* machine 	= snd_soc_card_get_drvdata(card);
	
	int sys_clk;
	int err;
	int i;	

	/* Get the requested sampling rate */
	unsigned int srate = params_rate(params);
	
#ifdef USE_ORG_DAS		
	/* I2S <-> DAC <-> DAS <-> DAP <-> CODEC
	   -If DAP is master, codec will be slave */
	int codec_is_master = !tegra_das_is_port_master(tegra_audio_codec_type_hifi);
	
	/* Get DAS dataformat - DAP is connecting to codec */
	enum dac_dap_data_format data_fmt = tegra_das_get_codec_data_fmt(tegra_audio_codec_type_hifi);

	/* We are supporting DSP and I2s format for now */
	int dai_flag = 0;
	if (data_fmt & dac_dap_data_format_i2s)
		dai_flag |= SND_SOC_DAIFMT_I2S;
	else
		dai_flag |= SND_SOC_DAIFMT_DSP_A;
	
	if (codec_is_master)
		dai_flag |= SND_SOC_DAIFMT_CBM_CFM; /* codec is master */
	else
		dai_flag |= SND_SOC_DAIFMT_CBS_CFS; 
#else

	/* I2S <-> DAC <-> DAS <-> DAP <-> CODEC
	   -If DAP is master, codec will be slave */
	bool codec_is_master = machine->hifi_codec_master;
	
	/* Get DAS dataformat - DAP is connecting to codec */
	int dai_flag = machine->hifi_codec_datafmt;
	
	/* Depending on the number of channels, we must select the mode -
		I2S only supports stereo operation, DSP_A can support mono 
		with the ALC5624 */
	/*t dai_flag = (params_channels(params) == 1) 
						? SND_SOC_DAIFMT_DSP_A
						: SND_SOC_DAIFMT_I2S;*/
	
	dev_dbg(card->dev,"%s(): cpu_dai:'%s'\n", __FUNCTION__,cpu_dai->name);
	dev_dbg(card->dev,"%s(): codec_dai:'%s'\n", __FUNCTION__,codec_dai->name);
	
	if (codec_is_master)
		dai_flag |= SND_SOC_DAIFMT_CBM_CFM; /* codec is master */
	else
		dai_flag |= SND_SOC_DAIFMT_CBS_CFS;
#endif

	dev_dbg(card->dev,"%s(): format: 0x%08x, channels:%d, srate:%d\n", __FUNCTION__,
		params_format(params),params_channels(params),params_rate(params));

	/* Set the CPU dai format. This will also set the clock rate in master mode */
	err = snd_soc_dai_set_fmt(cpu_dai, dai_flag);
	if (err < 0) {
		dev_err(card->dev,"cpu_dai fmt not set \n");
		return err;
	}

	err = snd_soc_dai_set_fmt(codec_dai, dai_flag);
	if (err < 0) {
		dev_err(card->dev,"codec_dai fmt not set \n");
		return err;
	}

	/* Get system clock */
	sys_clk = clk_get_rate(machine->util_data.clk_cdev1);

	if (codec_is_master) {
		dev_dbg(card->dev,"%s(): codec in master mode\n",__FUNCTION__);
		
		/* If using port as slave (=codec as master), then we can use the
		   codec PLL to get the other sampling rates */
		
		/* Try each one until success */
		for (i = 0; i < ARRAY_SIZE(clocktab); i++) {
		
			if (clocktab[i].srate != srate) 
				continue;
				
			if (snd_soc_dai_set_pll(codec_dai, 0, 0, sys_clk, clocktab[i].mclk) >= 0) {
				/* Codec PLL is synthetizing this new clock */
				sys_clk = clocktab[i].mclk;
				break;
			}
		}
		
		if (i >= ARRAY_SIZE(clocktab)) {
			dev_err(card->dev,"%s(): unable to set required MCLK for SYSCLK of %d, sampling rate: %d\n",__FUNCTION__,sys_clk,srate);
			return -EINVAL;
		}
		
	} else {
		dev_dbg(card->dev,"%s(): codec in slave mode\n",__FUNCTION__);

		/* Disable codec PLL */
		err = snd_soc_dai_set_pll(codec_dai, 0, 0, sys_clk, sys_clk);
		if (err < 0) {
			dev_err(card->dev,"%s(): unable to disable codec PLL\n",__FUNCTION__);
			return err;
		}
		
		/* Check this sampling rate can be achieved with this sysclk */
		for (i = 0; i < ARRAY_SIZE(clocktab); i++) {
		
			if (clocktab[i].srate != srate) 
				continue;
				
			if (sys_clk == clocktab[i].mclk)
				break;
		}
		
		if (i >= ARRAY_SIZE(clocktab)) {
			dev_err(card->dev,"%s(): unable to get required %d hz sampling rate of %d hz SYSCLK\n",__FUNCTION__,srate,sys_clk);
			return -EINVAL;
		}
	}

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, sys_clk);
	if (err < 0) {
		dev_err(card->dev, "Can't configure clocks\n");
		return err;
	}

	/* Set CODEC sysclk */
	err = snd_soc_dai_set_sysclk(codec_dai, 0, sys_clk, SND_SOC_CLOCK_IN);
	if (err < 0) {
		dev_err(card->dev,"codec_dai clock not set\n");
		return err;
	}
	
	return 0;
}

static int tegra_voice_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai 	= rtd->codec_dai;
	struct snd_soc_dai *cpu_dai 	= rtd->cpu_dai;
	struct snd_soc_codec *codec		= rtd->codec;
	struct snd_soc_card* card		= codec->card;
	struct tegra_alc5624* machine 	= snd_soc_card_get_drvdata(card);
	
	int sys_clk;
	int err;

#ifdef USE_ORG_DAS
		/* Get DAS dataformat and master flag */
	int codec_is_master = !tegra_das_is_port_master(tegra_audio_codec_type_bluetooth);
	enum dac_dap_data_format data_fmt = tegra_das_get_codec_data_fmt(tegra_audio_codec_type_bluetooth);

	/* We are supporting DSP and I2s format for now */
	int dai_flag = 0;
	if (data_fmt & dac_dap_data_format_dsp)
		dai_flag |= SND_SOC_DAIFMT_DSP_A;
	else
		dai_flag |= SND_SOC_DAIFMT_I2S;

	if (codec_is_master)
		dai_flag |= SND_SOC_DAIFMT_CBM_CFM; /* codec is master */
	else
		dai_flag |= SND_SOC_DAIFMT_CBS_CFS;
#else
	
	/* Get DAS dataformat and master flag */
	int codec_is_master = machine->bt_codec_master;

	/* We are supporting DSP and I2s format for now */
	int dai_flag = machine->bt_codec_datafmt;

	if (codec_is_master)
		dai_flag |= SND_SOC_DAIFMT_CBM_CFM; /* codec is master */
	else
		dai_flag |= SND_SOC_DAIFMT_CBS_CFS;
#endif

	dev_dbg(card->dev,"%s(): format: 0x%08x, channels:%d, srate:%d\n", __FUNCTION__,
		params_format(params),params_channels(params),params_rate(params));

	/* Set the CPU dai format. This will also set the clock rate in master mode */
	err = snd_soc_dai_set_fmt(cpu_dai, dai_flag);
	if (err < 0) {
		dev_err(card->dev,"cpu_dai fmt not set \n");
		return err;
	}

	/* Bluetooth Codec is always slave here */
	err = snd_soc_dai_set_fmt(codec_dai, dai_flag);
	if (err < 0) {
		dev_err(card->dev,"codec_dai fmt not set \n");
		return err;
	}
	
	/* Get system clock */
	sys_clk = clk_get_rate(machine->util_data.clk_cdev1);

	/* Set CPU sysclock as the same - in Tegra, seems to be a NOP */
	err = snd_soc_dai_set_sysclk(cpu_dai, 0, sys_clk, SND_SOC_CLOCK_IN);
	if (err < 0) {
		dev_err(card->dev,"cpu_dai clock not set\n");
		return err;
	}
	
	/* Set CODEC sysclk */
	err = snd_soc_dai_set_sysclk(codec_dai, 0, sys_clk, SND_SOC_CLOCK_IN);
	if (err < 0) {
		dev_err(card->dev,"cpu_dai clock not set\n");
		return err;
	}
	
	return 0;
}

static int tegra_spdif_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec		= rtd->codec;
	struct snd_soc_card* card		= codec->card;

	dev_dbg(card->dev,"%s(): format: 0x%08x\n", __FUNCTION__,params_format(params));
	return 0;
}

#ifdef USE_ORG_DAS
static int tegra_codec_startup(struct snd_pcm_substream *substream)
{
	tegra_das_power_mode(true);
	return 0;
}

static void tegra_codec_shutdown(struct snd_pcm_substream *substream)
{
	tegra_das_power_mode(false);
}
#endif 

static int tegra_soc_suspend_pre(struct snd_soc_card* card)
{
	return 0;
}

static int tegra_soc_suspend_post(struct snd_soc_card* card)
{
	struct tegra_alc5624 *machine = snd_soc_card_get_drvdata(card);
	clk_disable(machine->util_data.clk_cdev1);

	return 0;
}

static int tegra_soc_resume_pre(struct snd_soc_card* card)
{
	struct tegra_alc5624 *machine = snd_soc_card_get_drvdata(card);
	clk_enable(machine->util_data.clk_cdev1);

	return 0;
}

static int tegra_soc_resume_post(struct snd_soc_card *card)
{
	return 0;
}

static struct snd_soc_ops tegra_hifi_ops = {
	.hw_params = tegra_hifi_hw_params,
#ifdef USE_ORG_DAS	
	.startup = tegra_codec_startup,
	.shutdown = tegra_codec_shutdown, 	
#endif
};

static struct snd_soc_ops tegra_voice_ops = {
	.hw_params = tegra_voice_hw_params,
#ifdef USE_ORG_DAS	
	.startup = tegra_codec_startup,
	.shutdown = tegra_codec_shutdown, 	
#endif
};

static struct snd_soc_ops tegra_spdif_ops = {
	.hw_params = tegra_spdif_hw_params,
};

/* ------- Tegra audio routing using DAS -------- */

static void tegra_audio_route(struct tegra_alc5624* machine,
			      int play_device, int capture_device)
{
	
	int is_bt_sco_mode = (play_device    & SHUTTLE_AUDIO_DEVICE_BLUETOOTH) ||
						 (capture_device & SHUTTLE_AUDIO_DEVICE_BLUETOOTH);
	int is_call_mode   = (play_device    & SHUTTLE_AUDIO_DEVICE_VOICE) ||
						 (capture_device & SHUTTLE_AUDIO_DEVICE_VOICE);

	pr_debug("%s(): is_bt_sco_mode: %d, is_call_mode: %d\n", __FUNCTION__, is_bt_sco_mode, is_call_mode);

#ifdef USE_ORG_DAS
	if (is_call_mode && is_bt_sco_mode) {
		tegra_das_set_connection(tegra_das_port_con_id_voicecall_with_bt);
	}
	else if (is_call_mode && !is_bt_sco_mode) {
		tegra_das_set_connection(tegra_das_port_con_id_voicecall_no_bt);
	}
	else if (!is_call_mode && is_bt_sco_mode) {
		tegra_das_set_connection(tegra_das_port_con_id_bt_codec);
	}
	else {
		tegra_das_set_connection(tegra_das_port_con_id_hifi);
	}
#endif
	machine->play_device = play_device;
	machine->capture_device = capture_device;
}

static int tegra_play_route_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = SHUTTLE_AUDIO_DEVICE_NONE;
	uinfo->value.integer.max = SHUTTLE_AUDIO_DEVICE_MAX;
	return 0;
}

static int tegra_play_route_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct tegra_alc5624* machine = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = SHUTTLE_AUDIO_DEVICE_NONE;
	if (machine) {
		ucontrol->value.integer.value[0] = machine->play_device;
		return 0;
	}
	return -EINVAL;
}

static int tegra_play_route_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct tegra_alc5624* machine = snd_kcontrol_chip(kcontrol);

	if (machine) {
		int play_device_new = ucontrol->value.integer.value[0];

		if (machine->play_device != play_device_new) {
			tegra_audio_route(machine, play_device_new, machine->capture_device);
			return 1;
		}
		return 0;
	}
	return -EINVAL;
}

struct snd_kcontrol_new tegra_play_route_control = {
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Pcm Playback Route",
	.private_value = 0xffff,
	.info = tegra_play_route_info,
	.get = tegra_play_route_get,
	.put = tegra_play_route_put
};

static int tegra_capture_route_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = SHUTTLE_AUDIO_DEVICE_NONE;
	uinfo->value.integer.max = SHUTTLE_AUDIO_DEVICE_MAX;
	return 0;
}

static int tegra_capture_route_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct tegra_alc5624* machine = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = TEGRA_AUDIO_DEVICE_NONE;
	if (machine) {
		ucontrol->value.integer.value[0] = machine->capture_device;
		return 0;
	}
	return -EINVAL;
}

static int tegra_capture_route_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct tegra_alc5624* machine = snd_kcontrol_chip(kcontrol);

	if (machine) {
		int capture_device_new = ucontrol->value.integer.value[0];

		if (machine->capture_device != capture_device_new) {
			tegra_audio_route(machine,
				machine->play_device , capture_device_new);
			return 1;
		}
		return 0;
	}
	return -EINVAL;
}

static struct snd_kcontrol_new tegra_capture_route_control = {
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Pcm Capture Route",
	.private_value = 0xffff,
	.info = tegra_capture_route_info,
	.get = tegra_capture_route_get,
	.put = tegra_capture_route_put
};

static int tegra_call_mode_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int tegra_call_mode_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct tegra_alc5624* machine = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = 0;
	if (machine) {
		int is_call_mode   = (machine->play_device    & SHUTTLE_AUDIO_DEVICE_VOICE) ||
							 (machine->capture_device & SHUTTLE_AUDIO_DEVICE_VOICE);
	
		ucontrol->value.integer.value[0] = is_call_mode ? 1 : 0;
		return 0;
	}
	return -EINVAL;
}

static int tegra_call_mode_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct tegra_alc5624* machine = snd_kcontrol_chip(kcontrol);

	if (machine) {
		int is_call_mode   = (machine->play_device    & SHUTTLE_AUDIO_DEVICE_VOICE) ||
							 (machine->capture_device & SHUTTLE_AUDIO_DEVICE_VOICE);
	
		int is_call_mode_new = ucontrol->value.integer.value[0];

		if (is_call_mode != is_call_mode_new) {
			if (is_call_mode_new) {
				machine->play_device 	|= SHUTTLE_AUDIO_DEVICE_VOICE;
				machine->capture_device |= SHUTTLE_AUDIO_DEVICE_VOICE;
				machine->play_device 	&= ~SHUTTLE_AUDIO_DEVICE_HIFI;
				machine->capture_device &= ~SHUTTLE_AUDIO_DEVICE_HIFI;
			} else {
				machine->play_device 	&= ~SHUTTLE_AUDIO_DEVICE_VOICE;
				machine->capture_device &= ~SHUTTLE_AUDIO_DEVICE_VOICE;
				machine->play_device 	|= SHUTTLE_AUDIO_DEVICE_HIFI;
				machine->capture_device |= SHUTTLE_AUDIO_DEVICE_HIFI;
			}
			tegra_audio_route(machine,
				machine->play_device,
				machine->capture_device);
			return 1;
		}
		return 0;
	}
	return -EINVAL;
}

static struct snd_kcontrol_new tegra_call_mode_control = {
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Call Mode Switch",
	.private_value = 0xffff,
	.info = tegra_call_mode_info,
	.get = tegra_call_mode_get,
	.put = tegra_call_mode_put
};

static int tegra_das_controls_init(struct snd_soc_codec *codec)
{
	struct snd_soc_card *card = codec->card;
	struct snd_card *scard = card->snd_card;
	struct tegra_alc5624* machine = snd_soc_card_get_drvdata(card);
	int err;

	/* Add play route control */
	err = snd_ctl_add(scard, snd_ctl_new1(&tegra_play_route_control, machine));
	if (err < 0)
		return err;

	/* Add capture route control */
	err = snd_ctl_add(scard, snd_ctl_new1(&tegra_capture_route_control, machine));
	if (err < 0)
		return err;

	/* Add call mode switch control */
	err = snd_ctl_add(scard, snd_ctl_new1(&tegra_call_mode_control, machine));
	if (err < 0)
		return err;

	return 0;
}

static struct snd_soc_jack_gpio tegra_jack_gpios[] = {
	{
		.name = "headphone detect",
		.report = SND_JACK_HEADPHONE,
		.debounce_time = 150,
		/*.gpio is filled in initialization from platform data */
	}
};

#ifdef CONFIG_SWITCH

/* These values are copied from Android WiredAccessoryObserver */
enum headset_state {
	BIT_NO_HEADSET = 0,
	BIT_HEADSET_NO_MIC = (1 << 1),
};

static struct switch_dev tegra_alc5624_headset_switch = {
	.name = "h2w",
};

static int tegra_alc5624_jack_notifier(struct notifier_block *self,
			      unsigned long action, void *dev)
{
	struct snd_soc_jack *jack = dev;
	struct snd_soc_codec *codec = jack->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_alc5624* machine = snd_soc_card_get_drvdata(card);
	enum headset_state state = BIT_NO_HEADSET;

	dev_dbg(card->dev,"Jack notifier: Status: 0x%08x\n",machine->jack_status);
	
	machine->jack_status &= ~SND_JACK_HEADPHONE;
	machine->jack_status |= (action & SND_JACK_HEADPHONE);

	switch (machine->jack_status) {
	case SND_JACK_HEADPHONE:
		state = BIT_HEADSET_NO_MIC;
		break;
	case SND_JACK_MICROPHONE:
		/* mic: would not report */
	default:
		state = BIT_NO_HEADSET;
	}

	switch_set_state(&tegra_alc5624_headset_switch, state);

	return NOTIFY_OK;
}

static struct notifier_block tegra_alc5624_jack_detect_nb = {
	.notifier_call = tegra_alc5624_jack_notifier,
};

#endif

/* ------- Headphone jack autodetection  -------- */
#if !defined(CONFIG_SWITCH) || defined(FORCE_AUTO_SPK_HP_SWITCH)
static struct snd_soc_jack_pin tegra_jack_pins[] = {
	/* Disable speaker when headphone is plugged in */
	{
		.pin = "Internal Speaker",
		.mask = SND_JACK_HEADPHONE,
		.invert = 1, /* Enable pin when status is not reported */
	},
	/* Enable headphone when status is reported */
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
		.invert = 0, /* Enable pin when status is reported */
	},
};
#endif


/*tegra machine dapm widgets */
static struct snd_soc_dapm_widget tegra_alc5624_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Internal Speaker", NULL),
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Internal Mic", NULL),
};

/* Tegra machine audio map (connections to the codec pins) */
static struct snd_soc_dapm_route shuttle_audio_map[] = {
	{"Headphone Jack", NULL, "HPR"},
	{"Headphone Jack", NULL, "HPL"},
	{"Internal Speaker", NULL, "SPKL"},
	{"Internal Speaker", NULL, "SPKLN"},
	{"Internal Speaker", NULL, "SPKR"},
	{"Internal Speaker", NULL, "SPKRN"},
	{"Mic Bias2", NULL, "Internal Mic"},
	{"MIC2", NULL, "Mic Bias2"},
};

static struct snd_kcontrol_new tegra_alc5624_controls[] = {
	SOC_DAPM_PIN_SWITCH("Internal Speaker"),
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("Internal Mic"),
};

static int tegra_alc5624_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec* codec = rtd->codec;
	struct snd_soc_dapm_context* dapm = &codec->dapm;
	struct snd_soc_card *card = codec->card;
	struct tegra_alc5624* machine = snd_soc_card_get_drvdata(card);
	int ret = 0;
	
	/* Add the controls used to route audio to bluetooth/voice */
	tegra_das_controls_init(codec);
	
	/* Store the GPIO used to detect headphone */
	tegra_jack_gpios[0].gpio = machine->gpio_hp_det;

	ret = snd_soc_add_controls(codec, tegra_alc5624_controls,
				   ARRAY_SIZE(tegra_alc5624_controls));
	if (ret < 0)
		return ret;

	snd_soc_dapm_new_controls(dapm, tegra_alc5624_dapm_widgets,
					ARRAY_SIZE(tegra_alc5624_dapm_widgets));

	snd_soc_dapm_add_routes(dapm, shuttle_audio_map,
					ARRAY_SIZE(shuttle_audio_map));

	/* Headphone jack detection */		
	ret = snd_soc_jack_new(codec, "Headphone Jack", SND_JACK_HEADPHONE,
			 &machine->tegra_jack);
	if (ret) {
		dev_err(card->dev,"Unable to register jack\n");
		return ret;
	}
	
#ifdef CONFIG_SWITCH		 
	/* Everytime the Jack status changes, notify our listerners */
	snd_soc_jack_notifier_register(&machine->tegra_jack,
				&tegra_alc5624_jack_detect_nb);
#endif

#if !defined(CONFIG_SWITCH) || defined(FORCE_AUTO_SPK_HP_SWITCH)
	/* Everytime the Jack status changes, update the DAPM pin status */
	snd_soc_jack_add_pins(&machine->tegra_jack,
			  ARRAY_SIZE(tegra_jack_pins),
			  tegra_jack_pins);
#endif

	/* Everytime the Jack detect gpio changes, report a Jack status change */
	ret = snd_soc_jack_add_gpios(&machine->tegra_jack,
			   ARRAY_SIZE(tegra_jack_gpios),
			   tegra_jack_gpios);
	if (ret)
		return ret;

	/* Set endpoints to not connected */
	snd_soc_dapm_nc_pin(dapm, "LINEL");
	snd_soc_dapm_nc_pin(dapm, "LINER");
	snd_soc_dapm_nc_pin(dapm, "PHONEIN");
	snd_soc_dapm_nc_pin(dapm, "MIC1");
	snd_soc_dapm_nc_pin(dapm, "MONO");

	/* Set endpoints to default off mode */
	snd_soc_dapm_enable_pin(dapm, "Internal Speaker");
	snd_soc_dapm_enable_pin(dapm, "Internal Mic");
	snd_soc_dapm_disable_pin(dapm, "Headphone Jack");

	snd_soc_dapm_force_enable_pin(dapm, "Mic Bias2");		
			  
	ret = snd_soc_dapm_sync(dapm);
	if (ret) {
		dev_err(card->dev,"Failed to sync\n");
		return ret;
	}
	
	return 0;
}

static struct snd_soc_dai_link tegra_alc5624_dai[] = {
	{
		.name = "ALC5624",
		.stream_name = "ALC5624 HiFi",
		.codec_name = "alc5624.0-0018",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra20-i2s.0",
		.codec_dai_name = "alc5624-hifi",
		.init = tegra_alc5624_init,
		.ops = &tegra_hifi_ops,
	},
	{
		.name = "VOICE",
		.stream_name = "Tegra Generic Voice",
		.codec_name = "tegra-generic-codec",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra20-i2s.1",
		.codec_dai_name = "tegra_generic_voice_codec",
		.ops = &tegra_voice_ops,
	},
	{
		.name = "SPDIF",
		.stream_name = "SPDIF PCM",
		.codec_name = "spdif-dit",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra20-spdif",
		.codec_dai_name = "dit-hifi",
		.ops = &tegra_spdif_ops,
	},
};

static struct snd_soc_card snd_soc_tegra_alc5624 = {
	.name		= "tegra-alc5624",
	.dai_link 	= tegra_alc5624_dai,
	.num_links 	= ARRAY_SIZE(tegra_alc5624_dai),
	
	.suspend_pre = tegra_soc_suspend_pre,
	.suspend_post = tegra_soc_suspend_post,
	.resume_pre = tegra_soc_resume_pre,
	.resume_post = tegra_soc_resume_post,
};

/* initialization */
static __devinit int tegra_snd_shuttle_probe(struct platform_device *pdev)
{
	struct snd_soc_card* card = &snd_soc_tegra_alc5624;
	struct tegra_alc5624 *machine;
	struct shuttle_audio_platform_data* pdata;	
	int ret = 0;
	
	/* Get platform data */
	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(&pdev->dev, "no platform data supplied\n");
		return -EINVAL;
	}

	/* Allocate private context */
	machine = kzalloc(sizeof(struct tegra_alc5624), GFP_KERNEL);
	if (!machine) {
		dev_err(&pdev->dev, "Can't allocate tegra_shuttle\n");
		return -ENOMEM;
	}
	
	ret = tegra_asoc_utils_init(&machine->util_data, &pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "Can't initialize Tegra ASOC utils\n");
		goto err_free_machine;
	}
	
	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card); 
	snd_soc_card_set_drvdata(card, machine);
	
	/* Fill in the GPIO used to detect the headphone */
	machine->gpio_hp_det = pdata->gpio_hp_det;
#ifndef USE_ORG_DAS	
	machine->hifi_codec_datafmt = pdata->hifi_codec_datafmt;	/* HiFi codec data format */
	machine->hifi_codec_master = pdata->hifi_codec_master;		/* If Hifi codec is master */
	machine->bt_codec_datafmt = pdata->bt_codec_datafmt;		/* Bluetooth codec data format */
	machine->bt_codec_master = pdata->bt_codec_master;			/* If bt codec is master */
#endif
	
	/* Add the device */
	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n",ret);
		goto err_fini_utils;
	}

#ifdef CONFIG_SWITCH
	/* Addd h2w swith class support */
	ret = switch_dev_register(&tegra_alc5624_headset_switch);
	if (ret < 0) {
		dev_err(&pdev->dev, "Can't register SWITCH device\n");
		goto err_unregister_card;
	}
#endif

	dev_info(&pdev->dev, "Shuttle sound card registered\n");

	return 0;
	
err_unregister_card:
	snd_soc_unregister_card(card);
err_fini_utils:
	tegra_asoc_utils_fini(&machine->util_data);
err_free_machine:
	kfree(machine);
	return ret;
	
}

static int __devexit tegra_snd_shuttle_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct tegra_alc5624 *machine = snd_soc_card_get_drvdata(card);
	
	snd_soc_unregister_card(card);
	
#ifdef CONFIG_SWITCH
	switch_dev_unregister(&tegra_alc5624_headset_switch);
#endif

	tegra_asoc_utils_fini(&machine->util_data);
	
	kfree(machine);
	
	return 0;
}

static struct platform_driver tegra_snd_shuttle_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
	},
	.probe = tegra_snd_shuttle_probe,
	.remove = __devexit_p(tegra_snd_shuttle_remove),
};

static int __init snd_tegra_shuttle_init(void)
{
	return platform_driver_register(&tegra_snd_shuttle_driver);
}
module_init(snd_tegra_shuttle_init);

static void __exit snd_tegra_shuttle_exit(void)
{
	platform_driver_unregister(&tegra_snd_shuttle_driver);
}
module_exit(snd_tegra_shuttle_exit);

MODULE_AUTHOR("Eduardo Jos� Tagle <ejtagle@tutopia.com>");
MODULE_DESCRIPTION("Shuttle machine ASoC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
