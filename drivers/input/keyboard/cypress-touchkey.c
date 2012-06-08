/*
 * Copyright (C) 2011 Samsung Electronics
 * Copyright (C) 2012 The CyanogenMod Project
 *
 * Authors: Shankar Bandal <shankar.b@samsung.com>
 *          Daniel Hillenbrand <codeworkx@cyanogenmod.com>
 *          Marco Hillenbrand <marco.hillenbrand@googlemail.com>
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/cypress-touchkey.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "../../../arch/arm/mach-omap2/board-t1.h"

#define DEVICE_NAME "sec_touchkey"
#define I2C_M_WR    0      /* for i2c */
#define FW_SIZE     8192

/*
 * Melfas touchkey register
 */
#define KEYCODE_REG         0x00
#define CMD_REG             0x03
#define DIFF_DATA_REG       0x0A

/* Command for 0x00 */
#define AUTO_CAL_MODE_CMD   0x50
#define LED_ON_CMD          0x10
#define LED_OFF_CMD         0x20

/* Command for 0x03 */
#define AUTO_CAL_EN_CMD     0x01
#define SENS_EN_CMD         0x40

#define UPDOWN_EVENT_BIT    0x08
#define KEYCODE_BIT         0x07
#define COMMAND_BIT         0xF0

struct cptk_data *cptk_local;
struct timer_list touch_led_timer;
int touch_led_timeout = 3; // timeout for the touchkey backlight in secs
int touch_led_disabled = 0; // 1= force disable the touchkey backlight

static void touch_led_enable(struct cptk_data *cptk);
static void touch_led_disable(struct cptk_data *cptk);

struct cptk_data {
    struct cptk_platform_data   *pdata;
    struct input_dev *input_dev;
    struct i2c_client *client;
    struct device *sec_touchkey;
    struct early_suspend early_suspend;
    struct mutex    i2c_lock;
    struct mutex    lock;
    struct work_struct	work;
    struct workqueue_struct *wq;
    u8 led_status;
    u8 cur_firm_ver[3];
    int touchkey_update_status;
    bool enable;
    bool notification;
    bool calibrated;
};

static irqreturn_t cptk_irq_thread(int irq, void *data);

static int cptk_i2c_write(struct cptk_data *cptk, u8 cmd,
        u8 val)
{
    int ret = 0;
    u8 data[2];
    struct i2c_msg msg[1];
    int retry = 2;

    if (!cptk->enable) {
        pr_err("cptk :key is not enabled.\n");
        return -ENODEV;
    }

    mutex_lock(&cptk->i2c_lock);

    while (retry--) {
        data[0] = cmd;
        data[1] = val;
        msg->addr = cptk->client->addr;
        msg->flags = I2C_M_WR;
        msg->len = 2;
        msg->buf = data;
        ret = i2c_transfer(cptk->client->adapter, msg, 1);
        if (ret >= 0) {
            mutex_unlock(&cptk->i2c_lock);
            return 0;
        }
        pr_info("cptk :i2c transfer retry for cmd:%d\n", cmd);
        msleep(20);
    }

    mutex_unlock(&cptk->i2c_lock);

    return ret;
}

static int cptk_i2c_read(struct cptk_data *cptk, u8 cmd,
        u8 *val, int len)
{
    int ret = 0;
    int retry = 10;
    struct i2c_msg msg[2];

    if (!cptk->enable) {
        pr_err("cptk :key is not enabled.\n");
        return -ENODEV;
    }

    mutex_lock(&cptk->i2c_lock);

    while (retry--) {
        msg[0].addr = cptk->client->addr;
        msg[0].flags = I2C_M_WR;
        msg[0].len = 1;
        msg[0].buf = &cmd;

        msg[1].addr = cptk->client->addr;
        msg[1].flags = I2C_M_RD;
        msg[1].len = len;
        msg[1].buf = val;

        ret = i2c_transfer(cptk->client->adapter, msg, 2);
        if (ret >= 0) {
            mutex_unlock(&cptk->i2c_lock);
            return 0;
        }
        pr_debug("cptk :i2c transfer retor for cmd:%d\n", cmd);
        msleep(20);
    }
    mutex_unlock(&cptk->i2c_lock);

    return ret;
}

static irqreturn_t cptk_irq_thread(int irq, void *data)
{
    struct cptk_data *cptk = data;
    int ret;
    u8 keycode;

    mutex_lock(&cptk->lock);
    if (!gpio_get_value(cptk->pdata->gpio)) {
        ret = cptk_i2c_read(cptk, KEYCODE_REG, &keycode, 1);
        if (keycode & UPDOWN_EVENT_BIT) {
            input_report_key(cptk->input_dev, cptk->pdata->keymap[keycode & KEYCODE_BIT], 0);

            // touch led timeout on keyup
            if (touch_led_disabled == 0) {
                if (timer_pending(&touch_led_timer) == 0 && !cptk->notification) {
                    pr_debug("cptk: %s: keyup - add_timer\n", __func__);
                    touch_led_timer.expires = jiffies + (HZ * touch_led_timeout);
                    add_timer(&touch_led_timer);
                } else {
                    mod_timer(&touch_led_timer, jiffies + (HZ * touch_led_timeout));
                }
            }
        } else {
            input_report_key(cptk->input_dev, cptk->pdata->keymap[keycode & KEYCODE_BIT], 1);

            // enable lights on keydown
            if (touch_led_disabled == 0) {
                if (cptk->led_status == LED_OFF_CMD) {
                    if (!cptk->enable) {
                        if (cptk && cptk->pdata->power)
                            cptk->pdata->power(1);
                        cptk->enable = true;
                        enable_irq(cptk->client->irq);
                    }
                    pr_debug("cptk: %s: keydown - LED ON\n", __func__);
                    cptk_i2c_write(cptk, KEYCODE_REG, LED_ON_CMD);
                    cptk->led_status = LED_ON_CMD;
                }
                if (timer_pending(&touch_led_timer) == 1) {
                    mod_timer(&touch_led_timer, jiffies + (HZ * touch_led_timeout));
                }
            }
        }

        input_sync(cptk->input_dev);
    }
    mutex_unlock(&cptk->lock);

    return IRQ_HANDLED;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static int cptk_early_suspend(struct early_suspend *h)
{
    pr_debug("cptk: %s\n", __func__);
    int i;

    struct cptk_data *cptk = container_of(h,
            struct cptk_data,
            early_suspend);

    mutex_lock(&cptk->lock);
    if (cptk->enable && !cptk->notification) {
        pr_info("cptk: %s suspending\n", __func__);

        cptk_i2c_write(cptk, KEYCODE_REG, LED_OFF_CMD);
        cptk->led_status = LED_OFF_CMD;

        disable_irq(cptk->client->irq);
        if (cptk && cptk->pdata->power)
            cptk->pdata->power(0);
        cptk->enable = false;
        cptk->calibrated = false;

        /* release key */
        for (i = 1; i < cptk->pdata->keymap_size; i++)
            input_report_key(cptk->input_dev,
                     cptk->pdata->keymap[i], 0);
    } else if (cptk->notification) {
        pr_info("cptk: %s not suspending, notification is active\n", __func__);
    }
    mutex_unlock(&cptk->lock);
    return 0;
}

static int cptk_late_resume(struct early_suspend *h)
{
    pr_info("cptk: %s\n", __func__);

    struct cptk_data *cptk = container_of(h,
            struct cptk_data,
            early_suspend);

    mutex_lock(&cptk->lock);
    if (!cptk->enable) {
        if (cptk && cptk->pdata->power)
            cptk->pdata->power(1);
        cptk->enable = true;
        enable_irq(cptk->client->irq);
    }
    pr_info("cptk: %s auto calibration...\n", __func__);
    cptk_i2c_write(cptk, KEYCODE_REG, AUTO_CAL_MODE_CMD);
    cptk_i2c_write(cptk, CMD_REG, AUTO_CAL_EN_CMD);
    cptk->calibrated = true;
    msleep(50);

    if (cptk->enable) {
        if (touch_led_disabled == 0) {
            cptk_i2c_write(cptk, KEYCODE_REG, LED_ON_CMD);
            cptk->led_status = LED_ON_CMD;
        }
    }
    mutex_unlock(&cptk->lock);

    return 0;
}
#endif
static void cptk_update_firmware_cb(const struct firmware *fw,
        void *context)
{

    int ret;
    struct cptk_data *cptk = context;
    struct device *dev = &cptk->input_dev->dev;
    int retries = 10;

    pr_info("cptk: firware download start\n");

    if (fw->size != FW_SIZE) {
        dev_err(dev, "%s: Firmware file size invalid size:%d\n",
                __func__, fw->size);
        return;
    }

    mutex_lock(&cptk->lock);

    disable_irq(cptk->client->irq);
    /* Lock the i2c bus since the firmware updater accesses it */
    i2c_lock_adapter(cptk->client->adapter);
    while (retries--) {
        ret = touchkey_flash_firmware(cptk->pdata, fw->data);
        if (!ret)
            break;
    }
    if (ret) {
        cptk->touchkey_update_status = -1;
        dev_err(dev, "%s: Firmware update failed\n", __func__);
    } else {
        cptk->touchkey_update_status = 0;
        pr_info("cptk: firware download finished\n");
    }

    i2c_unlock_adapter(cptk->client->adapter);
    enable_irq(cptk->client->irq);

    release_firmware(fw);
    mutex_unlock(&cptk->lock);

    cptk_i2c_read(cptk, KEYCODE_REG,
            cptk->cur_firm_ver,
            sizeof(cptk->cur_firm_ver));
    return;
}

static int cptk_update_firmware(struct cptk_data *cptk)
{
    int ret;
    struct device *dev = &cptk->input_dev->dev;
    cptk->touchkey_update_status = 1;
    if (!cptk->pdata->fw_name) {
        dev_err(dev, "%s: Device firmware name is not set\n", __func__);
        return -EINVAL;
    }

    ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG,
            cptk->pdata->fw_name, dev, GFP_KERNEL, cptk,
            cptk_update_firmware_cb);
    if (ret) {
        dev_err(dev, "%s: Can't open firmware file from %s\n", __func__,
            cptk->pdata->fw_name);
        return ret;
    }

    return 0;
}

static ssize_t set_touchkey_firm_update_store(struct device *dev,
        struct device_attribute *attr,
        const char *buf, size_t size)
{
    struct cptk_data *cptk = dev_get_drvdata(dev);

    if (*buf == 'S' || *buf == 'F') {
        if ((*buf != 'F' &&
        cptk->cur_firm_ver[1] >=
        cptk->pdata->firm_ver) &&
        cptk->cur_firm_ver[1] != 0xFF) {
            cptk->touchkey_update_status = 0;

            pr_info("cptk: already updated latest version\n");
            return size;
        }
        cptk_update_firmware(cptk);
    }

    return size;
}
static DEVICE_ATTR(touchkey_firm_update,
        S_IWUSR | S_IWGRP,
        NULL, set_touchkey_firm_update_store);

static ssize_t set_touchkey_firm_status_show
(struct device *dev, struct device_attribute *attr, char *buf)
{
    int count = 0;
    struct cptk_data *cptk = dev_get_drvdata(dev);

    if (cptk->touchkey_update_status == 0)
        count = sprintf(buf, "PASS\n");
    else if (cptk->touchkey_update_status == 1)
        count = sprintf(buf, "DOWNLOADING\n");
    else if (cptk->touchkey_update_status == -1)
        count = sprintf(buf, "FAIL\n");

    return count;
}
static DEVICE_ATTR(touchkey_firm_update_status, S_IRUGO,
set_touchkey_firm_status_show, NULL);

static ssize_t set_touchkey_firm_version_show
(struct device *dev, struct device_attribute *attr, char *buf)
{
    int count;

    struct cptk_data *cptk = dev_get_drvdata(dev);
    count = sprintf(buf, "0x%X\n", cptk->cur_firm_ver[1]);
    pr_info("cptk: touchkey_firm_version 0x%X\n", cptk->cur_firm_ver[1]);

    return count;
}
static DEVICE_ATTR(touchkey_firm_version_phone, S_IRUGO,
set_touchkey_firm_version_show, NULL);

static ssize_t set_touchkey_firm_version_read_show
(struct device *dev, struct device_attribute *attr, char *buf)
{
    char data[3] = { 0, };
    int count, ret;
    struct cptk_data *cptk = dev_get_drvdata(dev);

    mutex_lock(&cptk->lock);
    ret = cptk_i2c_read(cptk, KEYCODE_REG, data, 3);
    if (ret) {
        pr_err("cptk : %s: error in cptk_i2c_read\n" , __func__);
        mutex_unlock(&cptk->lock);
        return ret;
    }
    mutex_unlock(&cptk->lock);
    count = sprintf(buf, "0x%X\n", data[1]);
    pr_info("cptk :touch_version_read 0x%X\n", data[1]);

    return count;
}
static DEVICE_ATTR(touchkey_firm_version_panel, S_IRUGO,
set_touchkey_firm_version_read_show, NULL);

static void touch_led_enable(struct cptk_data *cptk)
{
    pr_debug("cptk: %s\n", __func__);

    mutex_lock(&cptk->lock);
    if (touch_led_disabled == 0 && cptk->led_status == LED_OFF_CMD) {
        if (!cptk->enable) {
            if (cptk && cptk->pdata->power)
                cptk->pdata->power(1);
            cptk->enable = true;
            enable_irq(cptk->client->irq);
        }

        pr_debug("cptk: %s LED ON\n", __func__);
        cptk_i2c_write(cptk, KEYCODE_REG, LED_ON_CMD);
        cptk->led_status = LED_ON_CMD;

        if (timer_pending(&touch_led_timer) == 0 && !cptk->notification) {
            // touch led timeout
            pr_debug("cptk: %s add_timer\n", __func__);
            touch_led_timer.expires = jiffies + (HZ * touch_led_timeout);
            add_timer(&touch_led_timer);
        }
    }
    mutex_unlock(&cptk->lock);
}

static void touch_led_disable(struct cptk_data *cptk)
{
    pr_debug("cptk: %s\n", __func__);

    mutex_lock(&cptk->lock);
    if (touch_led_disabled == 0 && !cptk->notification) {
        if (cptk->enable) {
            pr_debug("cptk: %s LED OFF\n", __func__);
            
            cptk_i2c_write(cptk, KEYCODE_REG, LED_OFF_CMD);
            cptk->led_status = LED_OFF_CMD;

            disable_irq(cptk->client->irq);
            if (cptk && cptk->pdata->power)
                cptk->pdata->power(0);
            cptk->enable = false;
        }
    }
    mutex_unlock(&cptk->lock);
}

static ssize_t touch_led_enable_disable(struct device *dev,
        struct device_attribute *attr, const char *buf,
        size_t size)
{
    struct cptk_data *cptk = dev_get_drvdata(dev);
    int data, ret;

    ret = sscanf(buf, "%d\n", &data);
    if (unlikely(ret != 1)) {
        pr_err("cptk: %s err\n", __func__);
        return -EINVAL;
    }
    pr_debug("cptk: %s value=%d\n", __func__, data);

    if (data > 0)
        touch_led_enable(cptk);
    else
        touch_led_disable(cptk);

    return size;
}
static DEVICE_ATTR(enable_disable, S_IRUGO | S_IWUSR | S_IWGRP,
        NULL, touch_led_enable_disable);

static ssize_t touch_led_force_disable(struct device *dev,
        struct device_attribute *attr, const char *buf,
        size_t size)
{
    struct cptk_data *cptk = dev_get_drvdata(dev);
    int data, ret;

    ret = sscanf(buf, "%d\n", &data);
    if (unlikely(ret != 1)) {
        pr_err("cptk: %s err\n", __func__);
        return -EINVAL;
    }
    pr_info("cptk: %s value=%d\n", __func__, data);
    
    if (data == 1) {
        if (cptk->enable) {
            cptk_i2c_write(cptk, KEYCODE_REG, LED_OFF_CMD);
            cptk->led_status = LED_OFF_CMD;
        }
    }
    touch_led_disabled = data;

    return size;
}
static DEVICE_ATTR(force_disable, S_IRUGO | S_IWUSR | S_IWGRP,
        NULL, touch_led_force_disable);

static ssize_t touch_led_notification(struct device *dev,
        struct device_attribute *attr, const char *buf,
        size_t size)
{
    struct cptk_data *cptk = dev_get_drvdata(dev);
    int data, ret;

    ret = sscanf(buf, "%d\n", &data);
    if (unlikely(ret != 1)) {
        pr_err("cptk: %s err\n", __func__);
        return -EINVAL;
    }
    pr_debug("cptk: %s value=%d\n", __func__, data);

    if (data > 0 && touch_led_disabled == 0) {
        pr_debug("cptk: %s on\n", __func__);
        cptk->notification = true;
        touch_led_enable(cptk);
    } else {
        pr_debug("cptk: %s off\n", __func__);
        cptk->notification = false;
    }

    return size;
}
static DEVICE_ATTR(notification, S_IRUGO | S_IWUSR | S_IWGRP,
        NULL, touch_led_notification);

static ssize_t touch_led_set_timeout(struct device *dev,
        struct device_attribute *attr, const char *buf,
        size_t size)
{
    int data;
    int ret;

    ret = sscanf(buf, "%d\n", &data);
    if (unlikely(ret != 1)) {
        pr_err("cptk: %s err\n", __func__);
        return -EINVAL;
    }
    pr_info("cptk: %s new timeout=%d\n", __func__, data);
    touch_led_timeout = data;

    return size;
}
static DEVICE_ATTR(timeout, S_IRUGO | S_IWUSR | S_IWGRP,
        NULL, touch_led_set_timeout);

void touch_led_timedout(unsigned long ptr)
{
    pr_debug("cptk: %s\n", __func__);
    queue_work(cptk_local->wq, &cptk_local->work);
}

void touch_led_timedout_work(struct work_struct *work)
{
	struct cptk_data *cptk = container_of(work, struct cptk_data, work);

    mutex_lock(&cptk->lock);
    if (!cptk->notification && touch_led_timeout != 0)
    {
        pr_debug("cptk: %s disabling touchled\n", __func__);
        cptk_i2c_write(cptk, KEYCODE_REG, LED_OFF_CMD);
        cptk->led_status = LED_OFF_CMD;
    }
    mutex_unlock(&cptk->lock);
}

void touchscreen_state_report(int state)
{
    if (touch_led_disabled == 0) {
        if (state == 1) {
            if(cptk_local->led_status == LED_OFF_CMD) {
                pr_debug("cptk: %s enable touchleds\n", __func__);
                touch_led_enable(cptk_local);
            } else {
                if (timer_pending(&touch_led_timer) == 1) {
                    pr_debug("cptk: %s mod_timer\n", __func__);
                    //del_timer(&touch_led_timer);
                    mod_timer(&touch_led_timer, jiffies + (HZ * touch_led_timeout));
                }
            }
        } else if (state == 0 && !cptk_local->notification) {
            if (timer_pending(&touch_led_timer) == 1) {
                pr_debug("cptk: %s mod_timer\n", __func__);
                mod_timer(&touch_led_timer, jiffies + (HZ * touch_led_timeout));
            } else if (cptk_local->led_status == LED_ON_CMD){
                pr_debug("cptk: %s add_timer\n", __func__);
                touch_led_timer.expires = jiffies + (HZ * touch_led_timeout);
                add_timer(&touch_led_timer);
            }
        }
    }
}

static ssize_t touchkey_menu_show(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct cptk_data *cptk = dev_get_drvdata(dev);
    u8 data[2];
    int ret;
    int menu_sensitivity;

    mutex_lock(&cptk->lock);
    ret = cptk_i2c_read(cptk, DIFF_DATA_REG,
            data, sizeof(data));

    menu_sensitivity = ((0x00FF&data[0])<<8)|data[1];
    pr_info("cptk : menu_sensitivity = %d\n", menu_sensitivity);

    mutex_unlock(&cptk->lock);

    return sprintf(buf, "%d\n", menu_sensitivity);
}
static DEVICE_ATTR(touchkey_menu, S_IRUGO, touchkey_menu_show, NULL);

static ssize_t touchkey_back_show(struct device *dev,
        struct device_attribute *attr, char *buf)
{

    u8 data[2];
    int ret;
    int back_sensitivity;

    struct cptk_data *cptk = dev_get_drvdata(dev);
    mutex_lock(&cptk->lock);
    ret = cptk_i2c_read(cptk, DIFF_DATA_REG + 2,
            data, sizeof(data));

    back_sensitivity = ((0x00FF&data[0])<<8)|data[1];
    pr_info("cptk : back_sensitivity = %d\n", back_sensitivity);

    mutex_unlock(&cptk->lock);

    return sprintf(buf, "%d\n", back_sensitivity);
}
static DEVICE_ATTR(touchkey_back, S_IRUGO, touchkey_back_show, NULL);

static ssize_t touch_sensitivity_control(struct device *dev,
        struct device_attribute *attr, const char *buf,
        size_t size)
{
    struct cptk_data *cptk = dev_get_drvdata(dev);

    mutex_lock(&cptk->lock);
    cptk_i2c_write(cptk, KEYCODE_REG, SENS_EN_CMD);
    mutex_unlock(&cptk->lock);

    return size;
}
static DEVICE_ATTR(touch_sensitivity,
        S_IRUGO | S_IWUSR | S_IWGRP,
        NULL, touch_sensitivity_control);

static int cptk_create_sec_touchkey(struct cptk_data *cptk)
{
    int ret;

    cptk->sec_touchkey = device_create(sec_class, NULL,
            0, NULL, "sec_touchkey");
    if (IS_ERR(cptk->sec_touchkey))
        goto err;

    ret = device_create_file(cptk->sec_touchkey, &dev_attr_enable_disable);
    if (ret < 0) {
        pr_err("cptk :Failed to create device file %s\n",
                dev_attr_enable_disable.attr.name);
        goto err;
    }

    ret = device_create_file(cptk->sec_touchkey, &dev_attr_force_disable);
    if (ret < 0) {
        pr_err("cptk :Failed to create device file %s\n",
                dev_attr_force_disable.attr.name);
        goto err;
    }

    ret = device_create_file(cptk->sec_touchkey, &dev_attr_notification);
    if (ret < 0) {
        pr_err("cptk :Failed to create device file %s\n",
                dev_attr_notification.attr.name);
        goto err;
    }

    ret = device_create_file(cptk->sec_touchkey, &dev_attr_timeout);
    if (ret < 0) {
        pr_err("cptk :Failed to create device file %s\n",
                dev_attr_timeout.attr.name);
        goto err;
    }

    ret = device_create_file(cptk->sec_touchkey,
        &dev_attr_touchkey_firm_update);
    if (ret < 0) {
        pr_err("cptk: Failed to create device file %s\n",
            dev_attr_touchkey_firm_update.attr.name);
        goto err;
    }

    ret = device_create_file(cptk->sec_touchkey,
        &dev_attr_touchkey_firm_update_status);
    if (ret < 0) {
        pr_err("cptk: Failed to create device file(%s)!\n",
        dev_attr_touchkey_firm_update_status.attr.name);
        goto err;
    }

    ret = device_create_file(cptk->sec_touchkey,
        &dev_attr_touchkey_firm_version_phone);
    if (ret < 0) {
        pr_err("cptk: Failed to create device file(%s)!\n",
        dev_attr_touchkey_firm_version_phone.attr.name);
        goto err;
    }

    ret = device_create_file(cptk->sec_touchkey,
        &dev_attr_touchkey_firm_version_panel);
    if (ret < 0) {
        pr_err("cptk :Failed to create device file(%s)!\n",
        dev_attr_touchkey_firm_version_panel.attr.name);
        goto err;
    }

    ret = device_create_file(cptk->sec_touchkey,
            &dev_attr_touchkey_menu);
    if (ret < 0) {
        pr_err("cptk : Failed to create device file %s\n",
        dev_attr_touchkey_menu.attr.name);
        goto err;
    }

    ret = device_create_file(cptk->sec_touchkey,
            &dev_attr_touchkey_back);
    if (ret < 0) {
        pr_err("cptk : Failed to create device file %s\n",
        dev_attr_touchkey_back.attr.name);
        goto err;
    }

    ret = device_create_file(cptk->sec_touchkey,
            &dev_attr_touch_sensitivity);
    if (ret < 0) {
        pr_err("cptk : Failed to create device file %s\n",
        dev_attr_touch_sensitivity.attr.name);
        goto err;
    }

    dev_set_drvdata(cptk->sec_touchkey, cptk);

    return 0;
err:
    return -EINVAL;
}
static int __devinit cptk_i2c_probe(struct i2c_client *client,
            const struct i2c_device_id *id)
{
    struct cptk_data *cptk;
    int ret;
    int i;

    cptk = kzalloc(sizeof(struct cptk_data), GFP_KERNEL);
    if (!cptk) {
        dev_err(&client->dev, "failed to allocate driver data\n");
        return -ENOMEM;
    }
    cptk_local = cptk;

    cptk->input_dev = input_allocate_device();
    if (!cptk->input_dev)
        return -ENOMEM;

    cptk->pdata = client->dev.platform_data;
    if (!cptk->pdata) {
        ret = -EINVAL;
        goto err_exit1;
    }
    cptk->client = client;
    strlcpy(cptk->client->name, "melfas-touchkey",
            I2C_NAME_SIZE);
    cptk->client->dev.init_name = DEVICE_NAME;
    i2c_set_clientdata(client, cptk);

    cptk->input_dev->name = DEVICE_NAME;
    cptk->input_dev->phys = "melfas-touchkey/input0";
    cptk->input_dev->id.bustype = BUS_HOST;

    set_bit(EV_SYN, cptk->input_dev->evbit);
    set_bit(EV_KEY, cptk->input_dev->evbit);
    set_bit(EV_LED, cptk->input_dev->evbit);
    set_bit(LED_MISC, cptk->input_dev->ledbit);

    for (i = 1; i < cptk->pdata->keymap_size; i++)
        set_bit(cptk->pdata->keymap[i], cptk->input_dev->keybit);

    ret = input_register_device(cptk->input_dev);
    if (ret) {
        input_free_device(cptk->input_dev);
        return ret;
    }

    mutex_init(&cptk->i2c_lock);
    mutex_init(&cptk->lock);

    if (cptk->pdata->power)
        cptk->pdata->power(1);

    cptk->enable = true;
    cptk->notification = false;
    cptk->calibrated = false;

    cptk_i2c_read(cptk, KEYCODE_REG,
            cptk->cur_firm_ver,
            sizeof(cptk->cur_firm_ver));

    pr_info("cptk :firm ver = 0x%x, mod ver = 0x%x\n",
            cptk->cur_firm_ver[1], cptk->cur_firm_ver[2]);

    if (cptk->cur_firm_ver[2] <= cptk->pdata->mod_ver) {
        if (cptk->cur_firm_ver[1] < cptk->pdata->firm_ver) {
            pr_info("cptk: force firmware update\n");
            cptk_update_firmware(cptk);
        }
    }

#ifdef CONFIG_HAS_EARLYSUSPEND
    cptk->early_suspend.level =
        EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 2;
    cptk->early_suspend.suspend = (void *) cptk_early_suspend;
    cptk->early_suspend.resume = (void *) cptk_late_resume;
    register_early_suspend(&cptk->early_suspend);
#endif

    pr_info("cptk: %s auto calibration...\n", __func__);
    cptk_i2c_write(cptk, KEYCODE_REG, AUTO_CAL_MODE_CMD);
    cptk_i2c_write(cptk, CMD_REG, AUTO_CAL_EN_CMD);
    cptk->calibrated = true;

    ret = request_threaded_irq(client->irq, NULL, cptk_irq_thread,
                   IRQF_TRIGGER_LOW | IRQF_ONESHOT,
                   DEVICE_NAME, cptk);

    if (ret < 0) {
        pr_err("%s: request_threaded_irq returned %d\n", __func__, ret);
        goto err_exit2;
    }

    // init workqueue
    cptk->wq = create_singlethread_workqueue("cptk_wq");
    if (!cptk->wq) {
        ret = -ENOMEM;
        pr_err("%s: could not create workqueue\n", __func__);
        goto err_exit2;
    }

    /* this is the thread function we run on the work queue */
	INIT_WORK(&cptk->work, touch_led_timedout_work);

    ret = cptk_create_sec_touchkey(cptk);
    if (ret < 0) {
        pr_err("%s: cptk_create_sec_touchkey returned %d\n", __func__, ret);
        goto err_exit2;
    }

    return 0;

err_exit2:
err_exit1:
    kfree(cptk);
    return ret;
}

static int __devexit cptk_remove(struct i2c_client *client)
{
    return 0;
}

static const struct i2c_device_id cptk_id[] = {
    {"melfas_touchkey", 0},
    {}
};
MODULE_DEVICE_TABLE(i2c, cptk_id);

static struct i2c_driver cptk_i2c_driver = {
    .driver = {
        .owner = THIS_MODULE,
        .name = "melfas_touchkey",
    },
    .id_table = cptk_id,
    .probe = cptk_i2c_probe,
    .remove = __devexit_p(cptk_remove),
    .command = NULL,
};

static int __init cptk_init(void)
{
    int ret;

    ret = i2c_add_driver(&cptk_i2c_driver);
    if (ret < 0)
        return ret;

    // init the touchled timer
    init_timer(&touch_led_timer);
    touch_led_timer.function = touch_led_timedout;

    return ret;
}

static void __exit cptk_exit(void)
{
    i2c_del_driver(&cptk_i2c_driver);
}

module_init(cptk_init);
module_exit(cptk_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("shankar bandal <shankar.b@samsung.com>");
MODULE_DESCRIPTION("melfas touch keypad");
