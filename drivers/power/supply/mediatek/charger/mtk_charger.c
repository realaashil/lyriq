/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 MediaTek Inc.
*/

/*
 *
 * Filename:
 * ---------
 *    mtk_charger.c
 *
 * Project:
 * --------
 *   Android_Software
 *
 * Description:
 * ------------
 *   This Module defines functions of Battery charging
 *
 * Author:
 * -------
 * Wy Chuang
 *
 */
// #define DEBUG
#include <linux/init.h>		/* For init/exit macros */
#include <linux/module.h>	/* For MODULE_ marcros  */
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/power_supply.h>
#include <linux/pm_wakeup.h>
#include <linux/time.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/proc_fs.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/scatterlist.h>
#include <linux/suspend.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/reboot.h>

#include <mt-plat/v1/charger_type.h>
#include <mt-plat/v1/mtk_battery.h>
#include <mt-plat/mtk_boot.h>
#include <pmic.h>
#include <mtk_gauge_time_service.h>

#include "mtk_charger_intf.h"
#include "mtk_charger_init.h"
#include <linux/qpnp_adaptive_charge.h>
#include <linux/of_gpio.h>

static char atm_mode[10];
int __init atm_mode_init(char *s)
{
	strlcpy(atm_mode, s, 10);
	return 1;
}
__setup("androidboot.atm=", atm_mode_init);

static struct charger_manager *pinfo;
static struct list_head consumer_head = LIST_HEAD_INIT(consumer_head);
static DEFINE_MUTEX(consumer_mutex);

struct tag_bootmode {
	u32 size;
	u32 tag;
	u32 bootmode;
	u32 boottype;
};

bool mtk_is_TA_support_pd_pps(struct charger_manager *pinfo)
{
	if (pinfo->enable_pe_4 == false
		&& pinfo->enable_pe_5 == false
		&& pinfo->enable_cp == false)
		return false;

	if (pinfo->pd_type == MTK_PD_CONNECT_PE_READY_SNK_APDO)
		return true;
	return false;
}

bool is_power_path_supported(void)
{
	if (pinfo == NULL)
		return false;

	if (pinfo->data.power_path_support == true)
		return true;

	return false;
}

bool is_disable_charger(void)
{
	if (pinfo == NULL)
		return true;

	if (pinfo->disable_charger == true || IS_ENABLED(CONFIG_POWER_EXT))
		return true;
	else
		return false;
}

void BATTERY_SetUSBState(int usb_state_value)
{
	if (is_disable_charger()) {
		chr_err("[%s] in FPGA/EVB, no service\n", __func__);
	} else {
		if ((usb_state_value < USB_SUSPEND) ||
			((usb_state_value > USB_CONFIGURED))) {
			chr_err("%s Fail! Restore to default value\n",
				__func__);
			usb_state_value = USB_UNCONFIGURED;
		} else {
			chr_err("%s Success! Set %d\n", __func__,
				usb_state_value);
			if (pinfo)
				pinfo->usb_state = usb_state_value;
		}
	}
}

unsigned int set_chr_input_current_limit(int current_limit)
{
	return 500;
}

int get_chr_temperature(int *min_temp, int *max_temp)
{
	*min_temp = 25;
	*max_temp = 30;

	return 0;
}

int set_chr_boost_current_limit(unsigned int current_limit)
{
	return 0;
}

int set_chr_enable_otg(unsigned int enable)
{
	return 0;
}

int mtk_chr_is_charger_exist(unsigned char *exist)
{
	if (mt_get_charger_type() == CHARGER_UNKNOWN)
		*exist = 0;
	else
		*exist = 1;
	return 0;
}

/*=============== fix me==================*/
int chargerlog_level = CHRLOG_ERROR_LEVEL;

int chr_get_debug_level(void)
{
	return chargerlog_level;
}

#ifdef MTK_CHARGER_EXP
#include <linux/string.h>

char chargerlog[1000];
#define LOG_LENGTH 500
int chargerlog_level = 10;
int chargerlogIdx;

int charger_get_debug_level(void)
{
	return chargerlog_level;
}

void charger_log(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vsprintf(chargerlog + chargerlogIdx, fmt, args);
	va_end(args);
	chargerlogIdx = strlen(chargerlog);
	if (chargerlogIdx >= LOG_LENGTH) {
		chr_err("%s", chargerlog);
		chargerlogIdx = 0;
		memset(chargerlog, 0, 1000);
	}
}

void charger_log_flash(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vsprintf(chargerlog + chargerlogIdx, fmt, args);
	va_end(args);
	chr_err("%s", chargerlog);
	chargerlogIdx = 0;
	memset(chargerlog, 0, 1000);
}
#endif

void _wake_up_charger(struct charger_manager *info)
{
	unsigned long flags;

	if (info == NULL)
		return;

	spin_lock_irqsave(&info->slock, flags);
	if (!info->charger_wakelock->active)
		__pm_stay_awake(info->charger_wakelock);
	spin_unlock_irqrestore(&info->slock, flags);
	info->charger_thread_timeout = true;
	wake_up_interruptible(&info->wait_que);
}

/* charger_manager ops  */
static int _mtk_charger_change_current_setting(struct charger_manager *info)
{
	if (info != NULL && info->change_current_setting)
		return info->change_current_setting(info);

	return 0;
}

static int _mtk_charger_do_charging(struct charger_manager *info, bool en)
{
	if (info != NULL && info->do_charging)
		info->do_charging(info, en);
	return 0;
}
/* charger_manager ops end */


/* user interface */
struct charger_consumer *charger_manager_get_by_name(struct device *dev,
	const char *name)
{
	struct charger_consumer *puser;

	puser = kzalloc(sizeof(struct charger_consumer), GFP_KERNEL);
	if (puser == NULL)
		return NULL;

	mutex_lock(&consumer_mutex);
	puser->dev = dev;

	list_add(&puser->list, &consumer_head);
	if (pinfo != NULL)
		puser->cm = pinfo;

	mutex_unlock(&consumer_mutex);

	return puser;
}
EXPORT_SYMBOL(charger_manager_get_by_name);

int charger_manager_enable_high_voltage_charging(
			struct charger_consumer *consumer, bool en)
{
	struct charger_manager *info = consumer->cm;
	struct list_head *pos = NULL;
	struct list_head *phead = &consumer_head;
	struct charger_consumer *ptr = NULL;

	if (!info)
		return -EINVAL;
	if (info->mmi.factory_mode)
		return 0;
	pr_debug("[%s] %s, %d\n", __func__, dev_name(consumer->dev), en);

	if (!en && consumer->hv_charging_disabled == false)
		consumer->hv_charging_disabled = true;
	else if (en && consumer->hv_charging_disabled == true)
		consumer->hv_charging_disabled = false;
	else {
		pr_info("[%s] already set: %d %d\n", __func__,
			consumer->hv_charging_disabled, en);
		return 0;
	}

	mutex_lock(&consumer_mutex);
	list_for_each(pos, phead) {
		ptr = container_of(pos, struct charger_consumer, list);
		if (ptr->hv_charging_disabled == true) {
			info->enable_hv_charging = false;
			break;
		}
		if (list_is_last(pos, phead))
			info->enable_hv_charging = true;
	}
	mutex_unlock(&consumer_mutex);

	pr_info("%s: user: %s, en = %d\n", __func__, dev_name(consumer->dev),
		info->enable_hv_charging);

	if (mtk_pe50_get_is_connect(info) && !info->enable_hv_charging)
		mtk_pe50_stop_algo(info, true);

	_wake_up_charger(info);

	return 0;
}
EXPORT_SYMBOL(charger_manager_enable_high_voltage_charging);

int charger_manager_enable_power_path(struct charger_consumer *consumer,
	int idx, bool en)
{
	int ret = 0;
	bool is_en = true;
	struct charger_manager *info = consumer->cm;
	struct charger_device *chg_dev = NULL;

	if (!info)
		return -EINVAL;

	switch (idx) {
	case MAIN_CHARGER:
		chg_dev = info->chg1_dev;
		break;
	case SLAVE_CHARGER:
		chg_dev = info->chg2_dev;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&info->pp_lock[idx]);
	info->enable_pp[idx] = en;

	if (info->force_disable_pp[idx])
		goto out;

	ret = charger_dev_is_powerpath_enabled(chg_dev, &is_en);
	if (ret < 0) {
		chr_err("%s: get is power path enabled failed\n", __func__);
		goto out;
	}
	if (is_en == en) {
		chr_err("%s: power path is already en = %d\n", __func__, is_en);
		goto out;
	}

	pr_info("%s: enable power path = %d\n", __func__, en);
	ret = charger_dev_enable_powerpath(chg_dev, en);
out:
	mutex_unlock(&info->pp_lock[idx]);
	return ret;
}

int charger_manager_force_disable_power_path(struct charger_consumer *consumer,
	int idx, bool disable)
{
	struct charger_manager *info = consumer->cm;
	struct charger_device *chg_dev = NULL;
	int ret = 0;
	if (info == NULL)
		return -ENODEV;
	switch (idx) {
	case MAIN_CHARGER:
		chg_dev = info->chg1_dev;
		break;
	case SLAVE_CHARGER:
		chg_dev = info->chg2_dev;
		break;
	default:
		ret = -EINVAL;
	}

	mutex_lock(&info->pp_lock[idx]);

	if (disable == info->force_disable_pp[idx])
		goto out;

	info->force_disable_pp[idx] = disable;
	ret = charger_dev_enable_powerpath(chg_dev,
		info->force_disable_pp[idx] ? false : info->enable_pp[idx]);
out:
	mutex_unlock(&info->pp_lock[idx]);
	return ret;
}

static int _charger_manager_enable_charging(struct charger_consumer *consumer,
	int idx, bool en)
{
	struct charger_manager *info = consumer->cm;

	chr_err("%s: dev:%s idx:%d en:%d\n", __func__, dev_name(consumer->dev),
		idx, en);

	if (info != NULL) {
		struct charger_data *pdata = NULL;

		if (idx == MAIN_CHARGER)
			pdata = &info->chg1_data;
		else if (idx == SLAVE_CHARGER)
			pdata = &info->chg2_data;
		else
			return -ENOTSUPP;

		if (en == false) {
			_mtk_charger_do_charging(info, en);
			pdata->disable_charging_count++;
		} else {
			if (pdata->disable_charging_count == 1) {
				_mtk_charger_do_charging(info, en);
				pdata->disable_charging_count = 0;
			} else if (pdata->disable_charging_count > 1)
				pdata->disable_charging_count--;
		}
		chr_err("%s: dev:%s idx:%d en:%d cnt:%d\n", __func__,
			dev_name(consumer->dev), idx, en,
			pdata->disable_charging_count);

		return 0;
	}
	return -EBUSY;

}

int charger_manager_enable_charging(struct charger_consumer *consumer,
	int idx, bool en)
{
	struct charger_manager *info = consumer->cm;
	int ret = 0;

	mutex_lock(&info->charger_lock);
	ret = _charger_manager_enable_charging(consumer, idx, en);
	mutex_unlock(&info->charger_lock);
	return ret;
}

int charger_manager_set_input_current_limit(struct charger_consumer *consumer,
	int idx, int input_current)
{
	struct charger_manager *info = consumer->cm;

	if (info != NULL) {
		struct charger_data *pdata;

		if (info->mmi.factory_mode)
			return -EBUSY;

		if (info->data.parallel_vbus) {
			if (idx == TOTAL_CHARGER) {
				info->chg1_data.thermal_input_current_limit =
					input_current;
				info->chg2_data.thermal_input_current_limit =
					input_current;
			} else
				return -ENOTSUPP;
		} else {
			if (idx == MAIN_CHARGER)
				pdata = &info->chg1_data;
			else if (idx == SLAVE_CHARGER)
				pdata = &info->chg2_data;
			else if (idx == MAIN_DIVIDER_CHARGER)
				pdata = &info->dvchg1_data;
			else if (idx == SLAVE_DIVIDER_CHARGER)
				pdata = &info->dvchg2_data;
			else
				return -ENOTSUPP;
			pdata->thermal_input_current_limit = input_current;
		}

		chr_err("%s: dev:%s idx:%d en:%d\n", __func__,
			dev_name(consumer->dev), idx, input_current);
		#ifdef MTK_BASE
		_mtk_charger_change_current_setting(info);
		#endif
		_wake_up_charger(info);
		return 0;
	}
	return -EBUSY;
}

int charger_manager_set_charging_current_limit(
	struct charger_consumer *consumer, int idx, int charging_current)
{
	struct charger_manager *info = consumer->cm;

	if (info != NULL) {
		struct charger_data *pdata;

		if (info->mmi.factory_mode)
			return -EBUSY;
		if (idx == MAIN_CHARGER)
			pdata = &info->chg1_data;
		else if (idx == SLAVE_CHARGER)
			pdata = &info->chg2_data;
		else
			return -ENOTSUPP;

		pdata->thermal_charging_current_limit = charging_current;
		chr_err("%s: dev:%s idx:%d en:%d\n", __func__,
			dev_name(consumer->dev), idx, charging_current);
		#ifdef MTK_BASE
		_mtk_charger_change_current_setting(info);
		#endif
		_wake_up_charger(info);
		return 0;
	}
	return -EBUSY;
}

int charger_manager_get_charger_temperature(struct charger_consumer *consumer,
	int idx, int *tchg_min,	int *tchg_max)
{
	struct charger_manager *info = consumer->cm;

	if (info != NULL) {
		struct charger_data *pdata = NULL;

		if (!upmu_get_rgs_chrdet()) {
			pr_debug("[%s] No cable in, skip it\n", __func__);
			*tchg_min = -127;
			*tchg_max = -127;
			return -EINVAL;
		}

		if (idx == MAIN_CHARGER)
			pdata = &info->chg1_data;
		else if (idx == SLAVE_CHARGER)
			pdata = &info->chg2_data;
		else if (idx == MAIN_DIVIDER_CHARGER)
			pdata = &info->dvchg1_data;
		else if (idx == SLAVE_DIVIDER_CHARGER)
			pdata = &info->dvchg2_data;
		else
			return -ENOTSUPP;

		*tchg_min = pdata->junction_temp_min;
		*tchg_max = pdata->junction_temp_max;

		return 0;
	}
	return -EBUSY;
}

int charger_manager_force_charging_current(struct charger_consumer *consumer,
	int idx, int charging_current)
{
	struct charger_manager *info = consumer->cm;

	if (info != NULL) {
		struct charger_data *pdata = NULL;

		if (idx == MAIN_CHARGER)
			pdata = &info->chg1_data;
		else if (idx == SLAVE_CHARGER)
			pdata = &info->chg2_data;
		else
			return -ENOTSUPP;

		pdata->force_charging_current = charging_current;
		#ifdef MTK_BASE
		_mtk_charger_change_current_setting(info);
		#endif
		_wake_up_charger(info);
		return 0;
	}
	return -EBUSY;
}

void adaptive_charging_disable_ichg(bool on)
{
	struct charger_manager *info = pinfo;

	if (info == NULL)
		return;

	info->mmi.adaptive_charging_disable_ichg = !!on;
	pr_info("%s: adaptive charging disable ichg %d\n", __func__,
		info->mmi.adaptive_charging_disable_ichg);
	_wake_up_charger(info);
}
EXPORT_SYMBOL(adaptive_charging_disable_ichg);

void adaptive_charging_disable_ibat(bool on)
{
	struct charger_manager *info = pinfo;

	if (info == NULL)
		return;

	info->mmi.adaptive_charging_disable_ibat = !!on;
	pr_info("%s: adaptive charging disable ibat %d\n", __func__,
		info->mmi.adaptive_charging_disable_ibat);
	_wake_up_charger(info);
}
EXPORT_SYMBOL(adaptive_charging_disable_ibat);

void wlc_control_pin_set(bool on)
{
	struct charger_manager *info = pinfo;

	if (info == NULL)
		return;

	if(gpio_is_valid(info->mmi.wls_control_en)) {
		gpio_set_value(info->mmi.wls_control_en, on);
	}

	pr_info("%s value:%d\n", __func__,on);
}
EXPORT_SYMBOL(wlc_control_pin_set);

int charger_manager_get_current_charging_type(struct charger_consumer *consumer)
{
	struct charger_manager *info = consumer->cm;

	if (info != NULL) {
		if (mtk_pe20_get_is_connect(info) ||wlc_get_online())
			return 2;
	}

	return 0;
}

int charger_manager_get_zcv(struct charger_consumer *consumer, int idx, u32 *uV)
{
	struct charger_manager *info = consumer->cm;
	int ret = 0;
	struct charger_device *pchg = NULL;


	if (info != NULL) {
		if (idx == MAIN_CHARGER) {
			pchg = info->chg1_dev;
			ret = charger_dev_get_zcv(pchg, uV);
		} else if (idx == SLAVE_CHARGER) {
			pchg = info->chg2_dev;
			ret = charger_dev_get_zcv(pchg, uV);
		} else
			ret = -1;

	} else {
		chr_err("%s info is null\n", __func__);
	}
	chr_err("%s zcv:%d ret:%d\n", __func__, *uV, ret);

	return 0;
}

int charger_manager_enable_sc(struct charger_consumer *consumer,
	bool en, int stime, int etime)
{
	struct charger_manager *info = consumer->cm;

	chr_err("%s en:%d %d %d\n", __func__,
		en, stime, etime);
	info->sc.start_time = stime;
	info->sc.end_time = etime;
	info->sc.enable = en;
	_wake_up_charger(info);
	return 0;
}

int charger_manager_set_sc_current_limit(struct charger_consumer *consumer,
	int cl)
{
	struct charger_manager *info = consumer->cm;

	chr_err("%s %d\n", __func__,
		cl);
	info->sc.current_limit = cl;
	_wake_up_charger(info);
	return 0;
}

int charger_manager_set_bh(struct charger_consumer *consumer,
	int bh)
{
	struct charger_manager *info = consumer->cm;

	chr_err("%s %d\n", __func__,
		bh);
	info->sc.bh = bh;
	_wake_up_charger(info);
	return 0;
}

int charger_manager_enable_chg_type_det(struct charger_consumer *consumer,
	bool en)
{
	struct charger_manager *info = consumer->cm;
	struct charger_device *chg_dev = NULL;
	int ret = 0;

	if (info != NULL) {
		switch (info->data.bc12_charger) {
		case MAIN_CHARGER:
			chg_dev = info->chg1_dev;
			break;
		case SLAVE_CHARGER:
			chg_dev = info->chg2_dev;
			break;
		default:
			chg_dev = info->chg1_dev;
			chr_err("%s: invalid number, use main charger as default\n",
				__func__);
			break;
		}

		chr_err("%s: chg%d is doing bc12\n", __func__,
			info->data.bc12_charger + 1);
		ret = charger_dev_enable_chg_type_det(chg_dev, en);
		if (ret < 0) {
			chr_err("%s: en chgdet fail, en = %d\n", __func__, en);
			return ret;
		}
	} else
		chr_err("%s: charger_manager is null\n", __func__);



	return 0;
}

int charger_manager_cp_set_ichg(
	struct charger_consumer *consumer, int idx, int charging_current)
{
	struct charger_manager *info = consumer->cm;

	if (info != NULL) {
		struct charger_data *pdata;

		if (idx == MAIN_CHARGER)
			pdata = &info->chg1_data;
		else if (idx == SLAVE_CHARGER)
			pdata = &info->chg2_data;
		else
			return -ENOTSUPP;

		pdata->cp_ichg_limit= charging_current;
		chr_err("%s: dev:%s idx:%d en:%d\n", __func__,
			dev_name(consumer->dev), idx, charging_current);
		_mtk_charger_change_current_setting(info);
		_wake_up_charger(info);
		return 0;
	}
	return -EBUSY;
}

int charger_manager_is_enabled(struct charger_consumer *consumer,
	int idx, bool *en)
{
	int ret = 0;
	struct charger_manager *info = consumer->cm;
	struct charger_device *chg_dev = NULL;


	if (!info)
		return -EINVAL;

	switch (idx) {
	case MAIN_CHARGER:
		chg_dev = info->chg1_dev;
		break;
	case SLAVE_CHARGER:
		chg_dev = info->chg2_dev;
		break;
	default:
		return -EINVAL;
	}

	ret = charger_dev_is_enabled(chg_dev, en);
	if (ret < 0)
		chr_err("%s: charger_dev_is_enabled failed\n", __func__);


	return ret;
}

int charger_manager_get_ibus(struct charger_consumer *consumer,
	int idx, u32 *ibus)
{
	int ret = 0;
	struct charger_manager *info = consumer->cm;
	struct charger_device *chg_dev = NULL;


	if (!info)
		return -EINVAL;

	switch (idx) {
	case MAIN_CHARGER:
		chg_dev = info->chg1_dev;
		break;
	case SLAVE_CHARGER:
		chg_dev = info->chg2_dev;
		break;
	default:
		return -EINVAL;
	}

	ret = charger_dev_get_ibus(chg_dev, ibus);
	if (ret < 0)
		chr_err("%s: charger_manager_get_ibus failed\n", __func__);


	return ret;
}

int register_charger_manager_notifier(struct charger_consumer *consumer,
	struct notifier_block *nb)
{
	int ret = 0;
	struct charger_manager *info = consumer->cm;


	mutex_lock(&consumer_mutex);
	if (info != NULL)
		ret = srcu_notifier_chain_register(&info->evt_nh, nb);
	else
		consumer->pnb = nb;
	mutex_unlock(&consumer_mutex);

	return ret;
}

int unregister_charger_manager_notifier(struct charger_consumer *consumer,
				struct notifier_block *nb)
{
	int ret = 0;
	struct charger_manager *info = consumer->cm;

	mutex_lock(&consumer_mutex);
	if (info != NULL)
		ret = srcu_notifier_chain_unregister(&info->evt_nh, nb);
	else
		consumer->pnb = NULL;
	mutex_unlock(&consumer_mutex);

	return ret;
}

/* user interface end*/

/* factory mode */
#define CHARGER_DEVNAME "charger_ftm"
#define GET_IS_SLAVE_CHARGER_EXIST _IOW('k', 13, int)

static struct class *charger_class;
static struct cdev *charger_cdev;
static int charger_major;
static dev_t charger_devno;

static int is_slave_charger_exist(void)
{
	if (get_charger_by_name("secondary_chg") == NULL)
		return 0;
	return 1;
}

static long charger_ftm_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	int ret = 0;
	int out_data = 0;
	void __user *user_data = (void __user *)arg;

	switch (cmd) {
	case GET_IS_SLAVE_CHARGER_EXIST:
		out_data = is_slave_charger_exist();
		ret = copy_to_user(user_data, &out_data, sizeof(out_data));
		chr_err("[%s] SLAVE_CHARGER_EXIST: %d\n", __func__, out_data);
		break;
	default:
		chr_err("[%s] Error ID\n", __func__);
		break;
	}

	return ret;
}
#ifdef CONFIG_COMPAT
static long charger_ftm_compat_ioctl(struct file *file,
			unsigned int cmd, unsigned long arg)
{
	int ret = 0;

	switch (cmd) {
	case GET_IS_SLAVE_CHARGER_EXIST:
		ret = file->f_op->unlocked_ioctl(file, cmd, arg);
		break;
	default:
		chr_err("[%s] Error ID\n", __func__);
		break;
	}

	return ret;
}
#endif
static int charger_ftm_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int charger_ftm_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations charger_ftm_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = charger_ftm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = charger_ftm_compat_ioctl,
#endif
	.open = charger_ftm_open,
	.release = charger_ftm_release,
};

void charger_ftm_init(void)
{
	struct class_device *class_dev = NULL;
	int ret = 0;

	ret = alloc_chrdev_region(&charger_devno, 0, 1, CHARGER_DEVNAME);
	if (ret < 0) {
		chr_err("[%s]Can't get major num for charger_ftm\n", __func__);
		return;
	}

	charger_cdev = cdev_alloc();
	if (!charger_cdev) {
		chr_err("[%s]cdev_alloc fail\n", __func__);
		goto unregister;
	}
	charger_cdev->owner = THIS_MODULE;
	charger_cdev->ops = &charger_ftm_fops;

	ret = cdev_add(charger_cdev, charger_devno, 1);
	if (ret < 0) {
		chr_err("[%s] cdev_add failed\n", __func__);
		goto free_cdev;
	}

	charger_major = MAJOR(charger_devno);
	charger_class = class_create(THIS_MODULE, CHARGER_DEVNAME);
	if (IS_ERR(charger_class)) {
		chr_err("[%s] class_create failed\n", __func__);
		goto free_cdev;
	}

	class_dev = (struct class_device *)device_create(charger_class,
				NULL, charger_devno, NULL, CHARGER_DEVNAME);
	if (IS_ERR(class_dev)) {
		chr_err("[%s] device_create failed\n", __func__);
		goto free_class;
	}

	pr_debug("%s done\n", __func__);
	return;

free_class:
	class_destroy(charger_class);
free_cdev:
	cdev_del(charger_cdev);
unregister:
	unregister_chrdev_region(charger_devno, 1);
}
/* factory mode end */

void mtk_charger_get_atm_mode(struct charger_manager *info)
{
	char atm_str[64] = {0};
	char *ptr = NULL, *ptr_e = NULL;
	char keyword[] = "androidboot.atm=";
	int size = 0;

	info->atm_enabled = false;

	ptr = strstr(saved_command_line, keyword);
	if (ptr != 0) {
		ptr_e = strstr(ptr, " ");
		if (ptr_e == NULL)
			goto end;

		size = ptr_e - (ptr + strlen(keyword));
		if (size <= 0)
			goto end;
		strncpy(atm_str, ptr + strlen(keyword), size);
		atm_str[size] = '\0';

		if (!strncmp(atm_str, "enable", strlen("enable")))
			info->atm_enabled = true;
	}
end:
	pr_info("%s: atm_enabled = %d\n", __func__, info->atm_enabled);
}

/* internal algorithm common function */
bool is_dual_charger_supported(struct charger_manager *info)
{
	if (info->chg2_dev == NULL)
		return false;
	return true;
}

int charger_enable_vbus_ovp(struct charger_manager *pinfo, bool enable)
{
	int ret = 0;
	u32 sw_ovp = 0;

	if (enable)
		sw_ovp = pinfo->data.max_charger_voltage_setting;
	else
		sw_ovp = 15000000;

	/* Enable/Disable SW OVP status */
	pinfo->data.max_charger_voltage = sw_ovp;

	chr_err("[%s] en:%d ovp:%d\n",
			    __func__, enable, sw_ovp);
	return ret;
}

bool is_typec_adapter(struct charger_manager *info)
{
	int rp;

	if (info == NULL || info->pd_adapter == NULL)
		return false;

	rp = adapter_dev_get_property(info->pd_adapter, TYPEC_RP_LEVEL);
	if (rp > 500 &&
			mtk_pe20_get_is_connect(info) == false &&
			mtk_pe_get_is_connect(info) == false &&
			info->enable_type_c == true&&
			wlc_get_online() == false &&
			info->chr_type != STANDARD_HOST &&
                        info->chr_type != CHARGING_HOST)
		return true;

	return false;
}

int charger_get_vbus(void)
{
	int ret = 0;
	int vchr = 0;

	if (pinfo == NULL)
		return 0;
	ret = charger_dev_get_vbus(pinfo->chg1_dev, &vchr);
	if (ret < 0) {
		chr_err("%s: get vbus failed: %d\n", __func__, ret);
		return ret;
	}

	vchr = vchr / 1000;
	return vchr;
}

/* internal algorithm common function end */

/* sw jeita */
void sw_jeita_state_machine_init(struct charger_manager *info)
{
	struct sw_jeita_data *sw_jeita;

	if (info->enable_sw_jeita == true) {
		sw_jeita = &info->sw_jeita;
		info->battery_temp = battery_get_bat_temperature();

		if (info->battery_temp >= info->data.temp_t4_thres)
			sw_jeita->sm = TEMP_ABOVE_T4;
		else if (info->battery_temp > info->data.temp_t3_thres)
			sw_jeita->sm = TEMP_T3_TO_T4;
		else if (info->battery_temp >= info->data.temp_t2_thres)
			sw_jeita->sm = TEMP_T2_TO_T3;
		else if (info->battery_temp >= info->data.temp_t1_thres)
			sw_jeita->sm = TEMP_T1_TO_T2;
		else if (info->battery_temp >= info->data.temp_t0_thres)
			sw_jeita->sm = TEMP_T0_TO_T1;
		else
			sw_jeita->sm = TEMP_BELOW_T0;

		chr_err("[SW_JEITA] tmp:%d sm:%d\n",
			info->battery_temp, sw_jeita->sm);
	}
}

void do_sw_jeita_state_machine(struct charger_manager *info)
{
	struct sw_jeita_data *sw_jeita;

	sw_jeita = &info->sw_jeita;
	sw_jeita->pre_sm = sw_jeita->sm;
	sw_jeita->charging = true;

	/* JEITA battery temp Standard */
	if (info->battery_temp >= info->data.temp_t4_thres) {
		chr_err("[SW_JEITA] Battery Over high Temperature(%d) !!\n",
			info->data.temp_t4_thres);

		sw_jeita->sm = TEMP_ABOVE_T4;
		sw_jeita->charging = false;
	} else if (info->battery_temp > info->data.temp_t3_thres) {
		/* control 45 degree to normal behavior */
		if ((sw_jeita->sm == TEMP_ABOVE_T4)
		    && (info->battery_temp
			>= info->data.temp_t4_thres_minus_x_degree)) {
			chr_err("[SW_JEITA] Battery Temperature between %d and %d,not allow charging yet!!\n",
				info->data.temp_t4_thres_minus_x_degree,
				info->data.temp_t4_thres);

			sw_jeita->charging = false;
		} else {
			chr_err("[SW_JEITA] Battery Temperature between %d and %d !!\n",
				info->data.temp_t3_thres,
				info->data.temp_t4_thres);

			sw_jeita->sm = TEMP_T3_TO_T4;
		}
	} else if (info->battery_temp >= info->data.temp_t2_thres) {
		if (((sw_jeita->sm == TEMP_T3_TO_T4)
		     && (info->battery_temp
			 >= info->data.temp_t3_thres_minus_x_degree))
		    || ((sw_jeita->sm == TEMP_T1_TO_T2)
			&& (info->battery_temp
			    <= info->data.temp_t2_thres_plus_x_degree))) {
			chr_err("[SW_JEITA] Battery Temperature not recovery to normal temperature charging mode yet!!\n");
		} else {
			chr_err("[SW_JEITA] Battery Normal Temperature between %d and %d !!\n",
				info->data.temp_t2_thres,
				info->data.temp_t3_thres);
			sw_jeita->sm = TEMP_T2_TO_T3;
		}
	} else if (info->battery_temp >= info->data.temp_t1_thres) {
		if ((sw_jeita->sm == TEMP_T0_TO_T1
		     || sw_jeita->sm == TEMP_BELOW_T0)
		    && (info->battery_temp
			<= info->data.temp_t1_thres_plus_x_degree)) {
			if (sw_jeita->sm == TEMP_T0_TO_T1) {
				chr_err("[SW_JEITA] Battery Temperature between %d and %d !!\n",
					info->data.temp_t1_thres_plus_x_degree,
					info->data.temp_t2_thres);
			}
			if (sw_jeita->sm == TEMP_BELOW_T0) {
				chr_err("[SW_JEITA] Battery Temperature between %d and %d,not allow charging yet!!\n",
					info->data.temp_t1_thres,
					info->data.temp_t1_thres_plus_x_degree);
				sw_jeita->charging = false;
			}
		} else {
			chr_err("[SW_JEITA] Battery Temperature between %d and %d !!\n",
				info->data.temp_t1_thres,
				info->data.temp_t2_thres);

			sw_jeita->sm = TEMP_T1_TO_T2;
		}
	} else if (info->battery_temp >= info->data.temp_t0_thres) {
		if ((sw_jeita->sm == TEMP_BELOW_T0)
		    && (info->battery_temp
			<= info->data.temp_t0_thres_plus_x_degree)) {
			chr_err("[SW_JEITA] Battery Temperature between %d and %d,not allow charging yet!!\n",
				info->data.temp_t0_thres,
				info->data.temp_t0_thres_plus_x_degree);

			sw_jeita->charging = false;
		} else {
			chr_err("[SW_JEITA] Battery Temperature between %d and %d !!\n",
				info->data.temp_t0_thres,
				info->data.temp_t1_thres);

			sw_jeita->sm = TEMP_T0_TO_T1;
		}
	} else {
		chr_err("[SW_JEITA] Battery below low Temperature(%d) !!\n",
			info->data.temp_t0_thres);
		sw_jeita->sm = TEMP_BELOW_T0;
		sw_jeita->charging = false;
	}

	/* set CV after temperature changed */
	/* In normal range, we adjust CV dynamically */
	if (sw_jeita->sm != TEMP_T2_TO_T3) {
		if (sw_jeita->sm == TEMP_ABOVE_T4)
			sw_jeita->cv = info->data.jeita_temp_above_t4_cv;
		else if (sw_jeita->sm == TEMP_T3_TO_T4)
			sw_jeita->cv = info->data.jeita_temp_t3_to_t4_cv;
		else if (sw_jeita->sm == TEMP_T2_TO_T3)
			sw_jeita->cv = 0;
		else if (sw_jeita->sm == TEMP_T1_TO_T2)
			sw_jeita->cv = info->data.jeita_temp_t1_to_t2_cv;
		else if (sw_jeita->sm == TEMP_T0_TO_T1)
			sw_jeita->cv = info->data.jeita_temp_t0_to_t1_cv;
		else if (sw_jeita->sm == TEMP_BELOW_T0)
			sw_jeita->cv = info->data.jeita_temp_below_t0_cv;
		else
			sw_jeita->cv = info->data.battery_cv;
	} else {
		sw_jeita->cv = 0;
	}

	chr_err("[SW_JEITA]preState:%d newState:%d tmp:%d cv:%d\n",
		sw_jeita->pre_sm, sw_jeita->sm, info->battery_temp,
		sw_jeita->cv);
}

static ssize_t show_sw_jeita(struct device *dev, struct device_attribute *attr,
					       char *buf)
{
	struct charger_manager *pinfo = dev->driver_data;

	chr_err("%s: %d\n", __func__, pinfo->enable_sw_jeita);
	return sprintf(buf, "%d\n", pinfo->enable_sw_jeita);
}

static ssize_t store_sw_jeita(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t size)
{
	struct charger_manager *pinfo = dev->driver_data;
	signed int temp;

	if (kstrtoint(buf, 10, &temp) == 0) {
		if (temp == 0)
			pinfo->enable_sw_jeita = false;
		else {
			pinfo->enable_sw_jeita = true;
			sw_jeita_state_machine_init(pinfo);
		}

	} else {
		chr_err("%s: format error!\n", __func__);
	}
	return size;
}

static DEVICE_ATTR(sw_jeita, 0644, show_sw_jeita,
		   store_sw_jeita);
/* sw jeita end*/

/* pump express series */
bool mtk_is_pep_series_connect(struct charger_manager *info)
{
	if (mtk_pe20_get_is_connect(info) || mtk_pe_get_is_connect(info) ||wlc_get_online() )
		return true;

	return false;
}

static ssize_t show_pe20(struct device *dev, struct device_attribute *attr,
					       char *buf)
{
	struct charger_manager *pinfo = dev->driver_data;

	chr_err("%s: %d\n", __func__, pinfo->enable_pe_2);
	return sprintf(buf, "%d\n", pinfo->enable_pe_2);
}

static ssize_t store_pe20(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t size)
{
	struct charger_manager *pinfo = dev->driver_data;
	signed int temp;

	if (kstrtoint(buf, 10, &temp) == 0) {
		if (temp == 0)
			pinfo->enable_pe_2 = false;
		else
			pinfo->enable_pe_2 = true;

	} else {
		chr_err("%s: format error!\n", __func__);
	}
	return size;
}

static DEVICE_ATTR(pe20, 0644, show_pe20, store_pe20);

static ssize_t show_pe40(struct device *dev, struct device_attribute *attr,
					       char *buf)
{
	struct charger_manager *pinfo = dev->driver_data;

	chr_err("%s: %d\n", __func__, pinfo->enable_pe_4);
	return sprintf(buf, "%d\n", pinfo->enable_pe_4);
}

static ssize_t store_pe40(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t size)
{
	struct charger_manager *pinfo = dev->driver_data;
	signed int temp;

	if (kstrtoint(buf, 10, &temp) == 0) {
		if (temp == 0)
			pinfo->enable_pe_4 = false;
		else
			pinfo->enable_pe_4 = true;

	} else {
		chr_err("%s: format error!\n", __func__);
	}
	return size;
}

static DEVICE_ATTR(pe40, 0644, show_pe40, store_pe40);

/* pump express series end*/

static ssize_t show_charger_log_level(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	chr_err("%s: %d\n", __func__, chargerlog_level);
	return sprintf(buf, "%d\n", chargerlog_level);
}

static ssize_t store_charger_log_level(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned long val = 0;
	int ret = 0;

	chr_err("%s\n", __func__);

	if (buf != NULL && size != 0) {
		chr_err("%s: buf is %s\n", __func__, buf);
		ret = kstrtoul(buf, 10, &val);
		if (ret < 0) {
			chr_err("%s: kstrtoul fail, ret = %d\n", __func__, ret);
			return ret;
		}
		if (val < 0) {
			chr_err("%s: val is inavlid: %ld\n", __func__, val);
			val = 0;
		}
		chargerlog_level = val;
		chr_err("%s: log_level=%d\n", __func__, chargerlog_level);
	}
	return size;
}
static DEVICE_ATTR(charger_log_level, 0644, show_charger_log_level,
		store_charger_log_level);

static ssize_t show_pdc_max_watt_level(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct charger_manager *pinfo = dev->driver_data;

	return sprintf(buf, "%d\n", mtk_pdc_get_max_watt(pinfo));
}

static ssize_t store_pdc_max_watt_level(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct charger_manager *pinfo = dev->driver_data;
	int temp;

	if (kstrtoint(buf, 10, &temp) == 0) {
		mtk_pdc_set_max_watt(pinfo, temp);
		chr_err("[store_pdc_max_watt]:%d\n", temp);
	} else
		chr_err("[store_pdc_max_watt]: format error!\n");

	return size;
}
static DEVICE_ATTR(pdc_max_watt, 0644, show_pdc_max_watt_level,
		store_pdc_max_watt_level);

int mtk_get_dynamic_cv(struct charger_manager *info, unsigned int *cv)
{
	int ret = 0;
	u32 _cv, _cv_temp;
	unsigned int vbat_threshold[4] = {3400000, 0, 0, 0};
	u32 vbat_bif = 0, vbat_auxadc = 0, vbat = 0;
	u32 retry_cnt = 0;

	if (pmic_is_bif_exist()) {
		do {
			vbat_auxadc = battery_get_bat_voltage() * 1000;
			ret = pmic_get_bif_battery_voltage(&vbat_bif);
			vbat_bif = vbat_bif * 1000;
			if (ret >= 0 && vbat_bif != 0 &&
			    vbat_bif < vbat_auxadc) {
				vbat = vbat_bif;
				chr_err("%s: use BIF vbat = %duV, dV to auxadc = %duV\n",
					__func__, vbat, vbat_auxadc - vbat_bif);
				break;
			}
			retry_cnt++;
		} while (retry_cnt < 5);

		if (retry_cnt == 5) {
			ret = 0;
			vbat = vbat_auxadc;
			chr_err("%s: use AUXADC vbat = %duV, since BIF vbat = %duV\n",
				__func__, vbat_auxadc, vbat_bif);
		}

		/* Adjust CV according to the obtained vbat */
		vbat_threshold[1] = info->data.bif_threshold1;
		vbat_threshold[2] = info->data.bif_threshold2;
		_cv_temp = info->data.bif_cv_under_threshold2;

		if (!info->enable_dynamic_cv && vbat >= vbat_threshold[2]) {
			_cv = info->data.battery_cv;
			goto out;
		}

		if (vbat < vbat_threshold[1])
			_cv = 4608000;
		else if (vbat >= vbat_threshold[1] && vbat < vbat_threshold[2])
			_cv = _cv_temp;
		else {
			_cv = info->data.battery_cv;
			info->enable_dynamic_cv = false;
		}
out:
		*cv = _cv;
		chr_err("%s: CV = %duV, enable_dynamic_cv = %d\n",
			__func__, _cv, info->enable_dynamic_cv);
	} else
		ret = -ENOTSUPP;

	return ret;
}

int charger_manager_notifier(struct charger_manager *info, int event)
{
	return srcu_notifier_call_chain(&info->evt_nh, event, NULL);
}
extern int wireless_get_charger_type(void);
int charger_psy_event(struct notifier_block *nb, unsigned long event, void *v)
{
	struct charger_manager *info =
			container_of(nb, struct charger_manager, psy_nb);
	struct power_supply *psy = v;
	union power_supply_propval val;
	int ret;
	int tmp = 0;

	if (strcmp(psy->desc->name, "battery") == 0) {
		ret = power_supply_get_property(psy,
				POWER_SUPPLY_PROP_TEMP, &val);
		if (!ret) {
			tmp = val.intval / 10;
			if (info->battery_temp != tmp
			    && mt_get_charger_type() != CHARGER_UNKNOWN) {
				_wake_up_charger(info);
				chr_err("%s: %ld %s tmp:%d %d chr:%d\n",
					__func__, event, psy->desc->name, tmp,
					info->battery_temp,
					mt_get_charger_type());
			}
		}
	}

	if (strcmp(psy->desc->name, "wireless") == 0) {
		ret = power_supply_get_property(psy,
				POWER_SUPPLY_PROP_ONLINE, &val);
		if (!ret) {
			tmp = val.intval ;
			if (info->wireless_online != tmp ) {
				info->wireless_online = tmp;
				if(info->wireless_online == true) {
					charger_manager_force_disable_power_path(info->chg1_consumer, MAIN_CHARGER, false);
					charger_manager_enable_power_path(info->chg1_consumer, MAIN_CHARGER, info->wireless_online == 1 ? true:false);
					if(info->mmi.factory_mode) {
						chr_err("%s:wireless factory input current:%d\n",
							__func__, info->data.wireless_factory_max_input_current);
						charger_dev_set_input_current(info->chg1_dev, info->data.wireless_factory_max_input_current);
					}
					wireless_get_charger_type();
					_wake_up_charger(info);
				}else {
					if(info->mmi.factory_mode) {
						_wake_up_charger(info);
					}
				}
				chr_err("%s: %ld %s tmp:%d %d chr:%d\n",
					__func__, event, psy->desc->name, tmp,
					info->wireless_online,
					mt_get_charger_type());
			}
		}
	}

	return NOTIFY_DONE;
}

void mtk_charger_int_handler(void)
{
	chr_err("%s\n", __func__);

	if (pinfo == NULL) {
		chr_err("charger is not rdy ,skip1\n");
		return;
	}

	if (pinfo->init_done != true) {
		chr_err("charger is not rdy ,skip2\n");
		return;
	}

	if (mt_get_charger_type() == CHARGER_UNKNOWN) {
		mutex_lock(&pinfo->cable_out_lock);
		pinfo->cable_out_cnt++;
		chr_err("cable_out_cnt=%d\n", pinfo->cable_out_cnt);
		mutex_unlock(&pinfo->cable_out_lock);
		charger_manager_notifier(pinfo, CHARGER_NOTIFY_STOP_CHARGING);
	} else
		charger_manager_notifier(pinfo, CHARGER_NOTIFY_START_CHARGING);

	chr_err("wake_up_charger\n");
	_wake_up_charger(pinfo);
}

#ifdef CONFIG_MOTO_CHG_FFC_5V10W_SUPPORT
static void mmi_charger_ffc_init(struct charger_manager *info)
{
	struct mmi_params *mmi = &info->mmi;

	mmi->ffc_state = CHARGER_FFC_STATE_INITIAL;
	mmi->ffc_entry_threshold = 2000000;
	mmi->ffc_exit_threshold =  1800000;
	mmi->ffc_ibat_windowsum = 0;
	mmi->ffc_ibat_count = 0;
	mmi->ffc_ibat_windowsize = 6;
	mmi->ffc_iavg = 0;
	mmi->ffc_iavg_update_timestamp = 0;
	mmi->ffc_uisoc_threshold = 75;

	info->ffc_discharging = false;
	info->ffc_max_fv_mv_backup = 4510;
	info->data.ac_charger_input_current = info->ffc_input_current_backup;
	pr_debug("[%s] ffc initilaize...\n", __func__);
}
#endif

static int mtk_charger_plug_in(struct charger_manager *info,
				enum charger_type chr_type)
{
	info->chr_type = chr_type;
	info->charger_thread_polling = true;

	info->can_charging = true;
	info->enable_dynamic_cv = true;
	info->safety_timeout = false;
	info->vbusov_stat = false;

	chr_err("mtk_is_charger_on plug in, type:%d\n", chr_type);
	if (info->plug_in != NULL)
		info->plug_in(info);

	memset(&pinfo->sc.data, 0, sizeof(struct scd_cmd_param_t_1));
	pinfo->sc.disable_in_this_plug = false;
	wakeup_sc_algo_cmd(&pinfo->sc.data, SC_EVENT_PLUG_IN, 0);
	if(info->wireless_online == true && info->mmi.factory_mode) {
			chr_err("%s:wireless factory input current:%d\n",
				__func__, info->data.wireless_factory_max_input_current);
			charger_dev_set_input_current(info->chg1_dev, info->data.wireless_factory_max_input_current);
	}else {
		charger_dev_set_input_current(info->chg1_dev,
				info->chg1_data.input_current_limit);
	}
	charger_dev_plug_in(info->chg1_dev);
	return 0;
}

static int mtk_charger_plug_out(struct charger_manager *info)
{
	struct charger_data *pdata1 = &info->chg1_data;
	struct charger_data *pdata2 = &info->chg2_data;

	chr_err("%s\n", __func__);
	info->chr_type = CHARGER_UNKNOWN;
	info->charger_thread_polling = false;
	info->pd_reset = false;

	pdata1->disable_charging_count = 0;
	pdata1->input_current_limit_by_aicl = -1;
	pdata2->disable_charging_count = 0;

	if (info->plug_out != NULL)
		info->plug_out(info);

#ifdef CONFIG_MOTO_CHG_FFC_5V10W_SUPPORT
	mmi_charger_ffc_init(info);
#endif
	memset(&pinfo->sc.data, 0, sizeof(struct scd_cmd_param_t_1));
	wakeup_sc_algo_cmd(&pinfo->sc.data, SC_EVENT_PLUG_OUT, 0);
	pdata1->input_current_limit = 500000;
	charger_dev_set_input_current(info->chg1_dev, 500000);
	charger_dev_set_mivr(info->chg1_dev, info->data.min_charger_voltage);
	charger_dev_plug_out(info->chg1_dev);
	chr_err("lenovo mtk_charger_plug_out, atm_mode=%s\n", atm_mode);
	if (!strcmp(atm_mode, "enable") && !info->chg_tcmd_client.factory_kill_disable) {
		if(info->wireless_online == true)
			msleep(3000);
		else
			msleep(1000);
		chr_err("lenovo mtk_charger_plug_out, wait 1000ms, vbus:%d\n",charger_get_vbus());
		if(charger_get_vbus() < 3500)
			orderly_poweroff(true);
	}
	return 0;
}

static bool mtk_is_charger_on(struct charger_manager *info)
{
	enum charger_type chr_type;

	chr_type = mt_get_charger_type();
	if (chr_type == CHARGER_UNKNOWN) {
		if (info->chr_type != CHARGER_UNKNOWN) {
			mtk_charger_plug_out(info);
			mutex_lock(&info->cable_out_lock);
			info->cable_out_cnt = 0;
			mutex_unlock(&info->cable_out_lock);
		}
	} else {
		if (info->chr_type == CHARGER_UNKNOWN)
			mtk_charger_plug_in(info, chr_type);
		else
			info->chr_type = chr_type;

		if (info->cable_out_cnt > 0) {
			mtk_charger_plug_out(info);
			mtk_charger_plug_in(info, chr_type);
			mutex_lock(&info->cable_out_lock);
			info->cable_out_cnt--;
			mutex_unlock(&info->cable_out_lock);
		}
	}

	if (chr_type == CHARGER_UNKNOWN)
		return false;

	return true;
}

static void charger_update_data(struct charger_manager *info)
{
	info->battery_temp = battery_get_bat_temperature();
}

static int mtk_chgstat_notify(struct charger_manager *info)
{
	int ret = 0;
	char *env[2] = { "CHGSTAT=1", NULL };

	chr_err("%s: 0x%x\n", __func__, info->notify_code);
	ret = kobject_uevent_env(&info->pdev->dev.kobj, KOBJ_CHANGE, env);
	if (ret)
		chr_err("%s: kobject_uevent_fail, ret=%d", __func__, ret);

	return ret;
}

/* return false if vbus is over max_charger_voltage */
static bool mtk_chg_check_vbus(struct charger_manager *info)
{
	int vchr = 0;

	vchr = battery_get_vbus() * 1000; /* uV */
	if (vchr > info->data.max_charger_voltage) {
		chr_err("%s: vbus(%d mV) > %d mV\n", __func__, vchr / 1000,
			info->data.max_charger_voltage / 1000);
		return false;
	}

	return true;
}

static void mtk_battery_notify_VCharger_check(struct charger_manager *info)
{
#if defined(BATTERY_NOTIFY_CASE_0001_VCHARGER)
	int vchr = 0;

	vchr = battery_get_vbus() * 1000; /* uV */
	if (vchr < info->data.max_charger_voltage)
		info->notify_code &= ~CHG_VBUS_OV_STATUS;
	else {
		info->notify_code |= CHG_VBUS_OV_STATUS;
		chr_err("[BATTERY] charger_vol(%d mV) > %d mV\n",
			vchr / 1000, info->data.max_charger_voltage / 1000);
		mtk_chgstat_notify(info);
	}
#endif
}

static void mtk_battery_notify_VBatTemp_check(struct charger_manager *info)
{
#if defined(BATTERY_NOTIFY_CASE_0002_VBATTEMP)
	if (info->battery_temp >= info->thermal.max_charge_temp) {
		info->notify_code |= CHG_BAT_OT_STATUS;
		chr_err("[BATTERY] bat_temp(%d) out of range(too high)\n",
			info->battery_temp);
		mtk_chgstat_notify(info);
	} else {
		info->notify_code &= ~CHG_BAT_OT_STATUS;
	}

	if (info->enable_sw_jeita == true) {
		if (info->battery_temp < info->data.temp_neg_10_thres) {
			info->notify_code |= CHG_BAT_LT_STATUS;
			chr_err("bat_temp(%d) out of range(too low)\n",
				info->battery_temp);
			mtk_chgstat_notify(info);
		} else {
			info->notify_code &= ~CHG_BAT_LT_STATUS;
		}
	} else {
#ifdef BAT_LOW_TEMP_PROTECT_ENABLE
		if (info->battery_temp < info->thermal.min_charge_temp) {
			info->notify_code |= CHG_BAT_LT_STATUS;
			chr_err("bat_temp(%d) out of range(too low)\n",
				info->battery_temp);
			mtk_chgstat_notify(info);
		} else {
			info->notify_code &= ~CHG_BAT_LT_STATUS;
		}
#endif
	}
#endif
}

static void mtk_battery_notify_UI_test(struct charger_manager *info)
{
	switch (info->notify_test_mode) {
	case 1:
		info->notify_code = CHG_VBUS_OV_STATUS;
		pr_debug("[%s] CASE_0001_VCHARGER\n", __func__);
		break;
	case 2:
		info->notify_code = CHG_BAT_OT_STATUS;
		pr_debug("[%s] CASE_0002_VBATTEMP\n", __func__);
		break;
	case 3:
		info->notify_code = CHG_OC_STATUS;
		pr_debug("[%s] CASE_0003_ICHARGING\n", __func__);
		break;
	case 4:
		info->notify_code = CHG_BAT_OV_STATUS;
		pr_debug("[%s] CASE_0004_VBAT\n", __func__);
		break;
	case 5:
		info->notify_code = CHG_ST_TMO_STATUS;
		pr_debug("[%s] CASE_0005_TOTAL_CHARGINGTIME\n", __func__);
		break;
	case 6:
		info->notify_code = CHG_BAT_LT_STATUS;
		pr_debug("[%s] CASE6: VBATTEMP_LOW\n", __func__);
		break;
	case 7:
		info->notify_code = CHG_TYPEC_WD_STATUS;
		pr_debug("[%s] CASE7: Moisture Detection\n", __func__);
		break;
	default:
		pr_debug("[%s] Unknown BN_TestMode Code: %x\n",
			__func__, info->notify_test_mode);
	}
	mtk_chgstat_notify(info);
}

static void mtk_battery_notify_check(struct charger_manager *info)
{
	if (info->notify_test_mode == 0x0000) {
		mtk_battery_notify_VCharger_check(info);
		mtk_battery_notify_VBatTemp_check(info);
	} else {
		mtk_battery_notify_UI_test(info);
	}
}

static void check_battery_exist(struct charger_manager *info)
{
	struct device *dev = NULL;
	struct device_node *boot_node = NULL;
	struct tag_bootmode *tag = NULL;
	unsigned int i = 0;
	int count = 0;
	int boot_mode = 11;//UNKNOWN_BOOT
// workaround for mt6768 
	//int boot_mode = get_boot_mode();
	dev = &(info->pdev->dev);
	if (dev != NULL){
		boot_node = of_parse_phandle(dev->of_node, "bootmode", 0);
		if (!boot_node){
			chr_err("%s: failed to get boot mode phandle\n", __func__);
			return;
		}
		else {
			tag = (struct tag_bootmode *)of_get_property(boot_node,
								"atag,boot", NULL);
			if (!tag){
				chr_err("%s: failed to get atag,boot\n", __func__);
				return;
			}
			else
				boot_mode = tag->bootmode;
		}
	}
	if (is_disable_charger())
		return;

	for (i = 0; i < 3; i++) {
		if (pmic_is_battery_exist() == false)
			count++;
	}

	if (count >= 3) {
		if (boot_mode == META_BOOT || boot_mode == ADVMETA_BOOT ||
		    boot_mode == ATE_FACTORY_BOOT)
			chr_info("boot_mode = %d, bypass battery check\n",
				boot_mode);
		else {
			chr_err("battery doesn't exist, shutdown\n");
			orderly_poweroff(true);
		}
	}
}

static void check_dynamic_mivr(struct charger_manager *info)
{
	int vbat = 0;

	if (info->enable_dynamic_mivr) {
		if (!mtk_pe40_get_is_connect(info) &&
			!mtk_pe20_get_is_connect(info) &&
			!wlc_get_online() &&
			!mtk_pe_get_is_connect(info) &&
			!mtk_pdc_check_charger(info)) {

			vbat = battery_get_bat_voltage();
			if (vbat <
				info->data.min_charger_voltage_2 / 1000 - 200)
				charger_dev_set_mivr(info->chg1_dev,
					info->data.min_charger_voltage_2);
			else if (vbat <
				info->data.min_charger_voltage_1 / 1000 - 200)
				charger_dev_set_mivr(info->chg1_dev,
					info->data.min_charger_voltage_1);
			else
				charger_dev_set_mivr(info->chg1_dev,
					info->data.min_charger_voltage);
		}
	}
}

static void mtk_chg_get_tchg(struct charger_manager *info)
{
	int ret;
	int tchg_min = -127, tchg_max = -127;
	struct charger_data *pdata;
	bool en = false;

	pdata = &info->chg1_data;
	ret = charger_dev_get_temperature(info->chg1_dev, &tchg_min, &tchg_max);

	if (ret < 0) {
		pdata->junction_temp_min = -127;
		pdata->junction_temp_max = -127;
	} else {
		pdata->junction_temp_min = tchg_min;
		pdata->junction_temp_max = tchg_max;
	}

	if (is_slave_charger_exist()) {
		pdata = &info->chg2_data;
		ret = charger_dev_get_temperature(info->chg2_dev,
			&tchg_min, &tchg_max);

		if (ret < 0) {
			pdata->junction_temp_min = -127;
			pdata->junction_temp_max = -127;
		} else {
			pdata->junction_temp_min = tchg_min;
			pdata->junction_temp_max = tchg_max;
		}
	}

	if (info->dvchg1_dev) {
		pdata = &info->dvchg1_data;
		pdata->junction_temp_min = -127;
		pdata->junction_temp_max = -127;
		ret = charger_dev_is_enabled(info->dvchg1_dev, &en);
		if (ret >= 0 && en) {
			ret = charger_dev_get_adc(info->dvchg1_dev,
						  ADC_CHANNEL_TEMP_JC,
						  &tchg_min, &tchg_max);
			if (ret >= 0) {
				pdata->junction_temp_min = tchg_min;
				pdata->junction_temp_max = tchg_max;
			}
		}
	}

	if (info->dvchg2_dev) {
		pdata = &info->dvchg2_data;
		pdata->junction_temp_min = -127;
		pdata->junction_temp_max = -127;
		ret = charger_dev_is_enabled(info->dvchg2_dev, &en);
		if (ret >= 0 && en) {
			ret = charger_dev_get_adc(info->dvchg2_dev,
						  ADC_CHANNEL_TEMP_JC,
						  &tchg_min, &tchg_max);
			if (ret >= 0) {
				pdata->junction_temp_min = tchg_min;
				pdata->junction_temp_max = tchg_max;
			}
		}
	}
}

int charging_enable_flag = 1;
static void charger_check_status(struct charger_manager *info)
{
	bool charging = true;
	int temperature = 0;
	struct battery_thermal_protection_data *thermal = NULL;

	if (mt_get_charger_type() == CHARGER_UNKNOWN)
		return;

	temperature = info->battery_temp;
	thermal = &info->thermal;

#ifdef MTK_BASE
	if (info->enable_sw_jeita == true) {
		do_sw_jeita_state_machine(info);
		if (info->sw_jeita.charging == false) {
			charging = false;
			goto stop_charging;
		}
	} else {

		if (thermal->enable_min_charge_temp) {
			if (temperature < thermal->min_charge_temp) {
				chr_err("Battery Under Temperature or NTC fail %d %d\n",
					temperature, thermal->min_charge_temp);
				thermal->sm = BAT_TEMP_LOW;
				charging = false;
				goto stop_charging;
			} else if (thermal->sm == BAT_TEMP_LOW) {
				if (temperature >=
				    thermal->min_charge_temp_plus_x_degree) {
					chr_err("Battery Temperature raise from %d to %d(%d), allow charging!!\n",
					thermal->min_charge_temp,
					temperature,
					thermal->min_charge_temp_plus_x_degree);
					thermal->sm = BAT_TEMP_NORMAL;
				} else {
					charging = false;
					goto stop_charging;
				}
			}
		}

		if (temperature >= thermal->max_charge_temp) {
			chr_err("Battery over Temperature or NTC fail %d %d\n",
				temperature, thermal->max_charge_temp);
			thermal->sm = BAT_TEMP_HIGH;
			charging = false;
			goto stop_charging;
		} else if (thermal->sm == BAT_TEMP_HIGH) {
			if (temperature
			    < thermal->max_charge_temp_minus_x_degree) {
				chr_err("Battery Temperature raise from %d to %d(%d), allow charging!!\n",
				thermal->max_charge_temp,
				temperature,
				thermal->max_charge_temp_minus_x_degree);
				thermal->sm = BAT_TEMP_NORMAL;
			} else {
				charging = false;
				goto stop_charging;
			}
		}
	}
#endif

	mtk_chg_get_tchg(info);

	if (!mtk_chg_check_vbus(info)) {
		charging = false;
		goto stop_charging;
	}

	if (info->cmd_discharging)
		charging = false;
	if (info->safety_timeout)
		charging = false;
	if (info->vbusov_stat)
		charging = false;
	if (info->sc.disable_charger == true)
		charging = false;

	if (info->mmi.pres_chrg_step == STEP_STOP)
		charging = false;
	if (info->mmi.demo_discharging)
		charging = false;

	if (info->mmi.factory_mode) {
		if (charging_enable_flag)
			charging = true;
		else
			charging = false;
	}

#ifdef CONFIG_MOTO_CHG_FFC_5V10W_SUPPORT
	if (info->ffc_discharging == true)
		charging = false;
#endif

stop_charging:
	mtk_battery_notify_check(info);

	chr_err("tmp:%d (jeita:%d sm:%d cv:%d en:%d) (sm:%d) en:%d c:%d s:%d ov:%d sc:%d %d %d\n",
		temperature, info->enable_sw_jeita, info->sw_jeita.sm,
		info->sw_jeita.cv, info->sw_jeita.charging, thermal->sm,
		charging, info->cmd_discharging, info->safety_timeout,
		info->vbusov_stat, info->sc.disable_charger,
		info->can_charging, charging);

	if (charging != info->can_charging)
		_charger_manager_enable_charging(info->chg1_consumer,
						0, charging);

	info->can_charging = charging;
}

static struct power_supply *cp_psy = NULL;

static bool mmi_get_pd_pps_support(struct charger_manager *info)
{
	int rc;
	bool pd_pps_support = false;
	union power_supply_propval val;
	if (!cp_psy) {
		cp_psy = power_supply_get_by_name("mmi_chrg_manager");
		if (!cp_psy) {
			pr_err("[%s] get cp psy failed\n", __func__);
			return false;
		}
	}
	rc = power_supply_get_property(cp_psy,
				POWER_SUPPLY_PROP_PD_PPS_SUPPORT, &val);
	if (!rc) {
		pd_pps_support = val.intval;
		pr_err("[%s] get pd pps support success, val is %d\n", __func__, val.intval);
	}
	return pd_pps_support;
}

/*********************
 * MMI Functionality *
 *********************/

static struct charger_manager *mmi_info;

static char *stepchg_str[] = {
	[STEP_MAX]		= "MAX",
	[STEP_NORM]		= "NORMAL",
	[STEP_FULL]		= "FULL",
	[STEP_FLOAT]		= "FLOAT",
	[STEP_DEMO]		= "DEMO",
	[STEP_STOP]		= "STOP",
	[STEP_NONE]		= "NONE",
};

int mmi_get_prop_from_battery(struct charger_manager *info,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	int rc;

	if (!info->battery_psy) {
		info->battery_psy = power_supply_get_by_name("battery");

		if (!info->battery_psy) {
			pr_err("[%s]Error getting battery power sypply\n", __func__);
			return -EINVAL;
		}
	}

	rc = power_supply_get_property(info->battery_psy, psp, val);

	return rc;
}

int mmi_get_prop_from_charger(struct charger_manager *info,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	int rc;

	if (!info->charger_psy) {
		info->charger_psy = power_supply_get_by_name("charger");

		if (!info->charger_psy) {
			pr_err("[%s]Error getting charger power sypply\n", __func__);
			return -EINVAL;
		}
	}

	rc = power_supply_get_property(info->charger_psy, psp, val);

	return rc;
}

void update_charging_limit_modes(struct charger_manager *info, int batt_soc)
{
	enum charging_limit_modes charging_limit_modes;

	charging_limit_modes = info->mmi.charging_limit_modes;
	if ((charging_limit_modes != CHARGING_LIMIT_RUN)
	    && (batt_soc >= info->mmi.upper_limit_capacity))
		charging_limit_modes = CHARGING_LIMIT_RUN;
	else if ((charging_limit_modes != CHARGING_LIMIT_OFF)
		   && (batt_soc <= info->mmi.lower_limit_capacity))
		charging_limit_modes = CHARGING_LIMIT_OFF;

	if (charging_limit_modes != info->mmi.charging_limit_modes)
		info->mmi.charging_limit_modes = charging_limit_modes;
}

#define WEAK_CHRG_THRSH 450
#define TURBO_CHRG_THRSH 2500
int mmi_chrg_rate_check(void)
{
	int chg_rate = POWER_SUPPLY_CHARGE_RATE_NONE;
	int icl = 0;
	int icl_c = 0;
	int  rc = 0;
	union power_supply_propval val;
	char *charge_rate[] = {
		"None", "Normal", "Weak", "Turbo"
	};

	if (pinfo == NULL) {
		chg_rate = POWER_SUPPLY_CHARGE_RATE_NONE;
		goto end_rate_check;
	}

	if (!pinfo->wl_psy) {
		pinfo->wl_psy = power_supply_get_by_name("wireless");
	}
	if (pinfo->wl_psy) {
		rc = power_supply_get_property(pinfo->wl_psy,
				POWER_SUPPLY_PROP_ONLINE, &val);
		if (val.intval) {
			rc = power_supply_get_property(pinfo->wl_psy,
				POWER_SUPPLY_PROP_POWER_NOW, &val);
			if (val.intval >= WLS_RX_CAP_15W) {
				chg_rate = POWER_SUPPLY_CHARGE_RATE_TURBO;
			} else if(val.intval >= WLS_RX_CAP_5W) {
				chg_rate = POWER_SUPPLY_CHARGE_RATE_NORMAL;
			} else {
				chg_rate = POWER_SUPPLY_CHARGE_RATE_WEAK;
			}
			goto end_rate_check;
		}
	}

	icl = pinfo->chg1_data.input_current_limit / 1000;
	icl_c = pinfo->chg1_data.typec_input_current_limit;

	rc = mmi_get_prop_from_charger(pinfo,
				POWER_SUPPLY_PROP_ONLINE, &val);
	if (rc < 0) {
		pr_err("[%s]Error get chg online rc = %d\n", __func__, rc);
		chg_rate = POWER_SUPPLY_CHARGE_RATE_NONE;
		goto end_rate_check;
	} else if (!val.intval) {
		chg_rate = POWER_SUPPLY_CHARGE_RATE_NONE;
		goto end_rate_check;
	}

	if (is_typec_adapter(pinfo)
		&& adapter_dev_get_property(pinfo->pd_adapter, TYPEC_RP_LEVEL)
			== 3000) {
			if (icl_c == -1 || icl_c > TURBO_CHRG_THRSH * 1000) {
				chg_rate = POWER_SUPPLY_CHARGE_RATE_TURBO;
				goto end_rate_check;
			}
	}

#if defined(CONFIG_MOTO_CHG_BQ25601_SUPPORT) || defined(CONFIG_MOTO_CHG_WT6670F_SUPPORT) || defined(CONFIG_MOTO_CHG_FFC_5V10W_SUPPORT)
	if (icl > TURBO_CHRG_THRSH) {
#else
	if (icl >= TURBO_CHRG_THRSH) {
#endif
		chg_rate = POWER_SUPPLY_CHARGE_RATE_TURBO;
		goto end_rate_check;
	} else if (icl < WEAK_CHRG_THRSH) {
		chg_rate = POWER_SUPPLY_CHARGE_RATE_WEAK;
		goto end_rate_check;
	}

        if (mtk_pe_get_is_connect(pinfo)) {
                pr_info("[%s]pe connect, set as Turbo\n", __func__);
                chg_rate = POWER_SUPPLY_CHARGE_RATE_TURBO;
		goto end_rate_check;
        }

	chg_rate =  POWER_SUPPLY_CHARGE_RATE_NORMAL;

end_rate_check:
	pr_info("%s Charger Detected\n", charge_rate[chg_rate]);
	return chg_rate;
}

int mmi_batt_health_check(void)
{
	if (pinfo == NULL) {
		pr_err("[%s]called before charger_manager valid!\n", __func__);
		return POWER_SUPPLY_HEALTH_GOOD;
	}
	return pinfo->mmi.batt_health;
}

static int mmi_get_ffc_fv(struct charger_manager *info, int zone)
{
	int ffc_max_fv;

	if (info->mmi.ffc_zones == NULL
		|| zone >= info->mmi.num_temp_zones)
		return 0;

	info->mmi.chrg_iterm = info->mmi.ffc_zones[zone].ffc_chg_iterm;
	ffc_max_fv = info->mmi.ffc_zones[zone].ffc_max_mv;
	pr_info("FFC temp zone %d, fv %d mV, chg iterm %d mA\n",
		  zone, ffc_max_fv, info->mmi.chrg_iterm);

	return ffc_max_fv;
}

#define MIN_TEMP_C -20
#define MAX_TEMP_C 60
#define MIN_MAX_TEMP_C 47
#define HYSTERISIS_DEGC 2
static bool mmi_find_temp_zone(struct charger_manager *info, int temp_c)
{
	int prev_zone, num_zones;
	struct mmi_temp_zone *zones;
	int hotter_t, hotter_fcc;
	int colder_t, colder_fcc;
	int i;
	int max_temp;

	if (!info) {
		pr_err("[%s]called before charger_manager valid!\n", __func__);
		return false;
	}

	zones = info->mmi.temp_zones;
	num_zones = info->mmi.num_temp_zones;
	prev_zone = info->mmi.pres_temp_zone;

	if (info->mmi.max_chrg_temp >= MIN_MAX_TEMP_C)
		max_temp = info->mmi.max_chrg_temp;
	else
		max_temp = zones[num_zones - 1].temp_c;

	if (prev_zone == ZONE_NONE) {
		for (i = num_zones - 1; i >= 0; i--) {
			if (temp_c >= zones[i].temp_c) {
				if (i == num_zones - 1)
					info->mmi.pres_temp_zone = ZONE_HOT;
				else
					info->mmi.pres_temp_zone = i + 1;
				return true;
			}
		}
		info->mmi.pres_temp_zone = ZONE_COLD;
		return true;
	}

	if (prev_zone == ZONE_COLD) {
		if (temp_c >= MIN_TEMP_C + HYSTERISIS_DEGC)
			info->mmi.pres_temp_zone = ZONE_FIRST;
	} else if (prev_zone == ZONE_HOT) {
		if (temp_c <= max_temp - HYSTERISIS_DEGC)
			info->mmi.pres_temp_zone = num_zones - 1;
	} else {
		if (prev_zone == ZONE_FIRST) {
			hotter_fcc = zones[prev_zone + 1].fcc_max_ma;
			colder_fcc = 0;
			hotter_t = zones[prev_zone].temp_c;
			colder_t = MIN_TEMP_C;
		} else if (prev_zone == num_zones - 1) {
			hotter_fcc = 0;
			colder_fcc = zones[prev_zone - 1].fcc_max_ma;
			hotter_t = zones[prev_zone].temp_c;
			colder_t = zones[prev_zone - 1].temp_c;
		} else {
			hotter_fcc = zones[prev_zone + 1].fcc_max_ma;
			colder_fcc = zones[prev_zone - 1].fcc_max_ma;
			hotter_t = zones[prev_zone].temp_c;
			colder_t = zones[prev_zone - 1].temp_c;
		}

		if (zones[prev_zone].fcc_max_ma < hotter_fcc)
			hotter_t += HYSTERISIS_DEGC;

		if (zones[prev_zone].fcc_max_ma < colder_fcc)
			colder_t -= HYSTERISIS_DEGC;

		if (temp_c < MIN_TEMP_C)
			info->mmi.pres_temp_zone = ZONE_COLD;
		else if (temp_c >= max_temp)
			info->mmi.pres_temp_zone = ZONE_HOT;
		else if (temp_c >= hotter_t)
			info->mmi.pres_temp_zone++;
		else if (temp_c < colder_t)
			info->mmi.pres_temp_zone--;
	}

	if (prev_zone != info->mmi.pres_temp_zone) {
		pr_info("[%s]Entered Temp Zone %d!\n", __func__,
			   info->mmi.pres_temp_zone);
		return true;
	}

	return false;
}

#define TAPER_COUNT 2
#define TAPER_DROP_MA 100
static bool mmi_has_current_tapered(struct charger_manager*info,
				    int batt_ma, int taper_ma)
{
	bool change_state = false;
	int allowed_fcc, target_ma, rc;

	if (!info) {
		pr_err("[%s]called before info valid!\n", __func__);
		return false;
	}

	rc = charger_dev_get_charging_current(info->chg1_dev, &allowed_fcc);
	if (rc < 0) {
		pr_err("[%s]can't get charging current!\n", __func__);
	} else
		allowed_fcc = allowed_fcc /1000;

	if (allowed_fcc >= taper_ma)
		target_ma = taper_ma;
	else
		target_ma = allowed_fcc - TAPER_DROP_MA;

	if (batt_ma > 0) {
		if (batt_ma <= target_ma)
			if (info->mmi.chrg_taper_cnt >= TAPER_COUNT) {
				change_state = true;
				info->mmi.chrg_taper_cnt = 0;
			} else
				info->mmi.chrg_taper_cnt++;
		else
			info->mmi.chrg_taper_cnt = 0;
	} else {
		if (info->mmi.chrg_taper_cnt >= TAPER_COUNT) {
			change_state = true;
			info->mmi.chrg_taper_cnt = 0;
		} else
			info->mmi.chrg_taper_cnt++;
	}

	return change_state;
}

#ifdef CONFIG_MOTO_CHG_FFC_5V10W_SUPPORT
static void ffc_enable_charge_work(struct work_struct *work)
{
	struct charger_manager *info = container_of(work,
							struct charger_manager, ffc_enable_charge_work.work);
	enum charger_type chr_type;
	int rc;
	int vbat_min = 0, vbat_max = 0, vbatt = 0;

	do {
		rc = charger_dev_get_adc(info->chg1_dev, ADC_CHANNEL_VBAT, &vbat_min, &vbat_max);

		if (rc < 0) {
			pr_err("[%s] Error getting batt voltage rc=%d\n", __func__, rc);
			break;
		}

		if (vbat_min < 0) {
			pr_err("[%s] error batt voltage:%d:%d\n", __func__, vbat_min, vbat_max);
			break;
		}

		vbatt = vbat_min / 1000;

	} while (0);

	if (info->ffc_max_fv_mv_backup >= vbatt) {
		info->ffc_discharging = false;
		chr_type = mt_get_charger_type();

		if (chr_type != CHARGER_UNKNOWN) {
			_wake_up_charger(pinfo);
		}
		__pm_relax(info->ffc_charger_wakelock);
	}
	else {
		if (!info->ffc_charger_wakelock->active)
			__pm_stay_awake(info->ffc_charger_wakelock);
		schedule_delayed_work(&info->ffc_enable_charge_work, 5 * HZ);
	}

	pr_info("ffc_enable_charge_work\n");
}

static int mmi_charger_check_dcp_ffc_status(struct charger_manager *info, int batt_soc, int current_charge_state, int _max_fv_mv)
{
	int rc;
	bool loop = true;
	struct mmi_params *mmi = &info->mmi;
	int max_fv_mv = _max_fv_mv, vbatt;
	int vcurr, vbat_min = 0, vbat_max = 0;
	union power_supply_propval val;
	unsigned long target_timestamp;

	do {
		pr_debug("[%s] ffc_state:%d, charge stage:%d, uisoc:%d\n", __func__, mmi->ffc_state, current_charge_state, batt_soc);
		switch (mmi->ffc_state) {
			case CHARGER_FFC_STATE_INITIAL:
				if (current_charge_state == STEP_MAX &&
					(info->chr_type == STANDARD_CHARGER && !is_typec_adapter(info) &&
					!(mtk_pe_get_is_cable_connect(info) || mmi_get_pd_pps_support(info)))) {
					if (info->data.ac_charger_input_current == info->ffc_input_current_backup) {
						info->data.ac_charger_input_current += 500000;
						/* bump charging current +500mA to try ffc */
						charger_dev_set_input_current(info->chg1_dev, info->data.ac_charger_input_current);
						pr_debug("[%s] bump inputcur up to %d\n", __func__, info->data.ac_charger_input_current);
					}

					if (batt_soc >= mmi->ffc_uisoc_threshold) {
						mmi->ffc_state = CHARGER_FFC_STATE_PROBING;
						pr_debug("[%s] uisoc is up to %d, ffc probing starts\n", __func__, mmi->ffc_uisoc_threshold);
					}
				}
				else {
					mmi->ffc_state = CHARGER_FFC_STATE_INVALID;
					pr_debug("[%s] ui_soc:%d, charge_type:%d not for ffc\n", __func__, batt_soc, info->chr_type);
				}
				loop = false;
			break;
			case CHARGER_FFC_STATE_PROBING:
				if (current_charge_state == STEP_MAX || current_charge_state == STEP_NORM) {
					if ((rc = mmi_get_prop_from_battery(info, POWER_SUPPLY_PROP_CURRENT_NOW, &val)) < 0) {
						pr_err("[%s] Error getting batt current avg current rc=%d\n", __func__, rc);
						mmi->ffc_state = CHARGER_FFC_STATE_INVALID;
						loop = false;
						break;
					}
					if (val.intval < 0) {
						pr_err("[%s] still in discharging at:%d\n", __func__, val.intval);
						loop = false;
						break;
					}

					if (val.intval > mmi->ffc_entry_threshold) {
						pr_debug("[%s] charging current can bump up to entry threshold:%d\n", __func__, mmi->ffc_entry_threshold);
						mmi->ffc_state = CHARGER_FFC_STATE_STANDBY;
					}
					else if (val.intval == 0) {
						pr_err("[%s] charger doesnt support to bump high level current\n", __func__);
						mmi->ffc_state = CHARGER_FFC_STATE_INVALID;
					}
					else {
						pr_debug("[%s] current: %d not reach ffc entry threshold\n", __func__, val.intval);
						loop = false;
					}
				}
				else {
					mmi->ffc_state = CHARGER_FFC_STATE_INVALID;
					pr_debug("[%s] invalid stage:%d, ffc session quit\n", __func__, current_charge_state);
				}
			break;
			case CHARGER_FFC_STATE_STANDBY:
				if (current_charge_state != STEP_MAX && current_charge_state != STEP_NORM) {
					pr_info("[%s] invalid stage:%d in ffc_state:%d\n", __func__, current_charge_state, mmi->ffc_state);
					mmi->ffc_state = CHARGER_FFC_STATE_INVALID;
					break;
				}

				if (current_charge_state == STEP_NORM && mmi->ffc_iavg >= mmi->ffc_entry_threshold) {
					mmi->ffc_state = CHARGER_FFC_STATE_GOREADY;
					break;
				}

				target_timestamp = mmi->ffc_iavg_update_timestamp + msecs_to_jiffies(10 * 1000);
				
				if (time_is_before_eq_jiffies(target_timestamp) || mmi->ffc_iavg_update_timestamp == 0) {
					/* iavg update required */
					if ((rc = mmi_get_prop_from_battery(info, POWER_SUPPLY_PROP_CURRENT_NOW, &val)) < 0) {
						pr_err("[%s] Error getting batt current avg current rc=%d\n", __func__, rc);
						mmi->ffc_state = CHARGER_FFC_STATE_INVALID;
						break;
					}

					if (val.intval < 0) {
						pr_err("[%s] still in discharging at:%d\n", __func__, val.intval);
						break;
					}

					mmi->ffc_iavg_update_timestamp = jiffies;
					mmi->ffc_ibat_count++;
					mmi->ffc_ibat_windowsum += val.intval;
					mmi->ffc_iavg = mmi->ffc_ibat_windowsum / mmi->ffc_ibat_count;
					pr_debug("[%s] iavg:%d, total:%d, count:%d\n", __func__, mmi->ffc_iavg, mmi->ffc_ibat_windowsum, mmi->ffc_ibat_count);
				}

				loop = false;
			break;
			case CHARGER_FFC_STATE_GOREADY:
				if (current_charge_state != STEP_NORM) {
					pr_err("%s] invalid stage:%d in ffc_state:%d, quit ffc session\n", __func__, current_charge_state, mmi->ffc_state);
					mmi->ffc_state = CHARGER_FFC_STATE_INVALID;
					break;
				}
				mmi->ffc_ibat_count = 0;
				mmi->ffc_ibat_windowsum = 0;
				mmi->ffc_iavg = 0;
				mmi->ffc_iavg_update_timestamp = 0;
				/* set target voltage as the ffc target */
				max_fv_mv = mmi_get_ffc_fv(info, mmi->pres_temp_zone);
				if (max_fv_mv == 0) {
					max_fv_mv = _max_fv_mv;
					pr_info("[%s] set ffc max_fv_mv fail, reset value to %d\n", __func__, max_fv_mv);
				}
				mmi->ffc_state = CHARGER_FFC_STATE_RUNNING;
				loop = false;
				pr_debug("[%s] target max_fv_mv:%d\n", __func__, max_fv_mv);
			break;
			case CHARGER_FFC_STATE_RUNNING:
				if (current_charge_state != STEP_NORM) {
					pr_err("%s] invalid stage:%d in ffc_state:%d, quit ffc session\n", __func__, current_charge_state, mmi->ffc_state);
					mmi->ffc_state = CHARGER_FFC_STATE_INVALID;
					break;
				}
				if ((rc = mmi_get_prop_from_battery(info, POWER_SUPPLY_PROP_CURRENT_NOW, &val)) < 0) {
					pr_err("[%s] Error getting batt current avg current rc=%d\n", __func__, rc);
					mmi->ffc_state = CHARGER_FFC_STATE_INVALID;
					break;
				}

				if (val.intval < 0) {
					pr_err("[%s] still in discharging at:%d, retry\n", __func__, val.intval);
					break;
				}

				vcurr = val.intval;

				rc = charger_dev_get_adc(info->chg1_dev, ADC_CHANNEL_VBAT, &vbat_min, &vbat_max);

				if (rc < 0) {
					pr_err("[%s] Error getting batt voltage rc=%d\n", __func__, rc);
					mmi->ffc_state = CHARGER_FFC_STATE_INVALID;
					break;
				}

				if (vbat_min < 0) {
					pr_err("[%s] error batt voltage:%d, retry\n", __func__, val.intval);
					break;
				}

				vbatt = vbat_min / 1000;

				max_fv_mv = mmi_get_ffc_fv(info, mmi->pres_temp_zone);
				if (max_fv_mv == 0) {
					max_fv_mv = _max_fv_mv;
					pr_info("[%s] get ffc max_fv_mv fail, reset value to %d\n", __func__, max_fv_mv);
					mmi->ffc_state = CHARGER_FFC_STATE_INVALID;
					break;
				}

				/* float down offset 5mV to fit in more robust ffc state */
				if (vbatt < (max_fv_mv - 5)) {
					mmi->ffc_ibat_count++;
					mmi->ffc_ibat_windowsum += vcurr;
					loop = false;

					if ((mmi->ffc_ibat_count % mmi->ffc_ibat_windowsize) == 0) {
						mmi->ffc_iavg = mmi->ffc_ibat_windowsum / mmi->ffc_ibat_count;
						if (mmi->ffc_iavg <= mmi->ffc_exit_threshold) {
							loop = true;
							mmi->ffc_state = CHARGER_FFC_STATE_AVGEXIT;
						}
						pr_debug("[%s] ffc_iavg:%d in ffc, max_fv_mv:%d, vbatt:%d\n", __func__, mmi->ffc_iavg, max_fv_mv, vbatt);
					}
				}
				else {
					pr_debug("[%s] ffc charging to target voltage:%d, quit ffc session \n", __func__, max_fv_mv);
					mmi->ffc_state = CHARGER_FFC_STATE_FFCDONE;
					break;
				}
			break;
			case CHARGER_FFC_STATE_AVGEXIT:
				max_fv_mv = _max_fv_mv;
				mmi->ffc_ibat_count = 0;
				mmi->ffc_ibat_windowsum = 0;
				mmi->ffc_iavg = 0;
				info->ffc_discharging = true;
				info->ffc_max_fv_mv_backup = max_fv_mv;
				if (!info->ffc_charger_wakelock->active)
					__pm_stay_awake(info->ffc_charger_wakelock);
				schedule_delayed_work(&info->ffc_enable_charge_work, 5 * HZ);

				mmi->ffc_state = CHARGER_FFC_STATE_INVALID;
				loop = false;
			break;
			case CHARGER_FFC_STATE_FFCDONE:
				max_fv_mv = mmi_get_ffc_fv(info, mmi->pres_temp_zone);
				if (max_fv_mv == 0) {
					max_fv_mv = _max_fv_mv;
					pr_info("[%s] set ffc max_fv_mv fail, reset value to %d\n", __func__, max_fv_mv);
					mmi->ffc_state = CHARGER_FFC_STATE_INVALID;
					break;
				}
				pr_debug("[%s] ffc session done, max_fv_mv:%d\n", __func__, max_fv_mv);
				loop = false;
			break;
			case CHARGER_FFC_STATE_INVALID:
				max_fv_mv = _max_fv_mv;
				info->data.ac_charger_input_current = info->ffc_input_current_backup;
				// charger_dev_set_input_current(info->chg1_dev, info->data.ac_charger_input_current);
				pr_err("[%s] no ffc session, restore input cur:%d\n", __func__, info->data.ac_charger_input_current);
				loop = false;
			break;
		}

	} while (loop);

	return max_fv_mv;
}

#endif

#define WARM_TEMP 45
#define COOL_TEMP 0
#define HYST_STEP_MV 50
#define DEMO_MODE_HYS_SOC 5
#define DEMO_MODE_VOLTAGE 4000
static void mmi_charger_check_status(struct charger_manager *info)
{
	int rc;
	int batt_mv;
	int batt_ma;
	int batt_soc;
	int batt_temp;
	int usb_mv;
	int charger_present = 0;
	int stop_recharge_hyst;
	int prev_step;

	union power_supply_propval val;
	struct mmi_params *mmi = &info->mmi;
	struct mmi_temp_zone *zone;
	int max_fv_mv = -EINVAL;
	int target_fcc = -EINVAL;
	int target_fv = -EINVAL;

	/* Collect Current Information */

	rc = mmi_get_prop_from_battery(info,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
	if (rc < 0) {
		pr_err("[%s]Error getting Batt Voltage rc = %d\n", __func__, rc);
		goto end_check;
	} else
		batt_mv = val.intval / 1000;

	rc = mmi_get_prop_from_battery(info,
				POWER_SUPPLY_PROP_CURRENT_NOW, &val);
	if (rc < 0) {
		pr_err("[%s]Error getting Batt Current rc = %d\n", __func__, rc);
		goto end_check;
	} else
		batt_ma = val.intval / 1000;

	rc = mmi_get_prop_from_battery(info,
				POWER_SUPPLY_PROP_CAPACITY, &val);
	if (rc < 0) {
		pr_err("[%s]Error getting Batt Capacity rc = %d\n", __func__, rc);
		goto end_check;
	} else
		batt_soc = val.intval;

	rc = mmi_get_prop_from_battery(info,
				POWER_SUPPLY_PROP_TEMP, &val);
	if (rc < 0) {
		pr_err("[%s]Error getting Batt Temperature rc = %d\n", __func__, rc);
		goto end_check;
	} else
		batt_temp = val.intval / 10;

	rc = mmi_get_prop_from_charger(info,
				POWER_SUPPLY_PROP_ONLINE, &val);
	if (rc < 0) {
		pr_err("[%s]Error getting charger online rc = %d\n", __func__, rc);
		goto end_check;
	} else
		charger_present = val.intval;

	usb_mv = charger_get_vbus();


	pr_info("[%s]batt=%d mV, %d mA, %d C, USB= %d mV\n", __func__,
		batt_mv, batt_ma, batt_temp, usb_mv);

	if (!mmi->temp_zones) {
		pr_err("[%s]temp_zones is NULL\n", __func__);
		pr_info("[%s]EFFECTIVE: FV = %d, CDIS = %d, FCC = %d, "
		"USBICL = %d, DEMO_DISCHARG = %d\n", __func__,
		mmi->target_fv,
		mmi->chg_disable,
		mmi->target_fcc,
		mmi->target_usb,
		mmi->demo_discharging);

		goto end_check;
	}

	if (mmi->enable_charging_limit && mmi->is_factory_image)
		update_charging_limit_modes(info, batt_soc);

	mmi_find_temp_zone(info, batt_temp);
	if (mmi->pres_temp_zone >= info->mmi.num_temp_zones)
		zone = &mmi->temp_zones[0];
	else
		zone = &mmi->temp_zones[mmi->pres_temp_zone];

	if (mmi->base_fv_mv == 0) {
		mmi->base_fv_mv = info->data.battery_cv / 1000;
	}
        pr_info("[%s]mtk pe connect: %d\n",
		 __func__, mtk_pe_get_is_cable_connect(info));
	if (mtk_pe_get_is_cable_connect(info) || mmi_get_pd_pps_support(info)) {
		max_fv_mv = mmi_get_ffc_fv(info, mmi->pres_temp_zone);
		if (max_fv_mv == 0)
			max_fv_mv = mmi->base_fv_mv;
	} else {
		max_fv_mv = mmi->base_fv_mv;
		info->mmi.chrg_iterm = info->mmi.back_chrg_iterm;
	}

	/* Determine Next State */
	prev_step = info->mmi.pres_chrg_step;

	if (mmi->charging_limit_modes == CHARGING_LIMIT_RUN)
		pr_warn("Factory Mode/Image so Limiting Charging!!!\n");

	if (!charger_present && !wlc_get_online()) {
		mmi->pres_chrg_step = STEP_NONE;
	} else if ((mmi->pres_temp_zone == ZONE_HOT) ||
		   (mmi->pres_temp_zone == ZONE_COLD) ||
		   (mmi->charging_limit_modes == CHARGING_LIMIT_RUN)) {
		info->mmi.pres_chrg_step = STEP_STOP;
	} else if (mmi->demo_mode) {
		bool voltage_full;
		static int demo_full_soc = 100;
		static int usb_suspend = 0;

		mmi->pres_chrg_step = STEP_DEMO;
		pr_info("[%s]Battery in Demo Mode charging Limited %dper\n",
				__func__, mmi->demo_mode);

		voltage_full = ((usb_suspend == 0) &&
			((batt_mv + HYST_STEP_MV) >= DEMO_MODE_VOLTAGE) &&
			mmi_has_current_tapered(info, batt_ma,
						mmi->chrg_iterm));

		if ((usb_suspend == 0) &&
		    ((batt_soc >= mmi->demo_mode) ||
		     voltage_full)) {
			demo_full_soc = batt_soc;
			mmi->demo_discharging = true;
			usb_suspend = 1;
		} else if (usb_suspend &&
			   (batt_soc <=
				(demo_full_soc - DEMO_MODE_HYS_SOC))) {
			mmi->demo_discharging = false;
			usb_suspend = 0;
			mmi->chrg_taper_cnt = 0;
		}
		if (usb_suspend)
			charger_dev_set_input_current(info->chg1_dev, 0);

		pr_info("Charge Demo Mode:us = %d, vf = %d, dfs = %d,bs = %d\n",
				usb_suspend, voltage_full, demo_full_soc, batt_soc);
	} else if (mmi->pres_chrg_step == STEP_NONE) {
		if (zone->norm_mv && ((batt_mv + HYST_STEP_MV) >= zone->norm_mv)) {
			if (zone->fcc_norm_ma)
				mmi->pres_chrg_step = STEP_NORM;
			else
				mmi->pres_chrg_step = STEP_STOP;
		} else
			mmi->pres_chrg_step = STEP_MAX;
	} else if (mmi->pres_chrg_step == STEP_STOP) {
		if (batt_temp > COOL_TEMP)
			stop_recharge_hyst = 2 * HYST_STEP_MV;
		else
			stop_recharge_hyst = 5 * HYST_STEP_MV;
		if (zone->norm_mv && ((batt_mv + stop_recharge_hyst) >= zone->norm_mv)) {
			if (zone->fcc_norm_ma)
				mmi->pres_chrg_step = STEP_NORM;
			else
				mmi->pres_chrg_step = STEP_STOP;
		} else
			mmi->pres_chrg_step = STEP_MAX;
	}else if (mmi->pres_chrg_step == STEP_MAX) {
		if (!zone->norm_mv) {
			/* No Step in this Zone */
#ifdef CONFIG_MOTO_CHG_FFC_5V10W_SUPPORT
			mmi_charger_check_dcp_ffc_status(info, batt_soc, STEP_MAX, max_fv_mv);
#endif
			mmi->chrg_taper_cnt = 0;
			if ((batt_mv + HYST_STEP_MV) >= max_fv_mv)
				mmi->pres_chrg_step = STEP_NORM;
			else
				mmi->pres_chrg_step = STEP_MAX;
		} else if ((batt_mv + HYST_STEP_MV) < zone->norm_mv) {
			mmi->chrg_taper_cnt = 0;
			mmi->pres_chrg_step = STEP_MAX;
#ifdef CONFIG_MOTO_CHG_FFC_5V10W_SUPPORT
			mmi_charger_check_dcp_ffc_status(info, batt_soc, STEP_MAX, max_fv_mv);
#endif
		} else if (!zone->fcc_norm_ma)
			mmi->pres_chrg_step = STEP_FLOAT;
		else if (mmi_has_current_tapered(info, batt_ma,
						 zone->fcc_norm_ma)) {
			mmi->chrg_taper_cnt = 0;
			mmi->pres_chrg_step = STEP_NORM;
		}
	} else if (mmi->pres_chrg_step == STEP_NORM) {
#ifdef CONFIG_MOTO_CHG_FFC_5V10W_SUPPORT
		max_fv_mv = mmi_charger_check_dcp_ffc_status(info, batt_soc, STEP_NORM, max_fv_mv);
#endif
		if (!zone->fcc_norm_ma)
			mmi->pres_chrg_step = STEP_FLOAT;
		else if ((batt_mv + HYST_STEP_MV/2) < max_fv_mv) {
			mmi->chrg_taper_cnt = 0;
			mmi->pres_chrg_step = STEP_NORM;
		} else if (mmi_has_current_tapered(info, batt_ma,
						   mmi->chrg_iterm)) {
			mmi->pres_chrg_step = STEP_FULL;
		}
	} else if (mmi->pres_chrg_step == STEP_FULL) {
		if (batt_mv < (max_fv_mv - HYST_STEP_MV * 2)) {
			mmi->chrg_taper_cnt = 0;
			mmi->pres_chrg_step = STEP_NORM;
		}
	} else if (mmi->pres_chrg_step == STEP_FLOAT) {
		if ((zone->fcc_norm_ma) ||
		    ((batt_mv + HYST_STEP_MV) < zone->norm_mv))
			mmi->pres_chrg_step = STEP_MAX;
		else if (mmi_has_current_tapered(info, batt_ma,
				   mmi->chrg_iterm))
			mmi->pres_chrg_step = STEP_STOP;

	}

	/* Take State actions */
	switch (mmi->pres_chrg_step) {
	case STEP_FLOAT:
	case STEP_MAX:
		if (!zone->norm_mv)
			target_fv = max_fv_mv + mmi->vfloat_comp_mv;
		else
			target_fv = zone->norm_mv + mmi->vfloat_comp_mv;
		target_fcc = zone->fcc_max_ma;
		break;
	case STEP_FULL:
		target_fv = max_fv_mv;
		target_fcc = -EINVAL;
		break;
	case STEP_NORM:
		target_fv = max_fv_mv + mmi->vfloat_comp_mv;
		target_fcc = zone->fcc_norm_ma;
		break;
	case STEP_NONE:
		target_fv = max_fv_mv;
		target_fcc = zone->fcc_norm_ma;
		break;
	case STEP_STOP:
		target_fv = max_fv_mv;
		target_fcc = -EINVAL;
		break;
	case STEP_DEMO:
		target_fv = DEMO_MODE_VOLTAGE;
		target_fcc = zone->fcc_max_ma;
		break;
	default:
		break;
	}

	mmi->target_fv = target_fv * 1000;

	mmi->chg_disable = (target_fcc < 0);

	mmi->target_fcc = ((target_fcc >= 0) ? (target_fcc * 1000) : 0);

	if (info->mmi.pres_temp_zone == ZONE_HOT) {
		info->mmi.batt_health = POWER_SUPPLY_HEALTH_OVERHEAT;
	} else if (info->mmi.pres_temp_zone == ZONE_COLD) {
		info->mmi.batt_health = POWER_SUPPLY_HEALTH_COLD;
	} else if (batt_temp >= WARM_TEMP) {
		if (info->mmi.pres_chrg_step == STEP_STOP)
			info->mmi.batt_health = POWER_SUPPLY_HEALTH_OVERHEAT;
		else
			info->mmi.batt_health = POWER_SUPPLY_HEALTH_GOOD;
	} else if (batt_temp <= COOL_TEMP) {
		if (info->mmi.pres_chrg_step == STEP_STOP)
			info->mmi.batt_health = POWER_SUPPLY_HEALTH_COLD;
		else
			info->mmi.batt_health = POWER_SUPPLY_HEALTH_GOOD;
	} else
		info->mmi.batt_health = POWER_SUPPLY_HEALTH_GOOD;

	pr_info("[%s]FV %d mV, FCC %d mA\n",
		 __func__, target_fv, target_fcc);
	pr_info("[%s]Step State = %s\n", __func__,
		stepchg_str[(int)mmi->pres_chrg_step]);
	pr_info("[%s]EFFECTIVE: FV = %d, CDIS = %d, FCC = %d, "
		"USBICL = %d, DEMO_DISCHARG = %d\n",
		__func__,
		mmi->target_fv,
		mmi->chg_disable,
		mmi->target_fcc,
		mmi->target_usb,
		mmi->demo_discharging);
	pr_info("[%s]adaptive charging:disable_ibat = %d, "
		"disable_ichg = %d, enable HZ = %d, "
		"charging disable = %d\n",
		__func__,
		mmi->adaptive_charging_disable_ibat,
		mmi->adaptive_charging_disable_ichg,
		mmi->charging_enable_hz,
		mmi->battery_charging_disable);
end_check:

	return;
}

static int parse_mmi_dt(struct charger_manager *info, struct device *dev)
{
	struct device_node *node = dev->of_node;
	int rc = 0;
	int byte_len;
	int i;

	if (!node) {
		pr_info("[%s]mmi dtree info. missing\n",__func__);
		return -ENODEV;
	}

	if (of_find_property(node, "mmi,mmi-temp-zones", &byte_len)) {
		if ((byte_len / sizeof(u32)) % 4) {
			pr_err("[%s]DT error wrong mmi temp zones\n",__func__);
			return -ENODEV;
		}

		info->mmi.temp_zones = (struct mmi_temp_zone *)
			devm_kzalloc(dev, byte_len, GFP_KERNEL);

		if (info->mmi.temp_zones == NULL)
			return -ENOMEM;

		info->mmi.num_temp_zones =
			byte_len / sizeof(struct mmi_temp_zone);

		rc = of_property_read_u32_array(node,
				"mmi,mmi-temp-zones",
				(u32 *)info->mmi.temp_zones,
				byte_len / sizeof(u32));
		if (rc < 0) {
			pr_err("[%s]Couldn't read mmi temp zones rc = %d\n", __func__, rc);
			return rc;
		}
		pr_info("[%s]"
			"mmi temp zones: Num: %d\n", __func__, info->mmi.num_temp_zones);
		for (i = 0; i < info->mmi.num_temp_zones; i++) {
			pr_info("[%s]"
				"mmi temp zones: Zone %d, Temp %d C, "
				"Step Volt %d mV, Full Rate %d mA, "
				"Taper Rate %d mA\n", __func__, i,
				info->mmi.temp_zones[i].temp_c,
				info->mmi.temp_zones[i].norm_mv,
				info->mmi.temp_zones[i].fcc_max_ma,
				info->mmi.temp_zones[i].fcc_norm_ma);
		}
		info->mmi.pres_temp_zone = ZONE_NONE;
	} else {
		info->mmi.temp_zones = NULL;
		info->mmi.num_temp_zones = 0;
		pr_err("[%s]mmi temp zones is not set\n", __func__);
	}

	if (of_find_property(node, "mmi,mmi-ffc-zones", &byte_len)) {
		if ((byte_len / sizeof(struct mmi_ffc_zone)
			!= info->mmi.num_temp_zones)
			|| ((byte_len / sizeof(u32)) % 2)) {
			pr_err("DT error wrong mmi ffc zones\n");
			return -ENODEV;
		}

		info->mmi.ffc_zones = (struct mmi_ffc_zone *)
			devm_kzalloc(dev, byte_len, GFP_KERNEL);

		if (info->mmi.ffc_zones == NULL)
			return -ENOMEM;

		rc = of_property_read_u32_array(node,
				"mmi,mmi-ffc-zones",
				(u32 *)info->mmi.ffc_zones,
				byte_len / sizeof(u32));
		if (rc < 0) {
			pr_err("Couldn't read mmi ffc zones rc = %d\n", rc);
			return rc;
		}

		for (i = 0; i < info->mmi.num_temp_zones; i++) {
			pr_err("FFC:Zone %d,Volt %d,Ich %d", i,
				 info->mmi.ffc_zones[i].ffc_max_mv,
				 info->mmi.ffc_zones[i].ffc_chg_iterm);
		}
	} else
		info->mmi.ffc_zones = NULL;

	rc = of_property_read_u32(node, "mmi,iterm-ma",
				  &info->mmi.chrg_iterm);
	if (rc)
		info->mmi.chrg_iterm = 150;
        info->mmi.back_chrg_iterm = info->mmi.chrg_iterm;

	info->mmi.enable_mux =
		of_property_read_bool(node, "mmi,enable-mux");

	info->mmi.wls_switch_en = of_get_named_gpio(node, "mmi,mux_wls_switch_en", 0);
	if(!gpio_is_valid(info->mmi.wls_switch_en))
		pr_err("mmi wls_switch_en is %d invalid\n", info->mmi.wls_switch_en );

	info->mmi.wls_boost_en = of_get_named_gpio(node, "mmi,mux_wls_boost_en", 0);
	if(!gpio_is_valid(info->mmi.wls_boost_en))
		pr_err("mmi wls_boost_en is %d invalid\n", info->mmi.wls_boost_en);

	info->mmi.wls_control_en = of_get_named_gpio(node, "mmi,mux_wls_control_en", 0);
	if(!gpio_is_valid(info->mmi.wls_control_en))
		pr_err("mmi wls_control_en is %d invalid\n", info->mmi.wls_control_en );
	
	info->mmi.enable_charging_limit =
		of_property_read_bool(node, "mmi,enable-charging-limit");

	rc = of_property_read_u32(node, "mmi,upper-limit-capacity",
				  &info->mmi.upper_limit_capacity);
	if (rc)
		info->mmi.upper_limit_capacity = 100;

	rc = of_property_read_u32(node, "mmi,lower-limit-capacity",
				  &info->mmi.lower_limit_capacity);
	if (rc)
		info->mmi.lower_limit_capacity = 0;

	rc = of_property_read_u32(node, "mmi,vfloat-comp-uv",
				  &info->mmi.vfloat_comp_mv);
	if (rc)
		info->mmi.vfloat_comp_mv = 0;
	info->mmi.vfloat_comp_mv /= 1000;

	return rc;
}

static int chg_reboot(struct notifier_block *nb,
			 unsigned long event, void *unused)
{
	struct charger_manager *info = container_of(nb, struct charger_manager,
						mmi.chg_reboot);
	union power_supply_propval val;
	int rc;

	pr_info("chg Reboot\n");
	if (!info) {
		pr_info("called before chip valid!\n");
		return NOTIFY_DONE;
	}

	if (info->mmi.factory_mode) {
		switch (event) {
		case SYS_POWER_OFF:
			aee_kernel_RT_Monitor_api_factory();
			pinfo->is_suspend = true;
			/* Disable Factory Kill */
			info->disable_charger = true;
			info->chg_tcmd_client.factory_kill_disable = true;
			/* Disable Charging */
			charger_dev_enable(info->chg1_dev, false);
			/* Suspend USB */
			charger_dev_enable_hz(info->chg1_dev, true);

			rc = mmi_get_prop_from_charger(info,
				POWER_SUPPLY_PROP_ONLINE, &val);
			while (rc >= 0 && val.intval) {
				msleep(100);
				rc = mmi_get_prop_from_charger(info,
					POWER_SUPPLY_PROP_ONLINE, &val);
				pr_info("Wait for VBUS to decay\n");
			}

			pr_info("VBUS UV wait 1 sec!\n");
			/* Delay 1 sec to allow more VBUS decay */
			msleep(1000);
			break;
		default:
			break;
		}
	}

	return NOTIFY_DONE;
}

#define CHG_SHOW_MAX_SIZE 50
static ssize_t factory_image_mode_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long r;
	unsigned long mode;

	r = kstrtoul(buf, 0, &mode);
	if (r) {
		pr_err("[%s]Invalid factory image mode value = %lu\n", __func__, mode);
		return -EINVAL;
	}

	if (!mmi_info) {
		pr_err("[%s]mmi_info not valid\n", __func__);
		return -ENODEV;
	}

	mmi_info->mmi.is_factory_image = (mode) ? true : false;

	return r ? r : count;
}

static ssize_t factory_image_mode_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	int state;

	if (!mmi_info) {
		pr_err("[%s]mmi_info not valid\n", __func__);
		return -ENODEV;
	}

	state = (mmi_info->mmi.is_factory_image) ? 1 : 0;

	return scnprintf(buf, CHG_SHOW_MAX_SIZE, "%d\n", state);
}

static DEVICE_ATTR(factory_image_mode, 0644,
		factory_image_mode_show,
		factory_image_mode_store);

static ssize_t factory_charge_upper_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	int state;

	if (!mmi_info) {
		pr_err("[%s]mmi_info not valid\n", __func__);
		return -ENODEV;
	}

	state = mmi_info->mmi.upper_limit_capacity;

	return scnprintf(buf, CHG_SHOW_MAX_SIZE, "%d\n", state);
}

static DEVICE_ATTR(factory_charge_upper, 0444,
		factory_charge_upper_show,
		NULL);

static ssize_t force_demo_mode_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long r;
	unsigned long mode;

	r = kstrtoul(buf, 0, &mode);
	if (r) {
		pr_err("[%s]Invalid demo  mode value = %lu\n", __func__, mode);
		return -EINVAL;
	}

	if (!mmi_info) {
		pr_err("[%s]mmi_info not valid\n", __func__);
		return -ENODEV;
	}
	mmi_info->mmi.chrg_taper_cnt = 0;

	if ((mode >= 35) && (mode <= 80))
		mmi_info->mmi.demo_mode = mode;
	else
		mmi_info->mmi.demo_mode = 35;

	return r ? r : count;
}

static ssize_t force_demo_mode_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	int state;

	if (!mmi_info) {
		pr_err("[%s]mmi_info not valid\n", __func__);
		return -ENODEV;
	}

	state = mmi_info->mmi.demo_mode;

	return scnprintf(buf, CHG_SHOW_MAX_SIZE, "%d\n", state);
}

static DEVICE_ATTR(force_demo_mode, 0644,
		force_demo_mode_show,
		force_demo_mode_store);

static ssize_t force_max_chrg_temp_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long r;
	unsigned long mode;

	r = kstrtoul(buf, 0, &mode);
	if (r) {
		pr_err("[%s]Invalid max temp value = %lu\n", __func__, mode);
		return -EINVAL;
	}

	if (!mmi_info) {
		pr_err("[%s]mmi_info not valid\n", __func__);
		return -ENODEV;
	}

	if ((mode >= MIN_MAX_TEMP_C) && (mode <= MAX_TEMP_C))
		mmi_info->mmi.max_chrg_temp = mode;
	else
		mmi_info->mmi.max_chrg_temp = MAX_TEMP_C;

	return r ? r : count;
}

static ssize_t force_max_chrg_temp_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	int state;

	if (!mmi_info) {
		pr_err("[%s]mmi_info not valid\n", __func__);
		return -ENODEV;
	}

	state = mmi_info->mmi.max_chrg_temp;

	return scnprintf(buf, CHG_SHOW_MAX_SIZE, "%d\n", state);
}

static DEVICE_ATTR(force_max_chrg_temp, 0644,
		force_max_chrg_temp_show,
		force_max_chrg_temp_store);

static int  mtk_charger_tcmd_set_usb_current(void *input, int  val);

void mmi_init(struct charger_manager *info)
{
	int rc;

	if (!info)
		return;

	info->mmi.factory_mode = !strcmp(atm_mode, "enable");

	info->mmi.is_factory_image = false;
	info->mmi.charging_limit_modes = CHARGING_LIMIT_UNKNOWN;
	info->mmi.adaptive_charging_disable_ibat = false;
	info->mmi.adaptive_charging_disable_ichg = false;
	info->mmi.charging_enable_hz = false;
	info->mmi.battery_charging_disable = false;

	rc = parse_mmi_dt(info, &info->pdev->dev);
	if (rc < 0)
		pr_info("[%s]Error getting mmi dt items rc = %d\n",__func__, rc);
	if(gpio_is_valid(info->mmi.wls_switch_en)) {
		rc  = devm_gpio_request_one(&info->pdev->dev, info->mmi.wls_switch_en,
				  GPIOF_OUT_INIT_LOW, "mux_wls_switch_en");
		if (rc  < 0)
			pr_err(" [%s] Failed to request wls_switch_en gpio, ret:%d", __func__, rc);
	}
	if(gpio_is_valid(info->mmi.wls_boost_en)) {
		rc  = devm_gpio_request_one(&info->pdev->dev, info->mmi.wls_boost_en,
				  GPIOF_OUT_INIT_LOW, "mux_wls_boost_en");
		if (rc  < 0)
			pr_err(" [%s] Failed to request wls_boost_en gpio, ret:%d", __func__, rc);
	}
	if(gpio_is_valid(info->mmi.wls_control_en)) {
		rc  = devm_gpio_request_one(&info->pdev->dev, info->mmi.wls_control_en,
				  GPIOF_OUT_INIT_LOW, "mux_wls_control_en");
		if (rc  < 0)
			pr_err(" [%s] Failed to request wls_control_en gpio, ret:%d", __func__, rc);
	}
	info->mmi.chg_reboot.notifier_call = chg_reboot;
	info->mmi.chg_reboot.next = NULL;
	info->mmi.chg_reboot.priority = 1;
	rc = register_reboot_notifier(&info->mmi.chg_reboot);
	if (rc)
		pr_err("SMB register for reboot failed\n");

	rc = device_create_file(&info->pdev->dev,
				&dev_attr_force_demo_mode);
	if (rc) {
		pr_err("[%s]couldn't create force_demo_mode\n", __func__);
	}

	rc = device_create_file(&info->pdev->dev,
				&dev_attr_force_max_chrg_temp);
	if (rc) {
		pr_err("[%s]couldn't create force_max_chrg_temp\n", __func__);
	}

	rc = device_create_file(&info->pdev->dev,
				&dev_attr_factory_image_mode);
	if (rc)
		pr_err("[%s]couldn't create factory_image_mode\n", __func__);

	rc = device_create_file(&info->pdev->dev,
				&dev_attr_factory_charge_upper);
	if (rc)
		pr_err("[%s]couldn't create factory_charge_upper\n", __func__);

	rc = charger_dev_set_eoc_current(info->chg1_dev,
				info->mmi.chrg_iterm*1000);
	if (rc)
		pr_err("[%s]couldn't set eoc current\n", __func__);

	info->mmi.init_done = true;
}

#ifdef MTK_BASE
static void kpoc_power_off_check(struct charger_manager *info)
{
	int vbus = 0;
	struct device *dev = NULL;
	struct device_node *boot_node = NULL;
	struct tag_bootmode *tag = NULL;
	int boot_mode = 11;//UNKNOWN_BOOT
// workaround for mt6768 
	//int boot_mode = get_boot_mode();
	dev = &(info->pdev->dev);
	if (dev != NULL){
		boot_node = of_parse_phandle(dev->of_node, "bootmode", 0);
		if (!boot_node){
			chr_err("%s: failed to get boot mode phandle\n", __func__);
		}
		else {
			tag = (struct tag_bootmode *)of_get_property(boot_node,
								"atag,boot", NULL);
			if (!tag){
				chr_err("%s: failed to get atag,boot\n", __func__);
			}
			else
				boot_mode = tag->bootmode;
		}
	}

	if (boot_mode == KERNEL_POWER_OFF_CHARGING_BOOT
	    || boot_mode == LOW_POWER_OFF_CHARGING_BOOT) {
		if (atomic_read(&info->enable_kpoc_shdn)) {
			vbus = battery_get_vbus();
			if (vbus >= 0 && vbus < 2500 && !mt_charger_plugin()) {
				chr_err("Unplug Charger/USB in KPOC mode, shutdown\n");
				chr_err("%s: system_state=%d\n", __func__,
					system_state);
				if (system_state != SYSTEM_POWER_OFF)
					kernel_power_off();
			}
		}
	}
}
#endif
#ifdef CONFIG_PM
static int charger_pm_event(struct notifier_block *notifier,
			unsigned long pm_event, void *unused)
{
	struct timespec now;

	switch (pm_event) {
	case PM_SUSPEND_PREPARE:
		pinfo->is_suspend = true;
		chr_debug("%s: enter PM_SUSPEND_PREPARE\n", __func__);
		break;
	case PM_POST_SUSPEND:
		pinfo->is_suspend = false;
		chr_debug("%s: enter PM_POST_SUSPEND\n", __func__);
		get_monotonic_boottime(&now);

		if (timespec_compare(&now, &pinfo->endtime) >= 0 &&
			pinfo->endtime.tv_sec != 0 &&
			pinfo->endtime.tv_nsec != 0) {
			chr_err("%s: alarm timeout, wake up charger\n",
				__func__);
			pinfo->endtime.tv_sec = 0;
			pinfo->endtime.tv_nsec = 0;
			_wake_up_charger(pinfo);
		}
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block charger_pm_notifier_func = {
	.notifier_call = charger_pm_event,
	.priority = 0,
};
#endif /* CONFIG_PM */

static enum alarmtimer_restart
	mtk_charger_alarm_timer_func(struct alarm *alarm, ktime_t now)
{
	struct charger_manager *info =
	container_of(alarm, struct charger_manager, charger_timer);
	unsigned long flags;

	if (info->is_suspend == false) {
		chr_err("%s: not suspend, wake up charger\n", __func__);
		_wake_up_charger(info);
	} else {
		chr_err("%s: alarm timer timeout\n", __func__);
		spin_lock_irqsave(&info->slock, flags);
		if (!info->charger_wakelock->active)
			__pm_stay_awake(info->charger_wakelock);
		spin_unlock_irqrestore(&info->slock, flags);
	}

	return ALARMTIMER_NORESTART;
}

static void mtk_charger_start_timer(struct charger_manager *info)
{
	struct timespec time, time_now;
	ktime_t ktime;
	int ret = 0;

	/* If the timer was already set, cancel it */
	ret = alarm_try_to_cancel(&pinfo->charger_timer);
	if (ret < 0) {
		chr_err("%s: callback was running, skip timer\n", __func__);
		return;
	}

	get_monotonic_boottime(&time_now);
	time.tv_sec = info->polling_interval;
	time.tv_nsec = 0;
	info->endtime = timespec_add(time_now, time);

	ktime = ktime_set(info->endtime.tv_sec, info->endtime.tv_nsec);

	chr_err("%s: alarm timer start:%d, %ld %ld\n", __func__, ret,
		info->endtime.tv_sec, info->endtime.tv_nsec);
	alarm_start(&pinfo->charger_timer, ktime);
}

static void mtk_charger_init_timer(struct charger_manager *info)
{
	alarm_init(&info->charger_timer, ALARM_BOOTTIME,
			mtk_charger_alarm_timer_func);
	mtk_charger_start_timer(info);

#ifdef CONFIG_PM
	if (register_pm_notifier(&charger_pm_notifier_func))
		chr_err("%s: register pm failed\n", __func__);
#endif /* CONFIG_PM */
}

static int charger_routine_thread(void *arg)
{
	struct charger_manager *info = arg;
	unsigned long flags = 0;
	bool is_charger_on = false;
	int bat_current = 0, chg_current = 0;
	int ret;

	while (1) {
		ret = wait_event_interruptible(info->wait_que,
			(info->charger_thread_timeout == true));
		if (ret < 0) {
			chr_err("%s: wait event been interrupted(%d)\n", __func__, ret);
			continue;
		}

		mutex_lock(&info->charger_lock);
		spin_lock_irqsave(&info->slock, flags);
		if (!info->charger_wakelock->active)
			__pm_stay_awake(info->charger_wakelock);
		spin_unlock_irqrestore(&info->slock, flags);

		info->charger_thread_timeout = false;
		bat_current = battery_get_bat_current();
		chg_current = pmic_get_charging_current();
		chr_err("Vbat=%d,Ibat=%d,I=%d,VChr=%d,T=%d,Soc=%d:%d,CT:%d:%d hv:%d pd:%d:%d\n",
			battery_get_bat_voltage(), bat_current, chg_current,
			battery_get_vbus(), battery_get_bat_temperature(),
			battery_get_soc(), battery_get_uisoc(),
			mt_get_charger_type(), info->chr_type,
			info->enable_hv_charging, info->pd_type,
			info->pd_reset);

		is_charger_on = mtk_is_charger_on(info);

		if (info->charger_thread_polling == true)
			mtk_charger_start_timer(info);

		charger_update_data(info);
		check_battery_exist(info);
		check_dynamic_mivr(info);
		mmi_charger_check_status(info);
		charger_check_status(info);
		#ifdef MTK_BASE
		kpoc_power_off_check(info);
		#endif
		if (is_disable_charger() == false) {
			if (is_charger_on == true) {
				if (info->do_algorithm)
					info->do_algorithm(info);
				wakeup_sc_algo_cmd(&pinfo->sc.data, SC_EVENT_CHARGING, 0);
			} else
				wakeup_sc_algo_cmd(&pinfo->sc.data, SC_EVENT_STOP_CHARGING, 0);
		} else
			chr_debug("disable charging\n");

		spin_lock_irqsave(&info->slock, flags);
		__pm_relax(info->charger_wakelock);
		spin_unlock_irqrestore(&info->slock, flags);
		chr_debug("%s end , %d\n",
			__func__, info->charger_thread_timeout);
		mutex_unlock(&info->charger_lock);
	}

	return 0;
}

static int mtk_charger_parse_dt(struct charger_manager *info,
				struct device *dev)
{
	struct device_node *np = dev->of_node;
	u32 val;

	chr_err("%s: starts\n", __func__);

	if (!np) {
		chr_err("%s: no device node\n", __func__);
		return -EINVAL;
	}

	if (of_property_read_string(np, "algorithm_name",
		&info->algorithm_name) < 0) {
		chr_err("%s: no algorithm_name name\n", __func__);
		info->algorithm_name = "SwitchCharging";
	}

	if (strcmp(info->algorithm_name, "SwitchCharging") == 0) {
		chr_err("found SwitchCharging\n");
		mtk_switch_charging_init(info);
	}
#ifdef CONFIG_MTK_DUAL_CHARGER_SUPPORT
	if (strcmp(info->algorithm_name, "DualSwitchCharging") == 0) {
		pr_debug("found DualSwitchCharging\n");
		mtk_dual_switch_charging_init(info);
	}
#endif

	info->disable_charger = of_property_read_bool(np, "disable_charger");
	info->enable_sw_safety_timer =
			of_property_read_bool(np, "enable_sw_safety_timer");
	info->sw_safety_timer_setting = info->enable_sw_safety_timer;
	info->enable_sw_jeita = of_property_read_bool(np, "enable_sw_jeita");
	info->enable_pe_plus = of_property_read_bool(np, "enable_pe_plus");
	info->enable_pe_2 = of_property_read_bool(np, "enable_pe_2");
	info->enable_wlc = of_property_read_bool(np, "enable_wlc");
	info->enable_pe_4 = of_property_read_bool(np, "enable_pe_4");
	info->enable_pe_5 = of_property_read_bool(np, "enable_pe_5");
	info->enable_cp = of_property_read_bool(np, "enable_cp");
	info->enable_type_c = of_property_read_bool(np, "enable_type_c");
	info->enable_dynamic_mivr =
			of_property_read_bool(np, "enable_dynamic_mivr");
	info->disable_pd_dual = of_property_read_bool(np, "disable_pd_dual");

	info->enable_hv_charging = true;

	/* common */
	if (of_property_read_u32(np, "battery_cv", &val) >= 0)
		info->data.battery_cv = val;
	else {
		chr_err("use default BATTERY_CV:%d\n", BATTERY_CV);
		info->data.battery_cv = BATTERY_CV;
	}

	if (of_property_read_u32(np, "max_charger_voltage", &val) >= 0)
		info->data.max_charger_voltage = val;
	else {
		chr_err("use default V_CHARGER_MAX:%d\n", V_CHARGER_MAX);
		info->data.max_charger_voltage = V_CHARGER_MAX;
	}
	info->data.max_charger_voltage_setting = info->data.max_charger_voltage;

	if (of_property_read_u32(np, "min_charger_voltage", &val) >= 0)
		info->data.min_charger_voltage = val;
	else {
		chr_err("use default V_CHARGER_MIN:%d\n", V_CHARGER_MIN);
		info->data.min_charger_voltage = V_CHARGER_MIN;
	}

	/* dynamic mivr */
	if (of_property_read_u32(np, "min_charger_voltage_1", &val) >= 0)
		info->data.min_charger_voltage_1 = val;
	else {
		chr_err("use default V_CHARGER_MIN_1:%d\n", V_CHARGER_MIN_1);
		info->data.min_charger_voltage_1 = V_CHARGER_MIN_1;
	}

	if (of_property_read_u32(np, "min_charger_voltage_2", &val) >= 0)
		info->data.min_charger_voltage_2 = val;
	else {
		chr_err("use default V_CHARGER_MIN_2:%d\n", V_CHARGER_MIN_2);
		info->data.min_charger_voltage_2 = V_CHARGER_MIN_2;
	}

	if (of_property_read_u32(np, "max_dmivr_charger_current", &val) >= 0)
		info->data.max_dmivr_charger_current = val;
	else {
		chr_err("use default MAX_DMIVR_CHARGER_CURRENT:%d\n",
			MAX_DMIVR_CHARGER_CURRENT);
		info->data.max_dmivr_charger_current =
					MAX_DMIVR_CHARGER_CURRENT;
	}

	/* charging current */
	if (of_property_read_u32(np, "usb_charger_current_suspend", &val) >= 0)
		info->data.usb_charger_current_suspend = val;
	else {
		chr_err("use default USB_CHARGER_CURRENT_SUSPEND:%d\n",
			USB_CHARGER_CURRENT_SUSPEND);
		info->data.usb_charger_current_suspend =
						USB_CHARGER_CURRENT_SUSPEND;
	}

	if (of_property_read_u32(np, "usb_charger_current_unconfigured", &val)
		>= 0) {
		info->data.usb_charger_current_unconfigured = val;
	} else {
		chr_err("use default USB_CHARGER_CURRENT_UNCONFIGURED:%d\n",
			USB_CHARGER_CURRENT_UNCONFIGURED);
		info->data.usb_charger_current_unconfigured =
					USB_CHARGER_CURRENT_UNCONFIGURED;
	}

	if (of_property_read_u32(np, "usb_charger_current_configured", &val)
		>= 0) {
		info->data.usb_charger_current_configured = val;
	} else {
		chr_err("use default USB_CHARGER_CURRENT_CONFIGURED:%d\n",
			USB_CHARGER_CURRENT_CONFIGURED);
		info->data.usb_charger_current_configured =
					USB_CHARGER_CURRENT_CONFIGURED;
	}

	if (of_property_read_u32(np, "usb_charger_current", &val) >= 0) {
		info->data.usb_charger_current = val;
	} else {
		chr_err("use default USB_CHARGER_CURRENT:%d\n",
			USB_CHARGER_CURRENT);
		info->data.usb_charger_current = USB_CHARGER_CURRENT;
	}

	if (of_property_read_u32(np, "ac_charger_current", &val) >= 0) {
		info->data.ac_charger_current = val;
	} else {
		chr_err("use default AC_CHARGER_CURRENT:%d\n",
			AC_CHARGER_CURRENT);
		info->data.ac_charger_current = AC_CHARGER_CURRENT;
	}

	info->data.pd_charger_current = 3000000;

	if (of_property_read_u32(np, "ac_charger_input_current", &val) >= 0) {

		info->data.ac_charger_input_current = val;
#ifdef CONFIG_MOTO_CHG_FFC_5V10W_SUPPORT
		info->ffc_input_current_backup = val;
#endif
	}
	else {
		chr_err("use default AC_CHARGER_INPUT_CURRENT:%d\n",
			AC_CHARGER_INPUT_CURRENT);
		info->data.ac_charger_input_current = AC_CHARGER_INPUT_CURRENT;
#ifdef CONFIG_MOTO_CHG_FFC_5V10W_SUPPORT
		info->ffc_input_current_backup = AC_CHARGER_INPUT_CURRENT;
#endif
	}

	if (of_property_read_u32(np, "non_std_ac_charger_current", &val) >= 0)
		info->data.non_std_ac_charger_current = val;
	else {
		chr_err("use default NON_STD_AC_CHARGER_CURRENT:%d\n",
			NON_STD_AC_CHARGER_CURRENT);
		info->data.non_std_ac_charger_current =
					NON_STD_AC_CHARGER_CURRENT;
	}

	if (of_property_read_u32(np, "charging_host_charger_current", &val)
		>= 0) {
		info->data.charging_host_charger_current = val;
	} else {
		chr_err("use default CHARGING_HOST_CHARGER_CURRENT:%d\n",
			CHARGING_HOST_CHARGER_CURRENT);
		info->data.charging_host_charger_current =
					CHARGING_HOST_CHARGER_CURRENT;
	}

	if (of_property_read_u32(np, "apple_1_0a_charger_current", &val) >= 0)
		info->data.apple_1_0a_charger_current = val;
	else {
		chr_err("use default APPLE_1_0A_CHARGER_CURRENT:%d\n",
			APPLE_1_0A_CHARGER_CURRENT);
		info->data.apple_1_0a_charger_current =
					APPLE_1_0A_CHARGER_CURRENT;
	}

	if (of_property_read_u32(np, "apple_2_1a_charger_current", &val) >= 0)
		info->data.apple_2_1a_charger_current = val;
	else {
		chr_err("use default APPLE_2_1A_CHARGER_CURRENT:%d\n",
			APPLE_2_1A_CHARGER_CURRENT);
		info->data.apple_2_1a_charger_current =
					APPLE_2_1A_CHARGER_CURRENT;
	}

	if (of_property_read_u32(np, "ta_ac_charger_current", &val) >= 0)
		info->data.ta_ac_charger_current = val;
	else {
		chr_err("use default TA_AC_CHARGING_CURRENT:%d\n",
			TA_AC_CHARGING_CURRENT);
		info->data.ta_ac_charger_current =
					TA_AC_CHARGING_CURRENT;
	}

	if (of_property_read_u32(np, "usb_unlimited_current", &val) >= 0)
		info->data.usb_unlimited_current = val;
	else {
		chr_err("use default usb_unlimited_current:%d\n",
			USB_UNLIMITED_CURRENT);
		info->data.usb_unlimited_current =
					USB_UNLIMITED_CURRENT;
	}

	/* sw jeita */
	if (of_property_read_u32(np, "jeita_temp_above_t4_cv", &val) >= 0)
		info->data.jeita_temp_above_t4_cv = val;
	else {
		chr_err("use default JEITA_TEMP_ABOVE_T4_CV:%d\n",
			JEITA_TEMP_ABOVE_T4_CV);
		info->data.jeita_temp_above_t4_cv = JEITA_TEMP_ABOVE_T4_CV;
	}

	if (of_property_read_u32(np, "jeita_temp_t3_to_t4_cv", &val) >= 0)
		info->data.jeita_temp_t3_to_t4_cv = val;
	else {
		chr_err("use default JEITA_TEMP_T3_TO_T4_CV:%d\n",
			JEITA_TEMP_T3_TO_T4_CV);
		info->data.jeita_temp_t3_to_t4_cv = JEITA_TEMP_T3_TO_T4_CV;
	}

	if (of_property_read_u32(np, "jeita_temp_t2_to_t3_cv", &val) >= 0)
		info->data.jeita_temp_t2_to_t3_cv = val;
	else {
		chr_err("use default JEITA_TEMP_T2_TO_T3_CV:%d\n",
			JEITA_TEMP_T2_TO_T3_CV);
		info->data.jeita_temp_t2_to_t3_cv = JEITA_TEMP_T2_TO_T3_CV;
	}

	if (of_property_read_u32(np, "jeita_temp_t1_to_t2_cv", &val) >= 0)
		info->data.jeita_temp_t1_to_t2_cv = val;
	else {
		chr_err("use default JEITA_TEMP_T1_TO_T2_CV:%d\n",
			JEITA_TEMP_T1_TO_T2_CV);
		info->data.jeita_temp_t1_to_t2_cv = JEITA_TEMP_T1_TO_T2_CV;
	}

	if (of_property_read_u32(np, "jeita_temp_t0_to_t1_cv", &val) >= 0)
		info->data.jeita_temp_t0_to_t1_cv = val;
	else {
		chr_err("use default JEITA_TEMP_T0_TO_T1_CV:%d\n",
			JEITA_TEMP_T0_TO_T1_CV);
		info->data.jeita_temp_t0_to_t1_cv = JEITA_TEMP_T0_TO_T1_CV;
	}

	if (of_property_read_u32(np, "jeita_temp_below_t0_cv", &val) >= 0)
		info->data.jeita_temp_below_t0_cv = val;
	else {
		chr_err("use default JEITA_TEMP_BELOW_T0_CV:%d\n",
			JEITA_TEMP_BELOW_T0_CV);
		info->data.jeita_temp_below_t0_cv = JEITA_TEMP_BELOW_T0_CV;
	}

	if (of_property_read_u32(np, "temp_t4_thres", &val) >= 0)
		info->data.temp_t4_thres = val;
	else {
		chr_err("use default TEMP_T4_THRES:%d\n",
			TEMP_T4_THRES);
		info->data.temp_t4_thres = TEMP_T4_THRES;
	}

	if (of_property_read_u32(np, "temp_t4_thres_minus_x_degree", &val) >= 0)
		info->data.temp_t4_thres_minus_x_degree = val;
	else {
		chr_err("use default TEMP_T4_THRES_MINUS_X_DEGREE:%d\n",
			TEMP_T4_THRES_MINUS_X_DEGREE);
		info->data.temp_t4_thres_minus_x_degree =
					TEMP_T4_THRES_MINUS_X_DEGREE;
	}

	if (of_property_read_u32(np, "temp_t3_thres", &val) >= 0)
		info->data.temp_t3_thres = val;
	else {
		chr_err("use default TEMP_T3_THRES:%d\n",
			TEMP_T3_THRES);
		info->data.temp_t3_thres = TEMP_T3_THRES;
	}

	if (of_property_read_u32(np, "temp_t3_thres_minus_x_degree", &val) >= 0)
		info->data.temp_t3_thres_minus_x_degree = val;
	else {
		chr_err("use default TEMP_T3_THRES_MINUS_X_DEGREE:%d\n",
			TEMP_T3_THRES_MINUS_X_DEGREE);
		info->data.temp_t3_thres_minus_x_degree =
					TEMP_T3_THRES_MINUS_X_DEGREE;
	}

	if (of_property_read_u32(np, "temp_t2_thres", &val) >= 0)
		info->data.temp_t2_thres = val;
	else {
		chr_err("use default TEMP_T2_THRES:%d\n",
			TEMP_T2_THRES);
		info->data.temp_t2_thres = TEMP_T2_THRES;
	}

	if (of_property_read_u32(np, "temp_t2_thres_plus_x_degree", &val) >= 0)
		info->data.temp_t2_thres_plus_x_degree = val;
	else {
		chr_err("use default TEMP_T2_THRES_PLUS_X_DEGREE:%d\n",
			TEMP_T2_THRES_PLUS_X_DEGREE);
		info->data.temp_t2_thres_plus_x_degree =
					TEMP_T2_THRES_PLUS_X_DEGREE;
	}

	if (of_property_read_u32(np, "temp_t1_thres", &val) >= 0)
		info->data.temp_t1_thres = val;
	else {
		chr_err("use default TEMP_T1_THRES:%d\n",
			TEMP_T1_THRES);
		info->data.temp_t1_thres = TEMP_T1_THRES;
	}

	if (of_property_read_u32(np, "temp_t1_thres_plus_x_degree", &val) >= 0)
		info->data.temp_t1_thres_plus_x_degree = val;
	else {
		chr_err("use default TEMP_T1_THRES_PLUS_X_DEGREE:%d\n",
			TEMP_T1_THRES_PLUS_X_DEGREE);
		info->data.temp_t1_thres_plus_x_degree =
					TEMP_T1_THRES_PLUS_X_DEGREE;
	}

	if (of_property_read_u32(np, "temp_t0_thres", &val) >= 0)
		info->data.temp_t0_thres = val;
	else {
		chr_err("use default TEMP_T0_THRES:%d\n",
			TEMP_T0_THRES);
		info->data.temp_t0_thres = TEMP_T0_THRES;
	}

	if (of_property_read_u32(np, "temp_t0_thres_plus_x_degree", &val) >= 0)
		info->data.temp_t0_thres_plus_x_degree = val;
	else {
		chr_err("use default TEMP_T0_THRES_PLUS_X_DEGREE:%d\n",
			TEMP_T0_THRES_PLUS_X_DEGREE);
		info->data.temp_t0_thres_plus_x_degree =
					TEMP_T0_THRES_PLUS_X_DEGREE;
	}

	if (of_property_read_u32(np, "temp_neg_10_thres", &val) >= 0)
		info->data.temp_neg_10_thres = val;
	else {
		chr_err("use default TEMP_NEG_10_THRES:%d\n",
			TEMP_NEG_10_THRES);
		info->data.temp_neg_10_thres = TEMP_NEG_10_THRES;
	}

	/* battery temperature protection */
	info->thermal.sm = BAT_TEMP_NORMAL;
	info->thermal.enable_min_charge_temp =
		of_property_read_bool(np, "enable_min_charge_temp");

	if (of_property_read_u32(np, "min_charge_temp", &val) >= 0)
		info->thermal.min_charge_temp = val;
	else {
		chr_err("use default MIN_CHARGE_TEMP:%d\n",
			MIN_CHARGE_TEMP);
		info->thermal.min_charge_temp = MIN_CHARGE_TEMP;
	}

	if (of_property_read_u32(np, "min_charge_temp_plus_x_degree", &val)
	    >= 0) {
		info->thermal.min_charge_temp_plus_x_degree = val;
	} else {
		chr_err("use default MIN_CHARGE_TEMP_PLUS_X_DEGREE:%d\n",
			MIN_CHARGE_TEMP_PLUS_X_DEGREE);
		info->thermal.min_charge_temp_plus_x_degree =
					MIN_CHARGE_TEMP_PLUS_X_DEGREE;
	}

	if (of_property_read_u32(np, "max_charge_temp", &val) >= 0)
		info->thermal.max_charge_temp = val;
	else {
		chr_err("use default MAX_CHARGE_TEMP:%d\n",
			MAX_CHARGE_TEMP);
		info->thermal.max_charge_temp = MAX_CHARGE_TEMP;
	}

	if (of_property_read_u32(np, "max_charge_temp_minus_x_degree", &val)
	    >= 0) {
		info->thermal.max_charge_temp_minus_x_degree = val;
	} else {
		chr_err("use default MAX_CHARGE_TEMP_MINUS_X_DEGREE:%d\n",
			MAX_CHARGE_TEMP_MINUS_X_DEGREE);
		info->thermal.max_charge_temp_minus_x_degree =
					MAX_CHARGE_TEMP_MINUS_X_DEGREE;
	}

	/* PE */
	info->data.ta_12v_support = of_property_read_bool(np, "ta_12v_support");
	info->data.ta_9v_support = of_property_read_bool(np, "ta_9v_support");

	if (of_property_read_u32(np, "pe_ichg_level_threshold", &val) >= 0)
		info->data.pe_ichg_level_threshold = val;
	else {
		chr_err("use default PE_ICHG_LEAVE_THRESHOLD:%d\n",
			PE_ICHG_LEAVE_THRESHOLD);
		info->data.pe_ichg_level_threshold = PE_ICHG_LEAVE_THRESHOLD;
	}

	if (of_property_read_u32(np, "ta_ac_12v_input_current", &val) >= 0)
		info->data.ta_ac_12v_input_current = val;
	else {
		chr_err("use default TA_AC_12V_INPUT_CURRENT:%d\n",
			TA_AC_12V_INPUT_CURRENT);
		info->data.ta_ac_12v_input_current = TA_AC_12V_INPUT_CURRENT;
	}

	if (of_property_read_u32(np, "ta_ac_9v_input_current", &val) >= 0)
		info->data.ta_ac_9v_input_current = val;
	else {
		chr_err("use default TA_AC_9V_INPUT_CURRENT:%d\n",
			TA_AC_9V_INPUT_CURRENT);
		info->data.ta_ac_9v_input_current = TA_AC_9V_INPUT_CURRENT;
	}

	if (of_property_read_u32(np, "ta_ac_7v_input_current", &val) >= 0)
		info->data.ta_ac_7v_input_current = val;
	else {
		chr_err("use default TA_AC_7V_INPUT_CURRENT:%d\n",
			TA_AC_7V_INPUT_CURRENT);
		info->data.ta_ac_7v_input_current = TA_AC_7V_INPUT_CURRENT;
	}

	/* PE 2.0 */
	if (of_property_read_u32(np, "pe20_ichg_level_threshold", &val) >= 0)
		info->data.pe20_ichg_level_threshold = val;
	else {
		chr_err("use default PE20_ICHG_LEAVE_THRESHOLD:%d\n",
			PE20_ICHG_LEAVE_THRESHOLD);
		info->data.pe20_ichg_level_threshold =
						PE20_ICHG_LEAVE_THRESHOLD;
	}

	if (of_property_read_u32(np, "ta_start_battery_soc", &val) >= 0)
		info->data.ta_start_battery_soc = val;
	else {
		chr_err("use default TA_START_BATTERY_SOC:%d\n",
			TA_START_BATTERY_SOC);
		info->data.ta_start_battery_soc = TA_START_BATTERY_SOC;
	}

	if (of_property_read_u32(np, "ta_stop_battery_soc", &val) >= 0)
		info->data.ta_stop_battery_soc = val;
	else {
		chr_err("use default TA_STOP_BATTERY_SOC:%d\n",
			TA_STOP_BATTERY_SOC);
		info->data.ta_stop_battery_soc = TA_STOP_BATTERY_SOC;
	}

	/* PE 4.0 */
	if (of_property_read_u32(np, "high_temp_to_leave_pe40", &val) >= 0) {
		info->data.high_temp_to_leave_pe40 = val;
	} else {
		chr_err("use default high_temp_to_leave_pe40:%d\n",
			HIGH_TEMP_TO_LEAVE_PE40);
		info->data.high_temp_to_leave_pe40 = HIGH_TEMP_TO_LEAVE_PE40;
	}

	if (of_property_read_u32(np, "high_temp_to_enter_pe40", &val) >= 0) {
		info->data.high_temp_to_enter_pe40 = val;
	} else {
		chr_err("use default high_temp_to_enter_pe40:%d\n",
			HIGH_TEMP_TO_ENTER_PE40);
		info->data.high_temp_to_enter_pe40 = HIGH_TEMP_TO_ENTER_PE40;
	}

	if (of_property_read_u32(np, "low_temp_to_leave_pe40", &val) >= 0) {
		info->data.low_temp_to_leave_pe40 = val;
	} else {
		chr_err("use default low_temp_to_leave_pe40:%d\n",
			LOW_TEMP_TO_LEAVE_PE40);
		info->data.low_temp_to_leave_pe40 = LOW_TEMP_TO_LEAVE_PE40;
	}

	if (of_property_read_u32(np, "low_temp_to_enter_pe40", &val) >= 0) {
		info->data.low_temp_to_enter_pe40 = val;
	} else {
		chr_err("use default low_temp_to_enter_pe40:%d\n",
			LOW_TEMP_TO_ENTER_PE40);
		info->data.low_temp_to_enter_pe40 = LOW_TEMP_TO_ENTER_PE40;
	}

	/* PE 4.0 single */
	if (of_property_read_u32(np,
		"pe40_single_charger_input_current", &val) >= 0) {
		info->data.pe40_single_charger_input_current = val;
	} else {
		chr_err("use default pe40_single_charger_input_current:%d\n",
			3000);
		info->data.pe40_single_charger_input_current = 3000;
	}

	if (of_property_read_u32(np, "pe40_single_charger_current", &val)
	    >= 0) {
		info->data.pe40_single_charger_current = val;
	} else {
		chr_err("use default pe40_single_charger_current:%d\n", 3000);
		info->data.pe40_single_charger_current = 3000;
	}

	/* PE 4.0 dual */
	if (of_property_read_u32(np, "pe40_dual_charger_input_current", &val)
	    >= 0) {
		info->data.pe40_dual_charger_input_current = val;
	} else {
		chr_err("use default pe40_dual_charger_input_current:%d\n",
			3000);
		info->data.pe40_dual_charger_input_current = 3000;
	}

	if (of_property_read_u32(np, "pe40_dual_charger_chg1_current", &val)
	    >= 0) {
		info->data.pe40_dual_charger_chg1_current = val;
	} else {
		chr_err("use default pe40_dual_charger_chg1_current:%d\n",
			2000);
		info->data.pe40_dual_charger_chg1_current = 2000;
	}

	if (of_property_read_u32(np, "pe40_dual_charger_chg2_current", &val)
	    >= 0) {
		info->data.pe40_dual_charger_chg2_current = val;
	} else {
		chr_err("use default pe40_dual_charger_chg2_current:%d\n",
			2000);
		info->data.pe40_dual_charger_chg2_current = 2000;
	}

	if (of_property_read_u32(np, "dual_polling_ieoc", &val) >= 0)
		info->data.dual_polling_ieoc = val;
	else {
		chr_err("use default dual_polling_ieoc :%d\n", 750000);
		info->data.dual_polling_ieoc = 750000;
	}

	if (of_property_read_u32(np, "pe40_stop_battery_soc", &val) >= 0)
		info->data.pe40_stop_battery_soc = val;
	else {
		chr_err("use default pe40_stop_battery_soc:%d\n", 80);
		info->data.pe40_stop_battery_soc = 80;
	}

	if (of_property_read_u32(np, "pe40_max_vbus", &val) >= 0)
		info->data.pe40_max_vbus = val;
	else {
		chr_err("use default pe40_max_vbus:%d\n", PE40_MAX_VBUS);
		info->data.pe40_max_vbus = PE40_MAX_VBUS;
	}

	if (of_property_read_u32(np, "pe40_max_ibus", &val) >= 0)
		info->data.pe40_max_ibus = val;
	else {
		chr_err("use default pe40_max_ibus:%d\n", PE40_MAX_IBUS);
		info->data.pe40_max_ibus = PE40_MAX_IBUS;
	}

	/* PE 4.0 cable impedance (mohm) */
	if (of_property_read_u32(np, "pe40_r_cable_1a_lower", &val) >= 0)
		info->data.pe40_r_cable_1a_lower = val;
	else {
		chr_err("use default pe40_r_cable_1a_lower:%d\n", 530);
		info->data.pe40_r_cable_1a_lower = 530;
	}

	if (of_property_read_u32(np, "pe40_r_cable_2a_lower", &val) >= 0)
		info->data.pe40_r_cable_2a_lower = val;
	else {
		chr_err("use default pe40_r_cable_2a_lower:%d\n", 390);
		info->data.pe40_r_cable_2a_lower = 390;
	}

	if (of_property_read_u32(np, "pe40_r_cable_3a_lower", &val) >= 0)
		info->data.pe40_r_cable_3a_lower = val;
	else {
		chr_err("use default pe40_r_cable_3a_lower:%d\n", 252);
		info->data.pe40_r_cable_3a_lower = 252;
	}

	/* PD */
	if (of_property_read_u32(np, "pd_vbus_upper_bound", &val) >= 0) {
		info->data.pd_vbus_upper_bound = val;
	} else {
		chr_err("use default pd_vbus_upper_bound:%d\n",
			PD_VBUS_UPPER_BOUND);
		info->data.pd_vbus_upper_bound = PD_VBUS_UPPER_BOUND;
	}

	if (of_property_read_u32(np, "pd_vbus_low_bound", &val) >= 0) {
		info->data.pd_vbus_low_bound = val;
	} else {
		chr_err("use default pd_vbus_low_bound:%d\n",
			PD_VBUS_LOW_BOUND);
		info->data.pd_vbus_low_bound = PD_VBUS_LOW_BOUND;
	}

	if (of_property_read_u32(np, "pd_ichg_level_threshold", &val) >= 0)
		info->data.pd_ichg_level_threshold = val;
	else {
		chr_err("use default pd_ichg_level_threshold:%d\n",
			PD_ICHG_LEAVE_THRESHOLD);
		info->data.pd_ichg_level_threshold = PD_ICHG_LEAVE_THRESHOLD;
	}

	if (of_property_read_u32(np, "pd_stop_battery_soc", &val) >= 0)
		info->data.pd_stop_battery_soc = val;
	else {
		chr_err("use default pd_stop_battery_soc:%d\n",
			PD_STOP_BATTERY_SOC);
		info->data.pd_stop_battery_soc = PD_STOP_BATTERY_SOC;
	}

	if (of_property_read_u32(np, "vsys_watt", &val) >= 0) {
		info->data.vsys_watt = val;
	} else {
		chr_err("use default vsys_watt:%d\n",
			VSYS_WATT);
		info->data.vsys_watt = VSYS_WATT;
	}

	if (of_property_read_u32(np, "ibus_err", &val) >= 0) {
		info->data.ibus_err = val;
	} else {
		chr_err("use default ibus_err:%d\n",
			IBUS_ERR);
		info->data.ibus_err = IBUS_ERR;
	}

	/* dual charger */
	if (of_property_read_u32(np, "chg1_ta_ac_charger_current", &val) >= 0)
		info->data.chg1_ta_ac_charger_current = val;
	else {
		chr_err("use default TA_AC_MASTER_CHARGING_CURRENT:%d\n",
			TA_AC_MASTER_CHARGING_CURRENT);
		info->data.chg1_ta_ac_charger_current =
						TA_AC_MASTER_CHARGING_CURRENT;
	}

	if (of_property_read_u32(np, "chg2_ta_ac_charger_current", &val) >= 0)
		info->data.chg2_ta_ac_charger_current = val;
	else {
		chr_err("use default TA_AC_SLAVE_CHARGING_CURRENT:%d\n",
			TA_AC_SLAVE_CHARGING_CURRENT);
		info->data.chg2_ta_ac_charger_current =
						TA_AC_SLAVE_CHARGING_CURRENT;
	}

	if (of_property_read_u32(np, "slave_mivr_diff", &val) >= 0)
		info->data.slave_mivr_diff = val;
	else {
		chr_err("use default SLAVE_MIVR_DIFF:%d\n", SLAVE_MIVR_DIFF);
		info->data.slave_mivr_diff = SLAVE_MIVR_DIFF;
	}

	/* slave charger */
	if (of_property_read_u32(np, "chg2_eff", &val) >= 0)
		info->data.chg2_eff = val;
	else {
		chr_err("use default CHG2_EFF:%d\n", CHG2_EFF);
		info->data.chg2_eff = CHG2_EFF;
	}

	info->data.parallel_vbus = of_property_read_bool(np, "parallel_vbus");

	/* cable measurement impedance */
	if (of_property_read_u32(np, "cable_imp_threshold", &val) >= 0)
		info->data.cable_imp_threshold = val;
	else {
		chr_err("use default CABLE_IMP_THRESHOLD:%d\n",
			CABLE_IMP_THRESHOLD);
		info->data.cable_imp_threshold = CABLE_IMP_THRESHOLD;
	}

	if (of_property_read_u32(np, "vbat_cable_imp_threshold", &val) >= 0)
		info->data.vbat_cable_imp_threshold = val;
	else {
		chr_err("use default VBAT_CABLE_IMP_THRESHOLD:%d\n",
			VBAT_CABLE_IMP_THRESHOLD);
		info->data.vbat_cable_imp_threshold = VBAT_CABLE_IMP_THRESHOLD;
	}

	/* BIF */
	if (of_property_read_u32(np, "bif_threshold1", &val) >= 0)
		info->data.bif_threshold1 = val;
	else {
		chr_err("use default BIF_THRESHOLD1:%d\n",
			BIF_THRESHOLD1);
		info->data.bif_threshold1 = BIF_THRESHOLD1;
	}

	if (of_property_read_u32(np, "bif_threshold2", &val) >= 0)
		info->data.bif_threshold2 = val;
	else {
		chr_err("use default BIF_THRESHOLD2:%d\n",
			BIF_THRESHOLD2);
		info->data.bif_threshold2 = BIF_THRESHOLD2;
	}

	if (of_property_read_u32(np, "bif_cv_under_threshold2", &val) >= 0)
		info->data.bif_cv_under_threshold2 = val;
	else {
		chr_err("use default BIF_CV_UNDER_THRESHOLD2:%d\n",
			BIF_CV_UNDER_THRESHOLD2);
		info->data.bif_cv_under_threshold2 = BIF_CV_UNDER_THRESHOLD2;
	}

	info->data.power_path_support =
				of_property_read_bool(np, "power_path_support");
	chr_debug("%s: power_path_support: %d\n",
		__func__, info->data.power_path_support);

	if (of_property_read_u32(np, "max_charging_time", &val) >= 0)
		info->data.max_charging_time = val;
	else {
		chr_err("use default MAX_CHARGING_TIME:%d\n",
			MAX_CHARGING_TIME);
		info->data.max_charging_time = MAX_CHARGING_TIME;
	}


	if (of_property_read_u32(np, "bc12_charger", &val) >= 0)
		info->data.bc12_charger = val;
	else {
		chr_err("use default BC12_CHARGER:%d\n",
			DEFAULT_BC12_CHARGER);
		info->data.bc12_charger = DEFAULT_BC12_CHARGER;
	}
	if (of_property_read_u32(np, "typec_limit_aicr", &val) >= 0) {
		info->chg1_data.typec_input_current_limit = val;
		chr_debug("%s: typec_input_current_limit: %d\n", __func__,
			info->chg1_data.typec_input_current_limit);
	} else {
		chr_err("Dont limit type C aicr\n");
		info->chg1_data.typec_input_current_limit = -1;
	}

	if (strcmp(info->algorithm_name, "SwitchCharging2") == 0) {
		chr_err("found SwitchCharging2\n");
		mtk_switch_charging_init2(info);
	}

	if (of_property_read_u32(np, "sc_battery_size", &val) >= 0)
		info->sc.battery_size = val;
	else {
		chr_err("use default sc_battery_size:%d\n",
			SC_BATTERY_SIZE);
		info->sc.battery_size = SC_BATTERY_SIZE;
	}

	if (of_property_read_u32(np, "sc_cv_time", &val) >= 0)
		info->sc.left_time_for_cv = val;
	else {
		chr_err("use default sc_cv_time:%d\n",
			SC_CV_TIME);
		info->sc.left_time_for_cv = SC_CV_TIME;
	}

	if (of_property_read_u32(np, "sc_current_limit", &val) >= 0)
		info->sc.current_limit = val;
	else {
		chr_err("use default sc_current_limit:%d\n",
			SC_CURRENT_LIMIT);
		info->sc.current_limit = SC_CURRENT_LIMIT;
	}
	if (of_property_read_u32(np, "wireless_factory_max_current", &val) >= 0) {
		info->data.wireless_factory_max_current = val;
	} else {
		chr_err("use default WIRELESS_FACTORY_MAX_CURRENT:%d\n",
			WIRELESS_FACTORY_MAX_CURRENT);
		info->data.wireless_factory_max_current = WIRELESS_FACTORY_MAX_CURRENT;
	}

	if (of_property_read_u32(np, "wireless_factory_max_input_current", &val) >= 0) {
		info->data.wireless_factory_max_input_current = val;
	} else {
		chr_err("use default WIRELESS_FACTORY_MAX_INPUT_CURRENT:%d\n",
			WIRELESS_FACTORY_MAX_INPUT_CURRENT);
		info->data.wireless_factory_max_input_current = WIRELESS_FACTORY_MAX_INPUT_CURRENT;
	}
	chr_err("algorithm name:%s\n", info->algorithm_name);

	return 0;
}


static ssize_t show_Pump_Express(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct charger_manager *pinfo = dev->driver_data;
	int is_ta_detected = 0;

	pr_debug("[%s] chr_type:%d UISOC:%d startsoc:%d stopsoc:%d\n", __func__,
		mt_get_charger_type(), battery_get_uisoc(),
		pinfo->data.ta_start_battery_soc,
		pinfo->data.ta_stop_battery_soc);

	if (IS_ENABLED(CONFIG_MTK_PUMP_EXPRESS_PLUS_20_SUPPORT)) {
		/* Is PE+20 connect */
		if (mtk_pe20_get_is_connect(pinfo))
			is_ta_detected = 1;
	}

	if (IS_ENABLED(CONFIG_MOTO_WLC_ALG_SUPPORT)) {
		/* Is PE+20 connect */
		if (wlc_get_online())
			is_ta_detected = 1;
	}

	if (IS_ENABLED(CONFIG_MTK_PUMP_EXPRESS_PLUS_SUPPORT)) {
		/* Is PE+ connect */
		if (mtk_pe_get_is_connect(pinfo))
			is_ta_detected = 1;
	}

	if (mtk_is_TA_support_pd_pps(pinfo) == true || pinfo->is_pdc_run == true)
		is_ta_detected = 1;

	pr_debug("%s: detected = %d, pe20_connect = %d, pe_connect = %d,wlc_connect:%d\n",
		__func__, is_ta_detected,
		mtk_pe20_get_is_connect(pinfo),
		mtk_pe_get_is_connect(pinfo),
		wlc_get_online());

	return sprintf(buf, "%u\n", is_ta_detected);
}

static DEVICE_ATTR(Pump_Express, 0444, show_Pump_Express, NULL);

static ssize_t show_input_current(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct charger_manager *pinfo = dev->driver_data;

	pr_debug("[Battery] %s : %x\n",
		__func__, pinfo->chg1_data.thermal_input_current_limit);
	return sprintf(buf, "%u\n",
			pinfo->chg1_data.thermal_input_current_limit);
}

static ssize_t store_input_current(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct charger_manager *pinfo = dev->driver_data;
	unsigned int reg = 0;
	int ret;

	pr_debug("[Battery] %s\n", __func__);
	if (buf != NULL && size != 0) {
		pr_debug("[Battery] buf is %s and size is %zu\n", buf, size);
		ret = kstrtouint(buf, 16, &reg);
		pinfo->chg1_data.thermal_input_current_limit = reg;
		if (pinfo->data.parallel_vbus)
			pinfo->chg2_data.thermal_input_current_limit = reg;
		pr_debug("[Battery] %s: %x\n",
			__func__, pinfo->chg1_data.thermal_input_current_limit);
	}
	return size;
}
static DEVICE_ATTR(input_current, 0644, show_input_current,
		store_input_current);

static ssize_t show_chg1_current(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct charger_manager *pinfo = dev->driver_data;

	pr_debug("[Battery] %s : %x\n",
		__func__, pinfo->chg1_data.thermal_charging_current_limit);
	return sprintf(buf, "%u\n",
			pinfo->chg1_data.thermal_charging_current_limit);
}

static ssize_t store_chg1_current(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct charger_manager *pinfo = dev->driver_data;
	unsigned int reg = 0;
	int ret;

	pr_debug("[Battery] %s\n", __func__);
	if (buf != NULL && size != 0) {
		pr_debug("[Battery] buf is %s and size is %zu\n", buf, size);
		ret = kstrtouint(buf, 16, &reg);
		pinfo->chg1_data.thermal_charging_current_limit = reg;
		pr_debug("[Battery] %s: %x\n", __func__,
			pinfo->chg1_data.thermal_charging_current_limit);
	}
	return size;
}
static DEVICE_ATTR(chg1_current, 0644, show_chg1_current, store_chg1_current);

static ssize_t show_chg2_current(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct charger_manager *pinfo = dev->driver_data;

	pr_debug("[Battery] %s : %x\n",
		__func__, pinfo->chg2_data.thermal_charging_current_limit);
	return sprintf(buf, "%u\n",
			pinfo->chg2_data.thermal_charging_current_limit);
}

static ssize_t store_chg2_current(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct charger_manager *pinfo = dev->driver_data;
	unsigned int reg = 0;
	int ret;

	pr_debug("[Battery] %s\n", __func__);
	if (buf != NULL && size != 0) {
		pr_debug("[Battery] buf is %s and size is %zu\n", buf, size);
		ret = kstrtouint(buf, 16, &reg);
		pinfo->chg2_data.thermal_charging_current_limit = reg;
		pr_debug("[Battery] %s: %x\n", __func__,
			pinfo->chg2_data.thermal_charging_current_limit);
	}
	return size;
}
static DEVICE_ATTR(chg2_current, 0644, show_chg2_current, store_chg2_current);

static ssize_t show_BatNotify(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct charger_manager *pinfo = dev->driver_data;

	pr_debug("[Battery] show_BatteryNotify: 0x%x\n", pinfo->notify_code);

	return sprintf(buf, "%u\n", pinfo->notify_code);
}

static ssize_t store_BatNotify(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct charger_manager *pinfo = dev->driver_data;
	unsigned int reg = 0;
	int ret;

	pr_debug("[Battery] store_BatteryNotify\n");
	if (buf != NULL && size != 0) {
		pr_debug("[Battery] buf is %s and size is %zu\n", buf, size);
		ret = kstrtouint(buf, 16, &reg);
		pinfo->notify_code = reg;
		pr_debug("[Battery] store code: 0x%x\n", pinfo->notify_code);
		mtk_chgstat_notify(pinfo);
	}
	return size;
}

static DEVICE_ATTR(BatteryNotify, 0644, show_BatNotify, store_BatNotify);

static ssize_t show_BatNotify2(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct charger_manager *pinfo = dev->driver_data;

	pr_debug("[Battery] show_BatteryNotify2: 0x%x\n", pinfo->notify_code);

	return sprintf(buf, "%u\n", pinfo->notify_code);
}

static ssize_t store_BatNotify2(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	return store_BatNotify(dev, attr, buf, size);
}

static DEVICE_ATTR(BatteryNotify2, 0644, show_BatNotify2, store_BatNotify2);

static ssize_t show_BN_TestMode(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct charger_manager *pinfo = dev->driver_data;

	pr_debug("[Battery] %s : %x\n", __func__, pinfo->notify_test_mode);
	return sprintf(buf, "%u\n", pinfo->notify_test_mode);
}

static ssize_t store_BN_TestMode(struct device *dev,
		struct device_attribute *attr, const char *buf,  size_t size)
{
	struct charger_manager *pinfo = dev->driver_data;
	unsigned int reg = 0;
	int ret;

	pr_debug("[Battery] %s\n", __func__);
	if (buf != NULL && size != 0) {
		pr_debug("[Battery] buf is %s and size is %zu\n", buf, size);
		ret = kstrtouint(buf, 16, &reg);
		pinfo->notify_test_mode = reg;
		pr_debug("[Battery] store mode: %x\n", pinfo->notify_test_mode);
	}
	return size;
}
static DEVICE_ATTR(BN_TestMode, 0644, show_BN_TestMode, store_BN_TestMode);

static ssize_t show_ADC_Charger_Voltage(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int vbus = battery_get_vbus();

	if (!atomic_read(&pinfo->enable_kpoc_shdn) || vbus < 0) {
		chr_err("HardReset or get vbus failed, vbus:%d:5000\n", vbus);
		vbus = 5000;
	}

	pr_debug("[%s]: %d\n", __func__, vbus);
	return sprintf(buf, "%d\n", vbus);
}

static DEVICE_ATTR(ADC_Charger_Voltage, 0444, show_ADC_Charger_Voltage, NULL);

/* procfs */
static int mtk_chg_current_cmd_show(struct seq_file *m, void *data)
{
	struct charger_manager *pinfo = m->private;

	seq_printf(m, "%d %d\n", pinfo->usb_unlimited, pinfo->cmd_discharging);
	return 0;
}

static ssize_t mtk_chg_current_cmd_write(struct file *file,
		const char *buffer, size_t count, loff_t *data)
{
	int len = 0;
	char desc[32] = {0};
	int current_unlimited = 0;
	int cmd_discharging = 0;
	struct charger_manager *info = PDE_DATA(file_inode(file));

	if (!info)
		return -EINVAL;
	if (count <= 0)
		return -EINVAL;

	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return -EFAULT;

	desc[len] = '\0';

	if (sscanf(desc, "%d %d", &current_unlimited, &cmd_discharging) == 2) {
		info->usb_unlimited = current_unlimited;
		if (cmd_discharging == 1) {
			info->cmd_discharging = true;
			charger_dev_enable(info->chg1_dev, false);
			charger_manager_notifier(info,
						CHARGER_NOTIFY_STOP_CHARGING);
		} else if (cmd_discharging == 0) {
			info->cmd_discharging = false;
			charger_dev_enable(info->chg1_dev, true);
			charger_manager_notifier(info,
						CHARGER_NOTIFY_START_CHARGING);
		}

		pr_debug("%s current_unlimited=%d, cmd_discharging=%d\n",
			__func__, current_unlimited, cmd_discharging);
		return count;
	}

	chr_err("bad argument, echo [usb_unlimited] [disable] > current_cmd\n");
	return count;
}

static int mtk_chg_en_power_path_show(struct seq_file *m, void *data)
{
	struct charger_manager *pinfo = m->private;
	bool power_path_en = true;

	charger_dev_is_powerpath_enabled(pinfo->chg1_dev, &power_path_en);
	seq_printf(m, "%d\n", power_path_en);

	return 0;
}

static ssize_t mtk_chg_en_power_path_write(struct file *file,
		const char *buffer, size_t count, loff_t *data)
{
	int len = 0, ret = 0;
	char desc[32] = {0};
	unsigned int enable = 0;
	struct charger_manager *info = PDE_DATA(file_inode(file));

	if (!info)
		return -EINVAL;
	if (count <= 0)
		return -EINVAL;

	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return -EFAULT;

	desc[len] = '\0';

	ret = kstrtou32(desc, 10, &enable);
	if (ret == 0) {
		charger_dev_enable_powerpath(info->chg1_dev, enable);
		pr_debug("%s: enable power path = %d\n", __func__, enable);
		return count;
	}

	chr_err("bad argument, echo [enable] > en_power_path\n");
	return count;
}

static int mtk_chg_en_safety_timer_show(struct seq_file *m, void *data)
{
	struct charger_manager *pinfo = m->private;
	bool safety_timer_en = false;

	charger_dev_is_safety_timer_enabled(pinfo->chg1_dev, &safety_timer_en);
	seq_printf(m, "%d\n", safety_timer_en);

	return 0;
}

static ssize_t mtk_chg_en_safety_timer_write(struct file *file,
	const char *buffer, size_t count, loff_t *data)
{
	int len = 0, ret = 0;
	char desc[32] = {0};
	unsigned int enable = 0;
	struct charger_manager *info = PDE_DATA(file_inode(file));

	if (!info)
		return -EINVAL;
	if (count <= 0)
		return -EINVAL;

	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return -EFAULT;

	desc[len] = '\0';

	ret = kstrtou32(desc, 10, &enable);
	if (ret == 0) {
		charger_dev_enable_safety_timer(info->chg1_dev, enable);
		pr_debug("%s: enable safety timer = %d\n", __func__, enable);

		/* SW safety timer */
		if (info->sw_safety_timer_setting == true) {
			if (enable)
				info->enable_sw_safety_timer = true;
			else
				info->enable_sw_safety_timer = false;
		}

		return count;
	}

	chr_err("bad argument, echo [enable] > en_safety_timer\n");
	return count;
}

/* PROC_FOPS_RW(battery_cmd); */
/* PROC_FOPS_RW(discharging_cmd); */
PROC_FOPS_RW(current_cmd);
PROC_FOPS_RW(en_power_path);
PROC_FOPS_RW(en_safety_timer);

/* Create sysfs and procfs attributes */
static int mtk_charger_setup_files(struct platform_device *pdev)
{
	int ret = 0;
	struct proc_dir_entry *battery_dir = NULL;
	struct charger_manager *info = platform_get_drvdata(pdev);
	/* struct charger_device *chg_dev; */

	ret = device_create_file(&(pdev->dev), &dev_attr_sw_jeita);
	if (ret)
		goto _out;
	ret = device_create_file(&(pdev->dev), &dev_attr_pe20);
	if (ret)
		goto _out;
	ret = device_create_file(&(pdev->dev), &dev_attr_pe40);
	if (ret)
		goto _out;

	/* Battery warning */
	ret = device_create_file(&(pdev->dev), &dev_attr_BatteryNotify);
	if (ret)
		goto _out;
	/* Battery warning */
	ret = device_create_file(&(pdev->dev), &dev_attr_BatteryNotify2);
	if (ret)
		goto _out;
	ret = device_create_file(&(pdev->dev), &dev_attr_BN_TestMode);
	if (ret)
		goto _out;
	/* Pump express */
	ret = device_create_file(&(pdev->dev), &dev_attr_Pump_Express);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_charger_log_level);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_pdc_max_watt);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_ADC_Charger_Voltage);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_input_current);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_chg1_current);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_chg2_current);
	if (ret)
		goto _out;

	battery_dir = proc_mkdir("mtk_battery_cmd", NULL);
	if (!battery_dir) {
		chr_err("[%s]: mkdir /proc/mtk_battery_cmd failed\n", __func__);
		return -ENOMEM;
	}

	proc_create_data("current_cmd", 0640, battery_dir,
			&mtk_chg_current_cmd_fops, info);
	proc_create_data("en_power_path", 0640, battery_dir,
			&mtk_chg_en_power_path_fops, info);
	proc_create_data("en_safety_timer", 0640, battery_dir,
			&mtk_chg_en_safety_timer_fops, info);

_out:
	return ret;
}
#ifdef CONFIG_MTK_TYPEC_WATER_DETECT
#define CHG_SHOW_MAX_SIEZE 50
static int mmi_notify_lpd_event(struct charger_manager *info) {
	char *event_string = NULL;
	char *batt_uenvp[2];

	if(!info->battery_psy)
		info->battery_psy = power_supply_get_by_name("battery");
	if(!info->battery_psy) {
		chr_err("%s: get battery supply failed\n", __func__);
		return -EINVAL;
	}

	event_string = kmalloc(CHG_SHOW_MAX_SIEZE, GFP_KERNEL);

	scnprintf(event_string, CHG_SHOW_MAX_SIEZE,
			"POWER_SUPPLY_LPD_PRESENT=%s", info->water_detected? "true": "false");

	batt_uenvp[0] = event_string;
	batt_uenvp[1] = NULL;
	kobject_uevent_env(&info->battery_psy->dev.kobj, KOBJ_CHANGE, batt_uenvp);
	chr_err("%s, lpd:%d send %s\n",__func__, info->water_detected, event_string);
	kfree(event_string);
	return 0;
}
#endif
void notify_adapter_event(enum adapter_type type, enum adapter_event evt,
	void *val)
{
	chr_err("%s %d %d\n", __func__, type, evt);
	switch (type) {
	case MTK_PD_ADAPTER:
		switch (evt) {
		case MTK_PD_CONNECT_NONE:
			mutex_lock(&pinfo->charger_pd_lock);
			chr_err("PD Notify Detach\n");
			pinfo->pd_type = MTK_PD_CONNECT_NONE;
			mutex_unlock(&pinfo->charger_pd_lock);
			/* reset PE40 */
			break;

		case MTK_PD_CONNECT_HARD_RESET:
			mutex_lock(&pinfo->charger_pd_lock);
			chr_err("PD Notify HardReset\n");
			pinfo->pd_type = MTK_PD_CONNECT_NONE;
			pinfo->pd_reset = true;
			mutex_unlock(&pinfo->charger_pd_lock);
			_wake_up_charger(pinfo);
			/* reset PE40 */
			break;

		case MTK_PD_CONNECT_PE_READY_SNK:
			mutex_lock(&pinfo->charger_pd_lock);
			chr_err("PD Notify fixe voltage ready\n");
			pinfo->pd_type = MTK_PD_CONNECT_PE_READY_SNK;
			mutex_unlock(&pinfo->charger_pd_lock);
			/* PD is ready */
			break;

		case MTK_PD_CONNECT_PE_READY_SNK_PD30:
			mutex_lock(&pinfo->charger_pd_lock);
			chr_err("PD Notify PD30 ready\r\n");
			pinfo->pd_type = MTK_PD_CONNECT_PE_READY_SNK_PD30;
			mutex_unlock(&pinfo->charger_pd_lock);
			/* PD30 is ready */
			break;

		case MTK_PD_CONNECT_PE_READY_SNK_APDO:
			mutex_lock(&pinfo->charger_pd_lock);
			chr_err("PD Notify APDO Ready\n");
			pinfo->pd_type = MTK_PD_CONNECT_PE_READY_SNK_APDO;
			mutex_unlock(&pinfo->charger_pd_lock);
			/* PE40 is ready */
			_wake_up_charger(pinfo);
			break;

		case MTK_PD_CONNECT_TYPEC_ONLY_SNK:
			mutex_lock(&pinfo->charger_pd_lock);
			chr_err("PD Notify Type-C Ready\n");
			pinfo->pd_type = MTK_PD_CONNECT_TYPEC_ONLY_SNK;
			mutex_unlock(&pinfo->charger_pd_lock);
			/* type C is ready */
			_wake_up_charger(pinfo);
			break;
		case MTK_TYPEC_WD_STATUS:
			chr_err("wd status = %d\n", *(bool *)val);
			mutex_lock(&pinfo->charger_pd_lock);
			pinfo->water_detected = *(bool *)val;
			mutex_unlock(&pinfo->charger_pd_lock);

			if (pinfo->water_detected == true)
				pinfo->notify_code |= CHG_TYPEC_WD_STATUS;
			else
				pinfo->notify_code &= ~CHG_TYPEC_WD_STATUS;
			mtk_chgstat_notify(pinfo);
#ifdef CONFIG_MTK_TYPEC_WATER_DETECT
			mmi_notify_lpd_event(pinfo);
#endif
			break;
		case MTK_TYPEC_HRESET_STATUS:
			chr_err("hreset status = %d\n", *(bool *)val);
			mutex_lock(&pinfo->charger_pd_lock);
			if (*(bool *)val)
				atomic_set(&pinfo->enable_kpoc_shdn, 1);
			else
				atomic_set(&pinfo->enable_kpoc_shdn, 0);
			mutex_unlock(&pinfo->charger_pd_lock);
			break;
		};
	}
	mtk_pe50_notifier_call(pinfo, MTK_PE50_NOTISRC_TCP, evt, val);
}

static int proc_dump_log_show(struct seq_file *m, void *v)
{
	struct adapter_power_cap cap;
	int i;

	cap.nr = 0;
	cap.pdp = 0;
	for (i = 0; i < ADAPTER_CAP_MAX_NR; i++) {
		cap.max_mv[i] = 0;
		cap.min_mv[i] = 0;
		cap.ma[i] = 0;
		cap.type[i] = 0;
		cap.pwr_limit[i] = 0;
	}

	if (pinfo->pd_type == MTK_PD_CONNECT_PE_READY_SNK_APDO) {
		seq_puts(m, "********** PD APDO cap Dump **********\n");

		adapter_dev_get_cap(pinfo->pd_adapter, MTK_PD_APDO, &cap);
		for (i = 0; i < cap.nr; i++) {
			seq_printf(m,
			"%d: mV:%d,%d mA:%d type:%d pwr_limit:%d pdp:%d\n", i,
			cap.max_mv[i], cap.min_mv[i], cap.ma[i],
			cap.type[i], cap.pwr_limit[i], cap.pdp);
		}
	} else if (pinfo->pd_type == MTK_PD_CONNECT_PE_READY_SNK
		|| pinfo->pd_type == MTK_PD_CONNECT_PE_READY_SNK_PD30) {
		seq_puts(m, "********** PD cap Dump **********\n");

		adapter_dev_get_cap(pinfo->pd_adapter, MTK_PD, &cap);
		for (i = 0; i < cap.nr; i++) {
			seq_printf(m, "%d: mV:%d,%d mA:%d type:%d\n", i,
			cap.max_mv[i], cap.min_mv[i], cap.ma[i], cap.type[i]);
		}
	}

	return 0;
}

static ssize_t proc_write(
	struct file *file, const char __user *buffer,
	size_t count, loff_t *f_pos)
{
	return count;
}


static int proc_dump_log_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_dump_log_show, NULL);
}

static const struct file_operations charger_dump_log_proc_fops = {
	.open = proc_dump_log_open,
	.read = seq_read,
	.llseek	= seq_lseek,
	.write = proc_write,
};

void charger_debug_init(void)
{
	struct proc_dir_entry *charger_dir;

	charger_dir = proc_mkdir("charger", NULL);
	if (!charger_dir) {
		chr_err("fail to mkdir /proc/charger\n");
		return;
	}

	proc_create("dump_log", 0640,
		charger_dir, &charger_dump_log_proc_fops);
}

void scd_ctrl_cmd_from_user(void *nl_data, struct sc_nl_msg_t *ret_msg)
{
	struct sc_nl_msg_t *msg;

	msg = nl_data;
	ret_msg->sc_cmd = msg->sc_cmd;

	switch (msg->sc_cmd) {

	case SC_DAEMON_CMD_PRINT_LOG:
		{
			chr_err("%s", &msg->sc_data[0]);
		}
	break;

	case SC_DAEMON_CMD_SET_DAEMON_PID:
		{
			memcpy(&pinfo->sc.g_scd_pid, &msg->sc_data[0],
				sizeof(pinfo->sc.g_scd_pid));
			chr_err("[fr] SC_DAEMON_CMD_SET_DAEMON_PID = %d(first launch)\n",
				pinfo->sc.g_scd_pid);
		}
	break;

	case SC_DAEMON_CMD_SETTING:
		{
			struct scd_cmd_param_t_1 data;

			memcpy(&data, &msg->sc_data[0],
				sizeof(struct scd_cmd_param_t_1));

			chr_debug("rcv data:%d %d %d %d %d %d %d %d %d %d %d %d %d %d Ans:%d\n",
				data.data[0],
				data.data[1],
				data.data[2],
				data.data[3],
				data.data[4],
				data.data[5],
				data.data[6],
				data.data[7],
				data.data[8],
				data.data[9],
				data.data[10],
				data.data[11],
				data.data[12],
				data.data[13],
				data.data[14]);

			pinfo->sc.solution = data.data[SC_SOLUTION];
			if (data.data[SC_SOLUTION] == SC_DISABLE)
				pinfo->sc.disable_charger = true;
			else if (data.data[SC_SOLUTION] == SC_REDUCE)
				pinfo->sc.disable_charger = false;
			else
				pinfo->sc.disable_charger = false;
		}
	break;
	default:
		chr_err("bad sc_DAEMON_CTRL_CMD_FROM_USER 0x%x\n", msg->sc_cmd);
		break;
	}

}

static void sc_nl_send_to_user(u32 pid, int seq, struct sc_nl_msg_t *reply_msg)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	/* int size=sizeof(struct fgd_nl_msg_t); */
	int size = reply_msg->sc_data_len + SCD_NL_MSG_T_HDR_LEN;
	int len = NLMSG_SPACE(size);
	void *data;
	int ret;

	reply_msg->identity = SCD_NL_MAGIC;

	if (in_interrupt())
		skb = alloc_skb(len, GFP_ATOMIC);
	else
		skb = alloc_skb(len, GFP_KERNEL);

	if (!skb)
		return;

	if(pid == 0){
		chr_err("[Netlink] pid : %d\n", pid);
		return;
	}
	nlh = nlmsg_put(skb, pid, seq, 0, size, 0);
	data = NLMSG_DATA(nlh);
	memcpy(data, reply_msg, size);
	NETLINK_CB(skb).portid = 0;	/* from kernel */
	NETLINK_CB(skb).dst_group = 0;	/* unicast */

	ret = netlink_unicast(pinfo->sc.daemo_nl_sk, skb, pid, MSG_DONTWAIT);
	if (ret < 0) {
		chr_err("[Netlink] sc send failed %d\n", ret);
		return;
	}

}


static void chg_nl_data_handler(struct sk_buff *skb)
{
	u32 pid;
	kuid_t uid;
	int seq;
	void *data;
	struct nlmsghdr *nlh;
	struct sc_nl_msg_t *sc_msg, *sc_ret_msg;
	int size = 0;
	size_t fixed_part_size = offsetof(struct sc_nl_msg_t, sc_data);

	if (skb->len < NLMSG_HDRLEN) {
		chr_err("Received skb is too small\n");
		return;
	}

	nlh = (struct nlmsghdr *)skb->data;
	if (nlh->nlmsg_len < NLMSG_SPACE(sizeof(struct sc_nl_msg_t))) {
		chr_err("Netlink message is too short.\n");
		return;
	}

	pid = NETLINK_CREDS(skb)->pid;
	uid = NETLINK_CREDS(skb)->uid;
	seq = nlh->nlmsg_seq;

	if (nlh->nlmsg_len < NLMSG_LENGTH(fixed_part_size)) {
		chr_err("Netlink message is too short for sc_nl_msg_t header\n");
		return;
	}

	if (nlh->nlmsg_len > skb->len) {
		chr_err("Netlink message length exceeds skb length\n");
		return;
	}

	data = NLMSG_DATA(nlh);

	sc_msg = (struct sc_nl_msg_t *)data;

	if (sc_msg->sc_data_len > SCD_NL_MSG_MAX_LEN) {
		chr_err("sc_data_len exceeds SCD_NL_MSG_MAX_LEN\n");
		return;
	}

	if (nlh->nlmsg_len < NLMSG_LENGTH(fixed_part_size + sc_msg->sc_data_len)) {
		chr_err("Netlink message length is too short for sc_data\n");
		return;
	}

	if (sc_msg->sc_ret_data_len > INT_MAX - SCD_NL_MSG_T_HDR_LEN) {
		chr_err("Failed:sc_ret_data_len=%d maybe exceds INT_MAX\n",
			sc_msg->sc_ret_data_len);
		return;
	}

	size = sc_msg->sc_ret_data_len + SCD_NL_MSG_T_HDR_LEN;

	if (size > (PAGE_SIZE << 1))
		sc_ret_msg = vmalloc(size);
	else {
		if (in_interrupt())
			sc_ret_msg = kmalloc(size, GFP_ATOMIC);
	else
		sc_ret_msg = kmalloc(size, GFP_KERNEL);
	}

	if (sc_ret_msg == NULL) {
		if (size > PAGE_SIZE)
			sc_ret_msg = vmalloc(size);

		if (sc_ret_msg == NULL)
			return;
	}

	memset(sc_ret_msg, 0, size);

	scd_ctrl_cmd_from_user(data, sc_ret_msg);
	sc_nl_send_to_user(pid, seq, sc_ret_msg);

	kvfree(sc_ret_msg);
}

void sc_select_charging_current(struct charger_manager *info, struct charger_data *pdata)
{
	chr_err("sck: en:%d pid:%d %d %d %d %d %d thermal.dis:%d\n",
			info->sc.enable,
			info->sc.g_scd_pid,
			info->sc.pre_ibat,
			info->sc.sc_ibat,
			pdata->charging_current_limit,
			pdata->thermal_charging_current_limit,
			info->sc.solution,
			pinfo->sc.disable_in_this_plug);


	if (pinfo->sc.g_scd_pid != 0 && pinfo->sc.disable_in_this_plug == false) {
		if (info->sc.pre_ibat == -1 || info->sc.solution == SC_IGNORE
			|| info->sc.solution == SC_DISABLE) {
			info->sc.sc_ibat = -1;
		} else {
			if (info->sc.pre_ibat == pdata->charging_current_limit
				&& info->sc.solution == SC_REDUCE
				&& ((pdata->charging_current_limit - 100000) >= 500000)) {
				if (info->sc.sc_ibat == -1)
					info->sc.sc_ibat = pdata->charging_current_limit - 100000;
				else if (info->sc.sc_ibat - 100000 >= 500000)
					info->sc.sc_ibat = info->sc.sc_ibat - 100000;
			}
		}
	}
	info->sc.pre_ibat =  pdata->charging_current_limit;

	if (pdata->thermal_charging_current_limit != -1) {
		if (pdata->thermal_charging_current_limit <
		    pdata->charging_current_limit)
			pdata->charging_current_limit =
					pdata->thermal_charging_current_limit;
		pinfo->sc.disable_in_this_plug = true;
	} else if ((info->sc.solution == SC_REDUCE || info->sc.solution == SC_KEEP)
		&& info->sc.sc_ibat <
		pdata->charging_current_limit && pinfo->sc.g_scd_pid != 0 &&
		pinfo->sc.disable_in_this_plug == false && info->sc.sc_ibat != -1) {
		pdata->charging_current_limit = info->sc.sc_ibat;
	}
}

void sc_init(struct smartcharging *sc)
{
	sc->enable = false;
	sc->start_time = 0;
	sc->end_time = 80000;
	sc->target_percentage = 80;
	sc->pre_ibat = -1;
	sc->bh = 100;
	chr_err("%s: en:%d time:%d,%d tsoc:%d %d %d %d\n",
		__func__,
		sc->enable,
		sc->start_time,
		sc->end_time,
		sc->target_percentage,
		sc->battery_size,
		sc->left_time_for_cv,
		sc->current_limit);
}

void sc_update(struct charger_manager *pinfo)
{
	int time = pinfo->sc.left_time_for_cv;
	int bh = pinfo->sc.bh;

	memset(&pinfo->sc.data, 0, sizeof(struct scd_cmd_param_t_1));
	pinfo->sc.data.data[SC_VBAT] = battery_get_bat_voltage();
	pinfo->sc.data.data[SC_BAT_TMP] = battery_get_bat_temperature();
	pinfo->sc.data.data[SC_UISOC] = battery_get_uisoc();
	pinfo->sc.data.data[SC_SOC] = battery_get_soc();

	if (bh <= 80) {
		pinfo->sc.enable = false;
		chr_err("battery health(%d) is too low to enable sc\n", bh);
	}
	pinfo->sc.data.data[SC_ENABLE] = pinfo->sc.enable;
	pinfo->sc.data.data[SC_BAT_SIZE] = pinfo->sc.battery_size;
	pinfo->sc.data.data[SC_START_TIME] = pinfo->sc.start_time;
	pinfo->sc.data.data[SC_END_TIME] = pinfo->sc.end_time;
	pinfo->sc.data.data[SC_IBAT_LIMIT] = pinfo->sc.current_limit;
	pinfo->sc.data.data[SC_TARGET_PERCENTAGE] = pinfo->sc.target_percentage;

	if (bh <= 90) {
		chr_err("battery health(%d) is low ,time from %d => %d\n",
			bh, time, time * 3 / 2);
		time = time * 3 / 2;
	}

	pinfo->sc.data.data[SC_LEFT_TIME_FOR_CV] = time;

	charger_dev_get_charging_current(pinfo->chg1_dev, &pinfo->sc.data.data[SC_IBAT_SETTING]);
	pinfo->sc.data.data[SC_IBAT_SETTING] = pinfo->sc.data.data[SC_IBAT_SETTING] / 1000;
	pinfo->sc.data.data[SC_IBAT] = battery_get_bat_current() / 10;
	charger_dev_get_ibus(pinfo->chg1_dev, &pinfo->sc.data.data[SC_IBUS]);
	if (chargerlog_level == 1)
		pinfo->sc.data.data[SC_DBGLV] = 3;
	else
		pinfo->sc.data.data[SC_DBGLV] = 7;

}

int wakeup_sc_algo_cmd(struct scd_cmd_param_t_1 *data, int subcmd, int para1)
{

	if (pinfo->sc.g_scd_pid != 0) {
		struct sc_nl_msg_t *sc_msg;
		int size = SCD_NL_MSG_T_HDR_LEN + sizeof(struct scd_cmd_param_t_1);

		if (size > (PAGE_SIZE << 1))
			sc_msg = vmalloc(size);
		else {
			if (in_interrupt())
				sc_msg = kmalloc(size, GFP_ATOMIC);
		else
			sc_msg = kmalloc(size, GFP_KERNEL);

		}

		if (sc_msg == NULL) {
			if (size > PAGE_SIZE)
				sc_msg = vmalloc(size);

			if (sc_msg == NULL)
				return -1;
		}

		sc_update(pinfo);

		chr_debug(
			"[wakeup_fg_algo] malloc size=%d pid=%d\n",
			size, pinfo->sc.g_scd_pid);
		memset(sc_msg, 0, size);
		sc_msg->sc_cmd = SC_DAEMON_CMD_NOTIFY_DAEMON;
		sc_msg->sc_subcmd = subcmd;
		sc_msg->sc_subcmd_para1 = para1;
		memcpy(sc_msg->sc_data, data, sizeof(struct scd_cmd_param_t_1));
		sc_msg->sc_data_len += sizeof(struct scd_cmd_param_t_1);
		sc_nl_send_to_user(pinfo->sc.g_scd_pid, 0, sc_msg);

		kvfree(sc_msg);

		return 0;
	}
	chr_debug("pid is NULL\n");
	return -1;
}

static ssize_t show_sc_en(
	struct device *dev, struct device_attribute *attr,
					char *buf)
{

	chr_err(
	"[enable smartcharging] : %d\n",
	pinfo->sc.enable);

	return sprintf(buf, "%d\n", pinfo->sc.enable);
}

static ssize_t store_sc_en(
	struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t size)
{
	unsigned long val = 0;
	int ret;

	if (buf != NULL && size != 0) {
		chr_err("[enable smartcharging] buf is %s\n", buf);
		ret = kstrtoul(buf, 10, &val);
		if (val < 0) {
			chr_err(
				"[enable smartcharging] val is %d ??\n",
				(int)val);
			val = 0;
		}

		if (val == 0)
			pinfo->sc.enable = false;
		else
			pinfo->sc.enable = true;

		chr_err(
			"[enable smartcharging]enable smartcharging=%d\n",
			pinfo->sc.enable);
	}
	return size;
}
static DEVICE_ATTR(enable_sc, 0664,
	show_sc_en, store_sc_en);

static ssize_t show_sc_stime(
	struct device *dev, struct device_attribute *attr,
					char *buf)
{

	chr_err(
	"[smartcharging stime] : %d\n",
	pinfo->sc.start_time);

	return sprintf(buf, "%d\n", pinfo->sc.start_time);
}

static ssize_t store_sc_stime(
	struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t size)
{
	unsigned long val = 0;
	int ret;

	if (buf != NULL && size != 0) {
		chr_err("[smartcharging stime] buf is %s\n", buf);
		ret = kstrtoul(buf, 10, &val);
		if (val < 0) {
			chr_err(
				"[smartcharging stime] val is %d ??\n",
				(int)val);
			val = 0;
		}

		if (val >= 0)
			pinfo->sc.start_time = val;

		chr_err(
			"[smartcharging stime]enable smartcharging=%d\n",
			pinfo->sc.start_time);
	}
	return size;
}
static DEVICE_ATTR(sc_stime, 0664,
	show_sc_stime, store_sc_stime);

static ssize_t show_sc_etime(
	struct device *dev, struct device_attribute *attr,
					char *buf)
{

	chr_err(
	"[smartcharging etime] : %d\n",
	pinfo->sc.end_time);

	return sprintf(buf, "%d\n", pinfo->sc.end_time);
}

static ssize_t store_sc_etime(
	struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t size)
{
	unsigned long val = 0;
	int ret;

	if (buf != NULL && size != 0) {
		chr_err("[smartcharging etime] buf is %s\n", buf);
		ret = kstrtoul(buf, 10, &val);
		if (val < 0) {
			chr_err(
				"[smartcharging etime] val is %d ??\n",
				(int)val);
			val = 0;
		}

		if (val >= 0)
			pinfo->sc.end_time = val;

		chr_err(
			"[smartcharging stime]enable smartcharging=%d\n",
			pinfo->sc.end_time);
	}
	return size;
}
static DEVICE_ATTR(sc_etime, 0664,
	show_sc_etime, store_sc_etime);

static ssize_t show_sc_tuisoc(
	struct device *dev, struct device_attribute *attr,
					char *buf)
{

	chr_err(
	"[smartcharging target uisoc] : %d\n",
	pinfo->sc.target_percentage);

	return sprintf(buf, "%d\n", pinfo->sc.target_percentage);
}

static ssize_t store_sc_tuisoc(
	struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t size)
{
	unsigned long val = 0;
	int ret;

	if (buf != NULL && size != 0) {
		chr_err("[smartcharging tuisoc] buf is %s\n", buf);
		ret = kstrtoul(buf, 10, &val);
		if (val < 0) {
			chr_err(
				"[smartcharging tuisoc] val is %d ??\n",
				(int)val);
			val = 0;
		}

		if (val >= 0)
			pinfo->sc.target_percentage = val;

		chr_err(
			"[smartcharging stime]tuisoc=%d\n",
			pinfo->sc.target_percentage);
	}
	return size;
}
static DEVICE_ATTR(sc_tuisoc, 0664,
	show_sc_tuisoc, store_sc_tuisoc);

static ssize_t show_sc_ibat_limit(
	struct device *dev, struct device_attribute *attr,
					char *buf)
{

	chr_err(
	"[smartcharging ibat limit] : %d\n",
	pinfo->sc.current_limit);

	return sprintf(buf, "%d\n", pinfo->sc.current_limit);
}

static ssize_t store_sc_ibat_limit(
	struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t size)
{
	unsigned long val = 0;
	int ret;

	if (buf != NULL && size != 0) {
		chr_err("[smartcharging ibat limit] buf is %s\n", buf);
		ret = kstrtoul(buf, 10, &val);
		if (val < 0) {
			chr_err(
				"[smartcharging ibat limit] val is %d ??\n",
				(int)val);
			val = 0;
		}

		if (val >= 0)
			pinfo->sc.current_limit = val;

		chr_err(
			"[smartcharging ibat limit]=%d\n",
			pinfo->sc.current_limit);
	}
	return size;
}
static DEVICE_ATTR(sc_ibat_limit, 0664,
	show_sc_ibat_limit, store_sc_ibat_limit);

static ssize_t show_sc_test(
	struct device *dev, struct device_attribute *attr,
					char *buf)
{
	return sprintf(buf, "%d\n", 0);
}

static ssize_t store_sc_test(
	struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t size)
{
	unsigned long val = 0;
	int ret;

	if (buf != NULL && size != 0) {
		chr_err("[smartcharging test] buf is %s\n", buf);
		ret = kstrtoul(buf, 10, &val);
		if (val < 0) {
			chr_err(
				"[smartcharging test] val is %d ??\n",
				(int)val);
			val = 0;
		}

		if (val == 1) {
			charger_manager_enable_sc(pinfo->chg1_consumer,
				true, 1, 1111);
		} else if (val == 2) {
			charger_manager_enable_sc(pinfo->chg1_consumer,
				false, 2, 2222);
		} else if (val == 3) {
			charger_manager_set_sc_current_limit(pinfo->chg1_consumer,
				1000);
		} else {
			charger_manager_set_bh(pinfo->chg1_consumer,
				val);
		}

	}
	return size;
}
static DEVICE_ATTR(sc_test, 0664,
	show_sc_test, store_sc_test);

/*==================moto chg tcmd interface======================*/
static int  mtk_charger_tcmd_set_chg_enable(void *input, int  val)
{
	struct charger_manager *cm = (struct charger_manager *)input;
	int ret;

	val = !!val;
	charger_dev_enable(cm->chg1_dev, val);

	charging_enable_flag = val;
	val = val ? CHARGER_NOTIFY_START_CHARGING : CHARGER_NOTIFY_STOP_CHARGING;
	_wake_up_charger(cm);
	ret = charger_manager_notifier(cm, val);

	return ret;
}

static int  mtk_charger_tcmd_set_usb_enable(void *input, int  val)
{
	struct charger_manager *cm = (struct charger_manager *)input;
	int ret;

	val = !!val;
	ret = charger_dev_enable_powerpath(cm->chg1_dev, val);

	return ret;
}

static int  mtk_charger_tcmd_set_chg_current(void *input, int  val)
{
	struct charger_manager *cm = (struct charger_manager *)input;
	int ret;

	cm->chg1_data.moto_chg_tcmd_ibat = val * 1000;
	ret = _mtk_charger_change_current_setting(cm);

	return ret;
}

static int  mtk_charger_tcmd_get_chg_current(void *input, int* val)
{
	struct charger_manager *cm = (struct charger_manager *)input;
	int ret = 0;

	*val = cm->chg1_data.moto_chg_tcmd_ibat / 1000;

	return ret;
}

static int  mtk_charger_tcmd_set_usb_current(void *input, int  val)
{
	struct charger_manager *cm = (struct charger_manager *)input;
	int ret;

	cm->chg1_data.moto_chg_tcmd_ichg = val * 1000;
	ret = _mtk_charger_change_current_setting(cm);

	return ret;
}

static int  mtk_charger_tcmd_get_usb_current(void *input, int* val)
{
	struct charger_manager *cm = (struct charger_manager *)input;
	int ret = 0;

	*val = cm->chg1_data.moto_chg_tcmd_ichg / 1000;

	return ret;
}

static int  mtk_charger_tcmd_get_usb_voltage(void *input, int* val)
{
	int ret = 0;

	*val = charger_get_vbus(); /* mV */
	*val *= 1000; /*convert to uV*/

	return ret;
}

static int  mtk_charger_tcmd_get_charger_type(void *input, int* val)
{
	struct charger_manager *cm = (struct charger_manager *)input;
	int ret = 0;

	*val = cm->chr_type;

	return ret;
}

static int  mtk_charger_tcmd_register(struct charger_manager *cm)
{
	int ret;

	cm->chg_tcmd_client.data = cm;
	cm->chg_tcmd_client.client_id = MOTO_CHG_TCMD_CLIENT_CHG;

	cm->chg_tcmd_client.set_chg_enable = mtk_charger_tcmd_set_chg_enable;
	cm->chg_tcmd_client.set_usb_enable = mtk_charger_tcmd_set_usb_enable;

	cm->chg_tcmd_client.get_chg_current = mtk_charger_tcmd_get_chg_current;
	cm->chg_tcmd_client.set_chg_current = mtk_charger_tcmd_set_chg_current;
	cm->chg_tcmd_client.get_usb_current = mtk_charger_tcmd_get_usb_current;
	cm->chg_tcmd_client.set_usb_current = mtk_charger_tcmd_set_usb_current;

	cm->chg_tcmd_client.get_usb_voltage = mtk_charger_tcmd_get_usb_voltage;

	cm->chg_tcmd_client.get_charger_type = mtk_charger_tcmd_get_charger_type;

	ret = moto_chg_tcmd_register(&cm->chg_tcmd_client);

	return ret;
}
/*==================================================*/

static int mtk_charger_probe(struct platform_device *pdev)
{
	struct charger_manager *info = NULL;
	struct list_head *pos = NULL;
	struct list_head *phead = &consumer_head;
	struct charger_consumer *ptr = NULL;
	int i, ret;
	int ret_device_file;
	struct netlink_kernel_cfg cfg = {
		.input = chg_nl_data_handler,
	};
	struct device *dev = NULL;
	struct device_node *boot_node = NULL;
	struct tag_bootmode *tag = NULL;
	int boot_mode = 11;//UNKNOWN_BOOT
	
	chr_err("%s: starts\n", __func__);
	
	// workaround for mt6768 
	//int boot_mode = get_boot_mode();
	dev = &(pdev->dev);
	if (dev != NULL){
		boot_node = of_parse_phandle(dev->of_node, "bootmode", 0);
		if (!boot_node){
			chr_err("%s: failed to get boot mode phandle\n", __func__);
		}
		else {
			tag = (struct tag_bootmode *)of_get_property(boot_node,
								"atag,boot", NULL);
			if (!tag){
				chr_err("%s: failed to get atag,boot\n", __func__);
			}
			else
				boot_mode = tag->bootmode;
		}
	}

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);

	if (!info)
		return -ENOMEM;
	pinfo = info;
	
	platform_set_drvdata(pdev, info);
	info->pdev = pdev;
	mtk_charger_parse_dt(info, &pdev->dev);

	mutex_init(&info->charger_lock);
	mutex_init(&info->charger_pd_lock);
	mutex_init(&info->cable_out_lock);
	mutex_init(&info->mmi_mux_lock);
	for (i = 0; i < TOTAL_CHARGER; i++) {
		mutex_init(&info->pp_lock[i]);
		info->force_disable_pp[i] = false;
		info->enable_pp[i] = true;
	}
	/*work around for mt6768*/
	atomic_set(&info->enable_kpoc_shdn, 1);
	info->charger_wakelock = wakeup_source_register(NULL, "charger suspend wakelock");
#ifdef CONFIG_MOTO_CHG_FFC_5V10W_SUPPORT
	info->ffc_charger_wakelock = wakeup_source_register(NULL, "fcc charger suspend wakelock");
#endif
	spin_lock_init(&info->slock);

	/* init thread */
	init_waitqueue_head(&info->wait_que);
	info->polling_interval = CHARGING_INTERVAL;
	info->enable_dynamic_cv = true;

	info->chg1_data.thermal_charging_current_limit = -1;
	info->chg1_data.thermal_input_current_limit = -1;
	info->chg1_data.input_current_limit_by_aicl = -1;
	info->chg2_data.thermal_charging_current_limit = -1;
	info->chg2_data.thermal_input_current_limit = -1;
	info->dvchg1_data.thermal_input_current_limit = -1;
	info->dvchg2_data.thermal_input_current_limit = -1;
	info->chg1_data.moto_chg_tcmd_ibat = -1;
	info->chg1_data.moto_chg_tcmd_ichg = -1;
	info->chg1_data.cp_ichg_limit= -1;

	info->sw_jeita.error_recovery_flag = true;

	mmi_info = info;
	mmi_init(info);

#ifdef CONFIG_MOTO_CHG_FFC_5V10W_SUPPORT
	INIT_DELAYED_WORK(&info->ffc_enable_charge_work, ffc_enable_charge_work);
	mmi_charger_ffc_init(info);
#endif

	mtk_charger_init_timer(info);
	info->is_pdc_run = false;
	kthread_run(charger_routine_thread, info, "charger_thread");

	if (info->chg1_dev != NULL && info->do_event != NULL) {
		info->chg1_nb.notifier_call = info->do_event;
		register_charger_device_notifier(info->chg1_dev,
						&info->chg1_nb);
		charger_dev_set_drvdata(info->chg1_dev, info);
	}

	info->psy_nb.notifier_call = charger_psy_event;
	power_supply_reg_notifier(&info->psy_nb);

	srcu_init_notifier_head(&info->evt_nh);
	ret = mtk_charger_setup_files(pdev);
	if (ret)
		chr_err("Error creating sysfs interface\n");

	info->pd_adapter = get_adapter_by_name("pd_adapter");
	if (info->pd_adapter)
		chr_err("Found PD adapter [%s]\n",
			info->pd_adapter->props.alias_name);
	else
		chr_err("*** Error : can't find PD adapter ***\n");

	if (mtk_pe_init(info) < 0)
		info->enable_pe_plus = false;

	if (mtk_pe20_init(info) < 0)
		info->enable_pe_2 = false;

	if (mtk_wlc_init(info, &pdev->dev) < 0)
		info->enable_wlc = false;

	if (mtk_pe40_init(info) == false)
		info->enable_pe_4 = false;

	if (mtk_pe50_init(info) < 0)
		info->enable_pe_5 = false;

	mtk_pdc_init(info);
	charger_ftm_init();
	mtk_charger_get_atm_mode(info);
	sw_jeita_state_machine_init(info);

#ifdef CONFIG_MTK_CHARGER_UNLIMITED
	info->usb_unlimited = true;
	info->enable_sw_safety_timer = false;
	charger_dev_enable_safety_timer(info->chg1_dev, false);
#endif

	info->sc.daemo_nl_sk = netlink_kernel_create(&init_net, NETLINK_CHG, &cfg);

	if (info->sc.daemo_nl_sk == NULL)
		chr_err("sc netlink_kernel_create error id:%d\n", NETLINK_CHG);
	else
		chr_err("sc_netlink_kernel_create success id:%d\n", NETLINK_CHG);
	sc_init(&info->sc);

	charger_debug_init();

	mtk_charger_tcmd_register(info);

	mutex_lock(&consumer_mutex);
	list_for_each(pos, phead) {
		ptr = container_of(pos, struct charger_consumer, list);
		ptr->cm = info;
		if (ptr->pnb != NULL) {
			srcu_notifier_chain_register(&info->evt_nh, ptr->pnb);
			ptr->pnb = NULL;
		}
	}
	mutex_unlock(&consumer_mutex);

	/* sysfs node */
	ret_device_file = device_create_file(&(pdev->dev),
		&dev_attr_enable_sc);
	ret_device_file = device_create_file(&(pdev->dev),
		&dev_attr_sc_stime);
	ret_device_file = device_create_file(&(pdev->dev),
		&dev_attr_sc_etime);
	ret_device_file = device_create_file(&(pdev->dev),
		&dev_attr_sc_tuisoc);
	ret_device_file = device_create_file(&(pdev->dev),
		&dev_attr_sc_ibat_limit);
	ret_device_file = device_create_file(&(pdev->dev),
		&dev_attr_sc_test);

	info->chg1_consumer =
		charger_manager_get_by_name(&pdev->dev, "charger_port1");

	if (info->chg1_consumer != NULL &&
	    boot_mode != KERNEL_POWER_OFF_CHARGING_BOOT &&
	    boot_mode != LOW_POWER_OFF_CHARGING_BOOT)
#ifdef CONFIG_MOTO_POWER_PATH_DISABLE
		charger_manager_force_disable_power_path(
			info->chg1_consumer, MAIN_CHARGER, false);
#else
		charger_manager_force_disable_power_path(
			info->chg1_consumer, MAIN_CHARGER, true);
#endif
	info->init_done = true;
	if (info->mmi.factory_mode) {
		/* Disable charging when enter ATM mode(factory mode) */
		charging_enable_flag = 0;
		mtk_charger_tcmd_set_usb_current((void *)info, 2000);
	}
	_wake_up_charger(info);

	return 0;
}

static int mtk_charger_remove(struct platform_device *dev)
{
	struct charger_manager *info = platform_get_drvdata(dev);

	mtk_pe50_deinit(info);
	mtk_wlc_remove(info);
	return 0;
}

static void mtk_charger_shutdown(struct platform_device *dev)
{
	struct charger_manager *info = platform_get_drvdata(dev);
	int ret;

	if (mtk_pe20_get_is_connect(info) || mtk_pe_get_is_connect(info) || wlc_get_online()) {
		if (info->chg2_dev)
			charger_dev_enable(info->chg2_dev, false);
		ret = mtk_pe20_reset_ta_vchr(info);
		if (ret == -ENOTSUPP)
			mtk_pe_reset_ta_vchr(info);
		ret = mtk_wlc_reset_ta_vchr(info);
		if (ret == -ENOTSUPP)
			mtk_pe_reset_ta_vchr(info);
		pr_debug("%s: reset TA before shutdown\n", __func__);
	}
}

static const struct of_device_id mtk_charger_of_match[] = {
	{.compatible = "mediatek,charger",},
	{},
};

MODULE_DEVICE_TABLE(of, mtk_charger_of_match);

struct platform_device charger_device = {
	.name = "charger",
	.id = -1,
};

static struct platform_driver charger_driver = {
	.probe = mtk_charger_probe,
	.remove = mtk_charger_remove,
	.shutdown = mtk_charger_shutdown,
	.driver = {
		   .name = "charger",
		   .of_match_table = mtk_charger_of_match,
	},
};

static int __init mtk_charger_init(void)
{
	return platform_driver_register(&charger_driver);
}
late_initcall(mtk_charger_init);

static void __exit mtk_charger_exit(void)
{
	platform_driver_unregister(&charger_driver);
}
module_exit(mtk_charger_exit);


MODULE_AUTHOR("wy.chuang <wy.chuang@mediatek.com>");
MODULE_DESCRIPTION("MTK Charger Driver");
MODULE_LICENSE("GPL");
