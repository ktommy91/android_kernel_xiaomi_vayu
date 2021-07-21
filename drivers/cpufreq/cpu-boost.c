/*
 * Copyright (c) 2013-2015,2017, The Linux Foundation. All rights reserved.
 * Copyright (c) 2017-2018, Paranoid Android.
 * Copyright (C) 2017-2018, Razer Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "cpu-boost: " fmt

#include <linux/kernel.h>
#include <linux/init.h>

#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/time.h>
#include <uapi/linux/sched/types.h>

#include <linux/sched/rt.h>



static struct kthread_work input_boost_work;

static struct kthread_work powerkey_input_boost_work;

static unsigned int powerkey_input_boost_ms = 400;
module_param(powerkey_input_boost_ms, uint, 0644);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
static int dynamic_stune_boost=30;
module_param(dynamic_stune_boost, uint, 0644);
static bool stune_boost_active;
static int boost_slot;
static unsigned int dynamic_stune_boost_ms = 100;
module_param(dynamic_stune_boost_ms, uint, 0644);
static struct delayed_work dynamic_stune_boost_rem;
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

static u64 last_input_time;

static struct kthread_worker cpu_boost_worker;
static struct task_struct *cpu_boost_worker_thread;

static struct kthread_worker powerkey_cpu_boost_worker;
static struct task_struct *powerkey_cpu_boost_worker_thread;

#define MIN_INPUT_INTERVAL (100 * USEC_PER_MSEC)
#define MAX_NAME_LENGTH 64



#ifdef CONFIG_DYNAMIC_STUNE_BOOST
static void do_dynamic_stune_boost_rem(struct work_struct *work)
{
	/* Reset dynamic stune boost value to the default value */
	if (stune_boost_active) {
		reset_stune_boost("top-app", boost_slot);
		stune_boost_active = false;
	}
}
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

static void do_input_boost(struct kthread_work *work)
{
	unsigned int ret;

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	cancel_delayed_work_sync(&dynamic_stune_boost_rem);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	if (stune_boost_active) {
		reset_stune_boost("top-app", boost_slot);
		stune_boost_active = false;
	}
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Set dynamic stune boost value */
	ret = do_stune_boost("top-app", dynamic_stune_boost, &boost_slot);
	if (!ret)
		stune_boost_active = true;

	schedule_delayed_work(&dynamic_stune_boost_rem, msecs_to_jiffies(dynamic_stune_boost_ms));
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

}

static void do_powerkey_input_boost(struct kthread_work *work)
{

	unsigned int ret;
	
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	cancel_delayed_work_sync(&dynamic_stune_boost_rem);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	if (stune_boost_active) {
		reset_stune_boost("top-app", boost_slot);
		stune_boost_active = false;
	}
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Set dynamic stune boost value */
	ret = do_stune_boost("top-app", dynamic_stune_boost, &boost_slot);
	if (!ret)
		stune_boost_active = true;

	schedule_delayed_work(&dynamic_stune_boost_rem, msecs_to_jiffies(powerkey_input_boost_ms));
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

}

static void cpuboost_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	u64 now;

	if (dynamic_stune_boost==0 || dynamic_stune_boost_ms==0)
		return;

	now = ktime_to_us(ktime_get());
	if (now - last_input_time < MIN_INPUT_INTERVAL)
		return;

	if (queuing_blocked(&cpu_boost_worker, &input_boost_work))
		return;

	if ((type == EV_KEY && code == KEY_POWER) ||
		(type == EV_KEY && code == KEY_WAKEUP)) {
		kthread_queue_work(&cpu_boost_worker, &powerkey_input_boost_work);
	} else
		kthread_queue_work(&cpu_boost_worker, &input_boost_work);

	last_input_time = ktime_to_us(ktime_get());
}

static int cpuboost_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void cpuboost_input_disconnect(struct input_handle *handle)
{
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Reset dynamic stune boost value to the default value */
	reset_stune_boost("top-app", boost_slot);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpuboost_ids[] = {
	/* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler cpuboost_input_handler = {
	.event          = cpuboost_input_event,
	.connect        = cpuboost_input_connect,
	.disconnect     = cpuboost_input_disconnect,
	.name           = "cpu-boost",
	.id_table       = cpuboost_ids,
};

static int cpu_boost_init(void)
{
	int ret, i;
	struct sched_param param = { .sched_priority = 2 };
	cpumask_t sys_bg_mask;

	/* Hardcode the cpumask to bind the kthread to it */
	cpumask_clear(&sys_bg_mask);
	for (i = 0; i <= 3; i++) {
		cpumask_set_cpu(i, &sys_bg_mask);
	}

	kthread_init_worker(&cpu_boost_worker);
	cpu_boost_worker_thread = kthread_create(kthread_worker_fn,
		&cpu_boost_worker, "cpu_boost_worker_thread");
	if (IS_ERR(cpu_boost_worker_thread)) {
		pr_err("cpu-boost: Failed to init kworker!\n");
		return -EFAULT;
	}

	ret = sched_setscheduler(cpu_boost_worker_thread, SCHED_FIFO, &param);
	if (ret)
		pr_err("cpu-boost: Failed to set SCHED_FIFO!\n");

	kthread_init_worker(&powerkey_cpu_boost_worker);
	powerkey_cpu_boost_worker_thread = kthread_create(kthread_worker_fn,
		&powerkey_cpu_boost_worker, "powerkey_cpu_boost_worker_thread");
	if (IS_ERR(powerkey_cpu_boost_worker_thread)) {
		pr_err("powerkey_cpu-boost: Failed to init kworker!\n");
		return -EFAULT;
	}

	ret = sched_setscheduler(powerkey_cpu_boost_worker_thread, SCHED_FIFO, &param);
	if (ret)
		pr_err("powerkey_cpu-boost: Failed to set SCHED_FIFO!\n");

	/* Now bind it to the cpumask */
	kthread_bind_mask(cpu_boost_worker_thread, &sys_bg_mask);
	kthread_bind_mask(powerkey_cpu_boost_worker_thread, &sys_bg_mask);

	/* Wake it up! */
	wake_up_process(cpu_boost_worker_thread);
	wake_up_process(powerkey_cpu_boost_worker_thread);

	kthread_init_work(&input_boost_work, do_input_boost);
	kthread_init_work(&powerkey_input_boost_work, do_powerkey_input_boost);
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	INIT_DELAYED_WORK(&dynamic_stune_boost_rem, do_dynamic_stune_boost_rem);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	ret = input_register_handler(&cpuboost_input_handler);
	return 0;
}
late_initcall(cpu_boost_init);
