/*
 * This driver supports the digital controls for the internal codec
 * found in Allwinner's A33 SoCs.
 *
 * (C) Copyright 2010-2016
 * Reuuimlla Technology Co., Ltd. <www.reuuimllatech.com>
 * huangxin <huangxin@Reuuimllatech.com>
 * Mylène Josserand <mylene.josserand@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>

#define SUN8I_SYSCLK_CTL				0x00c
#define SUN8I_SYSCLK_CTL_AIF1CLK_ENA			11
#define SUN8I_SYSCLK_CTL_AIF1CLK_SRC_PLL		9
#define SUN8I_SYSCLK_CTL_AIF1CLK_SRC			8
#define SUN8I_SYSCLK_CTL_SYSCLK_ENA			3
#define SUN8I_SYSCLK_CTL_SYSCLK_SRC			0
#define SUN8I_MOD_CLK_ENA				0x010
#define SUN8I_MOD_CLK_ENA_AIF1				15
#define SUN8I_MOD_CLK_ENA_DAC				2
#define SUN8I_MOD_RST_CTL				0x014
#define SUN8I_MOD_RST_CTL_AIF1				15
#define SUN8I_MOD_RST_CTL_DAC				2
#define SUN8I_SYS_SR_CTRL				0x018
#define SUN8I_SYS_SR_CTRL_AIF1_FS			12
#define SUN8I_SYS_SR_CTRL_AIF2_FS			8
#define SUN8I_AIF1CLK_CTRL				0x040
#define SUN8I_AIF1CLK_CTRL_AIF1_MSTR_MOD		15
#define SUN8I_AIF1CLK_CTRL_AIF1_BCLK_INV		14
#define SUN8I_AIF1CLK_CTRL_AIF1_LRCK_INV		13
#define SUN8I_AIF1CLK_CTRL_AIF1_BCLK_DIV		9
#define SUN8I_AIF1CLK_CTRL_AIF1_LRCK_DIV		6
#define SUN8I_AIF1CLK_CTRL_AIF1_LRCK_DIV_16		(1 << 6)
#define SUN8I_AIF1CLK_CTRL_AIF1_WORD_SIZ		4
#define SUN8I_AIF1CLK_CTRL_AIF1_WORD_SIZ_16		(1 << 4)
#define SUN8I_AIF1CLK_CTRL_AIF1_DATA_FMT		2
#define SUN8I_AIF1_DACDAT_CTRL				0x048
#define SUN8I_AIF1_DACDAT_CTRL_AIF1_DA0L_ENA		15
#define SUN8I_AIF1_DACDAT_CTRL_AIF1_DA0R_ENA		14
#define SUN8I_DAC_DIG_CTRL				0x120
#define SUN8I_DAC_DIG_CTRL_ENDA			15
#define SUN8I_DAC_MXR_SRC				0x130
#define SUN8I_DAC_MXR_SRC_DACL_MXR_SRC_AIF1DA0L	15
#define SUN8I_DAC_MXR_SRC_DACL_MXR_SRC_AIF1DA1L	14
#define SUN8I_DAC_MXR_SRC_DACL_MXR_SRC_AIF2DACL	13
#define SUN8I_DAC_MXR_SRC_DACL_MXR_SRC_ADCL		12
#define SUN8I_DAC_MXR_SRC_DACR_MXR_SRC_AIF1DA0R	11
#define SUN8I_DAC_MXR_SRC_DACR_MXR_SRC_AIF1DA1R	10
#define SUN8I_DAC_MXR_SRC_DACR_MXR_SRC_AIF2DACR	9
#define SUN8I_DAC_MXR_SRC_DACR_MXR_SRC_ADCR		8

#define SUN8I_SYS_SR_CTRL_AIF1_FS_MASK		GENMASK(15, 12)
#define SUN8I_SYS_SR_CTRL_AIF2_FS_MASK		GENMASK(11, 8)
#define SUN8I_AIF1CLK_CTRL_AIF1_WORD_SIZ_MASK	GENMASK(5, 4)
#define SUN8I_AIF1CLK_CTRL_AIF1_LRCK_DIV_MASK	GENMASK(8, 6)

struct sun8i_codec {
	struct device	*dev;
	struct regmap	*regmap;
	struct clk	*clk_module;
	struct clk	*clk_bus;
};

static int sun8i_codec_runtime_resume(struct device *dev)
{
	struct sun8i_codec *scodec = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(scodec->clk_module);
	if (ret) {
		dev_err(dev, "Failed to enable the module clock\n");
		return ret;
	}

	ret = clk_prepare_enable(scodec->clk_bus);
	if (ret) {
		dev_err(dev, "Failed to enable the bus clock\n");
		goto err_disable_modclk;
	}

	regcache_cache_only(scodec->regmap, false);

	ret = regcache_sync(scodec->regmap);
	if (ret) {
		dev_err(dev, "Failed to sync regmap cache\n");
		goto err_disable_clk;
	}

	return 0;

err_disable_clk:
	clk_disable_unprepare(scodec->clk_bus);

err_disable_modclk:
	clk_disable_unprepare(scodec->clk_module);

	return ret;
}

static int sun8i_codec_runtime_suspend(struct device *dev)
{
	struct sun8i_codec *scodec = dev_get_drvdata(dev);

	regcache_cache_only(scodec->regmap, true);
	regcache_mark_dirty(scodec->regmap);

	clk_disable_unprepare(scodec->clk_module);
	clk_disable_unprepare(scodec->clk_bus);

	return 0;
}

static int sun8i_codec_get_hw_rate(struct snd_pcm_hw_params *params)
{
	unsigned int rate = params_rate(params);

	switch (rate) {
	case 8000:
	case 7350:
		return 0x0;
	case 11025:
		return 0x1;
	case 12000:
		return 0x2;
	case 16000:
		return 0x3;
	case 22050:
		return 0x4;
	case 24000:
		return 0x5;
	case 32000:
		return 0x6;
	case 44100:
		return 0x7;
	case 48000:
		return 0x8;
	case 96000:
		return 0x9;
	case 192000:
		return 0xa;
	default:
		return -EINVAL;
	}
}

static int sun8i_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct sun8i_codec *scodec = snd_soc_codec_get_drvdata(dai->codec);
	u32 value;

	/* clock masters */
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS: /* DAI Slave */
		value = 0x0; /* Codec Master */
		break;
	case SND_SOC_DAIFMT_CBM_CFM: /* DAI Master */
		value = 0x1; /* Codec Slave */
		break;
	default:
		return -EINVAL;
	}
	regmap_update_bits(scodec->regmap, SUN8I_AIF1CLK_CTRL,
			   BIT(SUN8I_AIF1CLK_CTRL_AIF1_MSTR_MOD),
			   value << SUN8I_AIF1CLK_CTRL_AIF1_MSTR_MOD);

	/* clock inversion */
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF: /* Normal */
		value = 0x0;
		break;
	case SND_SOC_DAIFMT_IB_IF: /* Inversion */
		value = 0x1;
		break;
	default:
		return -EINVAL;
	}
	regmap_update_bits(scodec->regmap, SUN8I_AIF1CLK_CTRL,
			   BIT(SUN8I_AIF1CLK_CTRL_AIF1_BCLK_INV),
			   value << SUN8I_AIF1CLK_CTRL_AIF1_BCLK_INV);
	regmap_update_bits(scodec->regmap, SUN8I_AIF1CLK_CTRL,
			   BIT(SUN8I_AIF1CLK_CTRL_AIF1_LRCK_INV),
			   value << SUN8I_AIF1CLK_CTRL_AIF1_LRCK_INV);

	/* DAI format */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		value = 0x0;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		value = 0x1;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		value = 0x2;
		break;
	case SND_SOC_DAIFMT_DSP_A:
	case SND_SOC_DAIFMT_DSP_B:
		value = 0x3;
		break;
	default:
		return -EINVAL;
	}
	regmap_update_bits(scodec->regmap, SUN8I_AIF1CLK_CTRL,
			   BIT(SUN8I_AIF1CLK_CTRL_AIF1_DATA_FMT),
			   value << SUN8I_AIF1CLK_CTRL_AIF1_DATA_FMT);

	return 0;
}

static int sun8i_codec_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct sun8i_codec *scodec = snd_soc_codec_get_drvdata(dai->codec);
	int sample_rate;

	/*
	 * The CPU DAI handles only a sample of 16 bits. Configure the
	 * codec to handle this type of sample resolution.
	 */
	regmap_update_bits(scodec->regmap, SUN8I_AIF1CLK_CTRL,
			   SUN8I_AIF1CLK_CTRL_AIF1_WORD_SIZ_MASK,
			   SUN8I_AIF1CLK_CTRL_AIF1_WORD_SIZ_16);

	regmap_update_bits(scodec->regmap, SUN8I_AIF1CLK_CTRL,
			   SUN8I_AIF1CLK_CTRL_AIF1_LRCK_DIV_MASK,
			   SUN8I_AIF1CLK_CTRL_AIF1_LRCK_DIV_16);

	sample_rate = sun8i_codec_get_hw_rate(params);
	if (sample_rate < 0)
		return sample_rate;

	regmap_update_bits(scodec->regmap, SUN8I_SYS_SR_CTRL,
			   SUN8I_SYS_SR_CTRL_AIF1_FS_MASK,
			   sample_rate << SUN8I_SYS_SR_CTRL_AIF1_FS);
	regmap_update_bits(scodec->regmap, SUN8I_SYS_SR_CTRL,
			   SUN8I_SYS_SR_CTRL_AIF2_FS_MASK,
			   sample_rate << SUN8I_SYS_SR_CTRL_AIF2_FS);

	regmap_update_bits(scodec->regmap, SUN8I_DAC_MXR_SRC, 0x8800, 0x8800);
	regmap_update_bits(scodec->regmap, 0x58, 0xffff, 0xa0a0);
	regmap_update_bits(scodec->regmap, 0x124, 0xffff, 0xa0a0);

	return 0;
}

static const struct snd_kcontrol_new sun8i_dac_mixer_controls[] = {
	SOC_DAPM_DOUBLE("AIF1 Slot 0 Digital DAC Playback Switch",
			SUN8I_DAC_MXR_SRC,
			SUN8I_DAC_MXR_SRC_DACL_MXR_SRC_AIF1DA0L,
			SUN8I_DAC_MXR_SRC_DACR_MXR_SRC_AIF1DA0R, 1, 0),
	SOC_DAPM_DOUBLE("AIF1 Slot 1 Digital DAC Playback Switch",
			SUN8I_DAC_MXR_SRC,
			SUN8I_DAC_MXR_SRC_DACL_MXR_SRC_AIF1DA1L,
			SUN8I_DAC_MXR_SRC_DACR_MXR_SRC_AIF1DA1R, 1, 0),
	SOC_DAPM_DOUBLE("AIF2 Digital DAC Playback Switch", SUN8I_DAC_MXR_SRC,
			SUN8I_DAC_MXR_SRC_DACL_MXR_SRC_AIF2DACL,
			SUN8I_DAC_MXR_SRC_DACR_MXR_SRC_AIF2DACR, 1, 0),
	SOC_DAPM_DOUBLE("ADC Digital DAC Playback Switch", SUN8I_DAC_MXR_SRC,
			SUN8I_DAC_MXR_SRC_DACL_MXR_SRC_ADCL,
			SUN8I_DAC_MXR_SRC_DACR_MXR_SRC_ADCR, 1, 0),
};

static const struct snd_soc_dapm_widget sun8i_codec_dapm_widgets[] = {
	/* Digital parts of the DACs */
	SND_SOC_DAPM_SUPPLY("DAC", SUN8I_DAC_DIG_CTRL, SUN8I_DAC_DIG_CTRL_ENDA,
			    0, NULL, 0),

	/* Analog DAC AIF */
	SND_SOC_DAPM_AIF_IN("AIF1 Slot 0 Left", "Playback", 0,
			    SUN8I_AIF1_DACDAT_CTRL,
			    SUN8I_AIF1_DACDAT_CTRL_AIF1_DA0L_ENA, 0),
	SND_SOC_DAPM_AIF_IN("AIF1 Slot 0 Right", "Playback", 0,
			    SUN8I_AIF1_DACDAT_CTRL,
			    SUN8I_AIF1_DACDAT_CTRL_AIF1_DA0R_ENA, 0),

	/* DAC Mixers */
	SOC_MIXER_ARRAY("Left Digital DAC Mixer", SND_SOC_NOPM, 0, 0,
			sun8i_dac_mixer_controls),
	SOC_MIXER_ARRAY("Right Digital DAC Mixer", SND_SOC_NOPM, 0, 0,
			sun8i_dac_mixer_controls),

	/* Clocks */
	SND_SOC_DAPM_SUPPLY("MODCLK AFI1", SUN8I_MOD_CLK_ENA,
			    SUN8I_MOD_CLK_ENA_AIF1, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("MODCLK DAC", SUN8I_MOD_CLK_ENA,
			    SUN8I_MOD_CLK_ENA_DAC, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("AIF1", SUN8I_SYSCLK_CTL,
			    SUN8I_SYSCLK_CTL_AIF1CLK_ENA, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("SYSCLK", SUN8I_SYSCLK_CTL,
			    SUN8I_SYSCLK_CTL_SYSCLK_ENA, 0, NULL, 0),

	SND_SOC_DAPM_SUPPLY("AIF1 PLL", SUN8I_SYSCLK_CTL,
			    SUN8I_SYSCLK_CTL_AIF1CLK_SRC_PLL, 0, NULL, 0),
	/* Inversion as 0=AIF1, 1=AIF2 */
	SND_SOC_DAPM_SUPPLY("SYSCLK AIF1", SUN8I_SYSCLK_CTL,
			    SUN8I_SYSCLK_CTL_SYSCLK_SRC, 1, NULL, 0),

	/* Module reset */
	SND_SOC_DAPM_SUPPLY("RST AIF1", SUN8I_MOD_RST_CTL,
			    SUN8I_MOD_RST_CTL_AIF1, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("RST DAC", SUN8I_MOD_RST_CTL,
			    SUN8I_MOD_RST_CTL_DAC, 0, NULL, 0),
};

static const struct snd_soc_dapm_route sun8i_codec_dapm_routes[] = {
	/* Clock Routes */
	{ "AIF1", NULL, "SYSCLK AIF1" },
	{ "AIF1 PLL", NULL, "AIF1" },
	{ "RST AIF1", NULL, "AIF1 PLL" },
	{ "MODCLK AFI1", NULL, "RST AIF1" },
	{ "DAC", NULL, "MODCLK AFI1" },

	{ "RST DAC", NULL, "SYSCLK" },
	{ "MODCLK DAC", NULL, "RST DAC" },
	{ "DAC", NULL, "MODCLK DAC" },

	/* DAC Routes */
	{ "AIF1 Slot 0 Right", NULL, "DAC" },
	{ "AIF1 Slot 0 Left", NULL, "DAC" },

	/* DAC Mixer Routes */
	{ "Left Digital DAC Mixer", "AIF1 Slot 0 Digital DAC Playback Switch",
	  "AIF1 Slot 0 Left"},
	{ "Right Digital DAC Mixer", "AIF1 Slot 0 Digital DAC Playback Switch",
	  "AIF1 Slot 0 Right"},
};

static const struct snd_soc_dai_ops sun8i_codec_dai_ops = {
	.hw_params = sun8i_codec_hw_params,
	.set_fmt = sun8i_set_fmt,
};

static struct snd_soc_dai_driver sun8i_codec_dai = {
	.name = "sun8i",
	/* playback capabilities */
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_192000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	/* pcm operations */
	.ops = &sun8i_codec_dai_ops,
};

static const struct snd_soc_codec_driver sun8i_soc_codec = {
	.component_driver = {
		.dapm_widgets		= sun8i_codec_dapm_widgets,
		.num_dapm_widgets	= ARRAY_SIZE(sun8i_codec_dapm_widgets),
		.dapm_routes		= sun8i_codec_dapm_routes,
		.num_dapm_routes	= ARRAY_SIZE(sun8i_codec_dapm_routes),
	},
};

static const struct regmap_config sun8i_codec_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= SUN8I_DAC_MXR_SRC,

	.cache_type	= REGCACHE_FLAT,
};

static int sun8i_codec_probe(struct platform_device *pdev)
{
	struct resource *res_base;
	struct sun8i_codec *scodec;
	void __iomem *base;
	int ret;

	scodec = devm_kzalloc(&pdev->dev, sizeof(*scodec), GFP_KERNEL);
	if (!scodec)
		return -ENOMEM;

	scodec->dev = &pdev->dev;

	scodec->clk_module = devm_clk_get(&pdev->dev, "mod");
	if (IS_ERR(scodec->clk_module)) {
		dev_err(&pdev->dev, "Failed to get the module clock\n");
		return PTR_ERR(scodec->clk_module);
	}

	scodec->clk_bus = devm_clk_get(&pdev->dev, "bus");
	if (IS_ERR(scodec->clk_bus)) {
		dev_err(&pdev->dev, "Failed to get the bus clock\n");
		return PTR_ERR(scodec->clk_bus);
	}

	res_base = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res_base);
	if (IS_ERR(base)) {
		dev_err(&pdev->dev, "Failed to map the registers\n");
		return PTR_ERR(base);
	}

	scodec->regmap = devm_regmap_init_mmio(&pdev->dev, base,
					       &sun8i_codec_regmap_config);
	if (IS_ERR(scodec->regmap)) {
		dev_err(&pdev->dev, "Failed to create our regmap\n");
		return PTR_ERR(scodec->regmap);
	}

	platform_set_drvdata(pdev, scodec);

	pm_runtime_enable(&pdev->dev);
	if (!pm_runtime_enabled(&pdev->dev)) {
		ret = sun8i_codec_runtime_resume(&pdev->dev);
		if (ret)
			goto err_pm_disable;
	}

	ret = snd_soc_register_codec(&pdev->dev, &sun8i_soc_codec,
				     &sun8i_codec_dai, 1);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register codec\n");
		goto err_suspend;
	}

	return ret;

err_suspend:
	if (!pm_runtime_status_suspended(&pdev->dev))
		sun8i_codec_runtime_suspend(&pdev->dev);

err_pm_disable:
	pm_runtime_disable(&pdev->dev);

	return ret;
}

static int sun8i_codec_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct sun8i_codec *scodec = snd_soc_card_get_drvdata(card);

	pm_runtime_disable(&pdev->dev);
	if (!pm_runtime_status_suspended(&pdev->dev))
		sun8i_codec_runtime_suspend(&pdev->dev);

	snd_soc_unregister_codec(&pdev->dev);
	clk_disable_unprepare(scodec->clk_module);
	clk_disable_unprepare(scodec->clk_bus);

	return 0;
}

static const struct of_device_id sun8i_codec_of_match[] = {
	{ .compatible = "allwinner,sun8i-a33-codec" },
	{}
};
MODULE_DEVICE_TABLE(of, sun8i_codec_of_match);

static const struct dev_pm_ops sun8i_codec_pm_ops = {
	SET_RUNTIME_PM_OPS(sun8i_codec_runtime_suspend,
			   sun8i_codec_runtime_resume, NULL)
};

static struct platform_driver sun8i_codec_driver = {
	.driver = {
		.name = "sun8i-codec",
		.of_match_table = sun8i_codec_of_match,
		.pm = &sun8i_codec_pm_ops,
	},
	.probe = sun8i_codec_probe,
	.remove = sun8i_codec_remove,
};
module_platform_driver(sun8i_codec_driver);

MODULE_DESCRIPTION("Allwinner A33 (sun8i) codec driver");
MODULE_AUTHOR("Mylène Josserand <mylene.josserand@free-electrons.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:sun8i-codec");
