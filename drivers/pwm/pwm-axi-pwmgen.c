// SPDX-License-Identifier: GPL-2.0
/*
 * Analog Devices AXI PWM generator
 *
 * Copyright 2020 Analog Devices Inc.
 */
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/slab.h>

#define AXI_PWMGEN_REG_CORE_VERSION	0x00
#define AXI_PWMGEN_REG_ID		0x04
#define AXI_PWMGEN_REG_SCRATCHPAD	0x08
#define AXI_PWMGEN_REG_CORE_MAGIC	0x0C
#define AXI_PWMGEN_REG_CONFIG		0x10
#define AXI_PWMGEN_REG_NPWM		0x14
#define AXI_PWMGEN_CH_PERIOD_BASE	0x40
#define AXI_PWMGEN_CH_DUTY_BASE		0x44
#define AXI_PWMGEN_CH_OFFSET_BASE	0x48
#define AXI_PWMGEN_CHX_PERIOD(ch)	(AXI_PWMGEN_CH_PERIOD_BASE + (12 * (ch)))
#define AXI_PWMGEN_CHX_DUTY(ch)		(AXI_PWMGEN_CH_DUTY_BASE + (12 * (ch)))
#define AXI_PWMGEN_CHX_OFFSET(ch)	(AXI_PWMGEN_CH_OFFSET_BASE + (12 * (ch)))
#define AXI_PWMGEN_TEST_DATA		0x5A0F0081
#define AXI_PWMGEN_LOAD_CONIG		BIT(1)
#define AXI_PWMGEN_RESET		BIT(0)

#define AXI_PWMGEN_PSEC_PER_SEC		1000000000000ULL

static const unsigned long long axi_pwmgen_scales[] = {
	[PWM_UNIT_SEC]  = 1000000000000ULL,
	[PWM_UNIT_MSEC] = 1000000000ULL,
	[PWM_UNIT_USEC] = 1000000ULL,
	[PWM_UNIT_NSEC] = 1000ULL,
	[PWM_UNIT_PSEC] = 1ULL,
};

struct axi_pwmgen {
	struct pwm_chip		chip;
	struct clk		*clk;
	void __iomem		*base;

	/* Used to store the period when the channel is disabled */
	unsigned long long	ch_period[4];

#ifdef CONFIG_PWM_AXI_PWMGEN_DEBUG
	unsigned char		debug_channel;
#endif
};

static inline unsigned int axi_pwmgen_read(struct axi_pwmgen *pwm,
					   unsigned int reg)
{
	return readl(pwm->base + reg);
}

static inline void axi_pwmgen_write(struct axi_pwmgen *pwm,
				    unsigned int reg,
				    unsigned int value)
{
	writel(value, pwm->base + reg);
}

static void axi_pwmgen_write_mask(struct axi_pwmgen *pwm,
				  unsigned int reg,
				  unsigned int mask,
				  unsigned int value)
{
	unsigned int temp;

	temp = axi_pwmgen_read(pwm, reg);
	axi_pwmgen_write(pwm, reg, (temp & ~mask) | value);
}

static inline struct axi_pwmgen *to_axi_pwmgen(struct pwm_chip *chip)
{
	return container_of(chip, struct axi_pwmgen, chip);
}

static int axi_pwmgen_apply(struct pwm_chip *chip, struct pwm_device *device,
			    const struct pwm_state *state)
{
	unsigned long long rate, clk_period_ps, target, cnt;
	struct axi_pwmgen *pwm = to_axi_pwmgen(chip);
	unsigned int ch = device->hwpwm;

	rate = clk_get_rate(pwm->clk);
	clk_period_ps = DIV_ROUND_CLOSEST_ULL(AXI_PWMGEN_PSEC_PER_SEC, rate);

	target = state->period * axi_pwmgen_scales[state->time_unit];
	cnt = DIV_ROUND_CLOSEST_ULL(target, clk_period_ps);
	pwm->ch_period[ch] = cnt;
	axi_pwmgen_write(pwm, AXI_PWMGEN_CHX_PERIOD(ch),
			 state->enabled ? pwm->ch_period[ch] : 0);

	target = state->duty_cycle * axi_pwmgen_scales[state->time_unit];
	cnt = DIV_ROUND_CLOSEST_ULL(target, clk_period_ps);
	axi_pwmgen_write(pwm, AXI_PWMGEN_CHX_DUTY(ch), cnt);

	target = state->offset * axi_pwmgen_scales[state->time_unit];
	cnt = DIV_ROUND_CLOSEST_ULL(target, clk_period_ps);
	axi_pwmgen_write(pwm, AXI_PWMGEN_CHX_OFFSET(ch),
			 state->offset ? cnt : 0);

	/* Apply the new config */
	axi_pwmgen_write(pwm, AXI_PWMGEN_REG_CONFIG, AXI_PWMGEN_LOAD_CONIG);

	return 0;
}

static int axi_pwmgen_capture(struct pwm_chip *chip, struct pwm_device *device,
			      struct pwm_capture *capture,
			      unsigned long timeout)
{
	struct axi_pwmgen *pwmgen = to_axi_pwmgen(chip);
	unsigned long long rate, cnt, clk_period_ps;
	unsigned int ch = device->hwpwm;

	(void)(timeout);

	rate = clk_get_rate(pwmgen->clk);
	clk_period_ps = DIV_ROUND_CLOSEST_ULL(AXI_PWMGEN_PSEC_PER_SEC, rate);

	cnt = axi_pwmgen_read(pwmgen, AXI_PWMGEN_CHX_PERIOD(ch));
	capture->period = cnt * clk_period_ps;
	cnt = axi_pwmgen_read(pwmgen, AXI_PWMGEN_CHX_DUTY(ch));
	capture->duty_cycle = cnt * clk_period_ps;
	cnt = axi_pwmgen_read(pwmgen, AXI_PWMGEN_CHX_OFFSET(ch));
	capture->offset = cnt * clk_period_ps;
	capture->time_unit = PWM_UNIT_PSEC;

	return 0;
}

static void axi_pwmgen_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	unsigned int ch = pwm->hwpwm;
	struct axi_pwmgen *pwmgen = to_axi_pwmgen(chip);

	axi_pwmgen_write(pwmgen, AXI_PWMGEN_CHX_PERIOD(ch), 0);
	axi_pwmgen_write(pwmgen, AXI_PWMGEN_REG_CONFIG, AXI_PWMGEN_LOAD_CONIG);
}

static int axi_pwmgen_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	unsigned int ch = pwm->hwpwm;
	struct axi_pwmgen *pwmgen = to_axi_pwmgen(chip);

	axi_pwmgen_write(pwmgen, AXI_PWMGEN_CHX_PERIOD(ch), pwmgen->ch_period[ch]);
	axi_pwmgen_write(pwmgen, AXI_PWMGEN_REG_CONFIG, AXI_PWMGEN_LOAD_CONIG);

	return 0;
}

static const struct pwm_ops axi_pwmgen_pwm_ops = {
	.apply = axi_pwmgen_apply,
	.disable = axi_pwmgen_disable,
	.enable = axi_pwmgen_enable,
	.capture = axi_pwmgen_capture,
	.owner = THIS_MODULE,
};

static const struct of_device_id axi_pwmgen_ids[] = {
	{
		.compatible = "adi,axi-pwmgen",
	},
	{ }
};
MODULE_DEVICE_TABLE(of, axi_pwmgen_ids);

static int axi_pwmgen_setup(struct pwm_chip *chip)
{
	struct axi_pwmgen *pwm;
	unsigned int reg;
	int idx;

	pwm = to_axi_pwmgen(chip);
	axi_pwmgen_write(pwm, AXI_PWMGEN_REG_SCRATCHPAD, AXI_PWMGEN_TEST_DATA);
	reg = axi_pwmgen_read(pwm, AXI_PWMGEN_REG_SCRATCHPAD);
	if (reg != AXI_PWMGEN_TEST_DATA) {
		dev_err(chip->dev, "failed to access the device registers\n");
		return -EIO;
	}

	pwm->chip.npwm = axi_pwmgen_read(pwm, AXI_PWMGEN_REG_NPWM);
	if (pwm->chip.npwm > 4)
		return -EINVAL;

	/* Disable all the outputs */
	for (idx = 0; idx < pwm->chip.npwm; idx++) {
		axi_pwmgen_write(pwm, AXI_PWMGEN_CHX_PERIOD(idx), 0);
		axi_pwmgen_write(pwm, AXI_PWMGEN_CHX_DUTY(idx), 0);
		axi_pwmgen_write(pwm, AXI_PWMGEN_CHX_OFFSET(idx), 0);
	}

	/* Enable the core */
	axi_pwmgen_write_mask(pwm, AXI_PWMGEN_REG_CONFIG, AXI_PWMGEN_RESET, 0);

	return 0;
}

static void axi_pwmgen_clk_disable(void *data)
{
	clk_disable_unprepare(data);
}

#ifdef CONFIG_PWM_AXI_PWMGEN_DEBUG

static ssize_t pwmgen_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct axi_pwmgen *pwm = dev_get_drvdata(dev);
	struct pwm_capture capture;
	struct pwm_device pwmdev;
	int val;

	pwmdev.hwpwm = pwm->debug_channel;
	axi_pwmgen_capture(&pwm->chip, &pwmdev, &capture, 0);

	if (strcmp(attr->attr.name, "period") == 0) {
		val = capture.period * axi_pwmgen_scales[capture.time_unit];
		return sprintf(buf, "0x%X\n", val);
	} else if (strcmp(attr->attr.name, "duty") == 0) {
		val = capture.duty_cycle * axi_pwmgen_scales[capture.time_unit];
		return sprintf(buf, "0x%X\n", val);
	} else if (strcmp(attr->attr.name, "offset") == 0) {
		val = capture.offset * axi_pwmgen_scales[capture.time_unit];
		return sprintf(buf, "0x%X\n", val);
	} else if (strcmp(attr->attr.name, "channel") == 0) {
		return sprintf(buf, "0x%X\n", pwm->debug_channel);
	} else {
		return -EINVAL;
	}
}

static ssize_t pwmgen_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct axi_pwmgen *pwm = dev_get_drvdata(dev);
	struct pwm_capture capture;
	struct pwm_device pwmdev;
	struct pwm_state state;
	long val;

	pwmdev.hwpwm = pwm->debug_channel;
	axi_pwmgen_capture(&pwm->chip, &pwmdev, &capture, 0);
	state.enabled = true;
	state.time_unit = PWM_UNIT_NSEC;
	state.period = capture.period;
	state.duty_cycle = capture.duty_cycle;
	state.offset = capture.offset;

	kstrtol(buf, 0, &val);
	if (strcmp(attr->attr.name, "period") == 0) {
		state.period = val;
	} else if (strcmp(attr->attr.name, "duty") == 0) {
		state.duty_cycle = val;
	} else if (strcmp(attr->attr.name, "offset") == 0) {
		state.offset = val;
	} else if (strcmp(attr->attr.name, "channel") == 0) {
		if (val > pwm->chip.npwm || val < 0)
			return -EINVAL;
		pwm->debug_channel = val;
		return count;
	} else {
		return -EINVAL;
	}

	axi_pwmgen_apply(&pwm->chip, &pwmdev, &state);

	return count;
}

static DEVICE_ATTR(period, 0644, pwmgen_show, pwmgen_store);
static DEVICE_ATTR(duty, 0644, pwmgen_show, pwmgen_store);
static DEVICE_ATTR(offset, 0644, pwmgen_show, pwmgen_store);
static DEVICE_ATTR(channel, 0644, pwmgen_show, pwmgen_store);

#endif//CONFIG_PWM_AXI_PWMGEN_DEBUG

static int axi_pwmgen_probe(struct platform_device *pdev)
{
	struct axi_pwmgen *pwm;
	struct resource *mem;
	int ret;

	pwm = devm_kzalloc(&pdev->dev, sizeof(*pwm), GFP_KERNEL);
	if (!pwm)
		return -ENOMEM;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pwm->base = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(pwm->base))
		return PTR_ERR(pwm->base);

	pwm->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(pwm->clk))
		return PTR_ERR(pwm->clk);

	ret = clk_prepare_enable(pwm->clk);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(&pdev->dev, axi_pwmgen_clk_disable,
				       pwm->clk);
	if (ret)
		return ret;

	pwm->chip.dev = &pdev->dev;
	pwm->chip.ops = &axi_pwmgen_pwm_ops;
	pwm->chip.base = -1;

	ret = axi_pwmgen_setup(&pwm->chip);
	if (ret < 0)
		return ret;

	ret = pwmchip_add(&pwm->chip);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, pwm);

#ifdef CONFIG_PWM_AXI_PWMGEN_DEBUG
	device_create_file(&pdev->dev, &dev_attr_period);
	device_create_file(&pdev->dev, &dev_attr_offset);
	device_create_file(&pdev->dev, &dev_attr_duty);
	device_create_file(&pdev->dev, &dev_attr_channel);
#endif
	return 0;
}

static int axi_pwmgen_remove(struct platform_device *pdev)
{
	struct axi_pwmgen *pwm = platform_get_drvdata(pdev);

	return pwmchip_remove(&pwm->chip);
}
static struct platform_driver axi_pwmgen_driver = {
	.driver = {
		.name = "adi,axi-pwmgen",
		.of_match_table = axi_pwmgen_ids,
	},
	.probe = axi_pwmgen_probe,
	.remove = axi_pwmgen_remove,
};

module_platform_driver(axi_pwmgen_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Sergiu Cuciurean <sergiu.cuciurean@analog>");
MODULE_DESCRIPTION("Driver for the Analog Devices AXI PWM generator");
