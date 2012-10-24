/*
 *  linux/drivers/cpufreq/cpufreq.c
 *
 *  Copyright (C) 2001 Russell King
 *            (C) 2002 - 2003 Dominik Brodowski <linux@brodo.de>
 *
 *  Oct 2005 - Ashok Raj <ashok.raj@intel.com>
 *	Added handling for CPU hotplug
 *  Feb 2006 - Jacob Shin <jacob.shin@amd.com>
 *	Fix handling for CPU hotplug -- affected CPUs
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/syscore_ops.h>
#include <linux/pm_qos_params.h>

#include <trace/events/power.h>

#include "../../arch/arm/mach-tegra/dvfs.h"
#include "../../arch/arm/mach-tegra/clock.h"
#include "../../arch/arm/mach-tegra/fuse.h"

#ifdef CONFIG_GPU_OVERCLOCK
static DEFINE_MUTEX(dvfs_lock);
#endif


#define dprintk(msg...) cpufreq_debug_printk(CPUFREQ_DEBUG_CORE, \
						"cpufreq-core", msg)

//                                                               
#ifdef CONFIG_ARCH_TEGRA_3x_SOC
unsigned long cpufreq_limited_max_cores_cur = 4;
unsigned long cpufreq_limited_max_cores_expected = 4;
#endif
//                                                               

/**
 * The "cpufreq driver" - the arch- or hardware-dependent low
 * level driver of CPUFreq support, and its spinlock. This lock
 * also protects the cpufreq_cpu_data array.
 */
static struct cpufreq_driver *cpufreq_driver;
static DEFINE_PER_CPU(struct cpufreq_policy *, cpufreq_cpu_data);
#ifdef CONFIG_HOTPLUG_CPU
/* This one keeps track of the previously set governor of a removed CPU */
static DEFINE_PER_CPU(char[CPUFREQ_NAME_LEN], cpufreq_cpu_governor);
#endif
static DEFINE_SPINLOCK(cpufreq_driver_lock);

/*
 * cpu_policy_rwsem is a per CPU reader-writer semaphore designed to cure
 * all cpufreq/hotplug/workqueue/etc related lock issues.
 *
 * The rules for this semaphore:
 * - Any routine that wants to read from the policy structure will
 *   do a down_read on this semaphore.
 * - Any routine that will write to the policy structure and/or may take away
 *   the policy altogether (eg. CPU hotplug), will hold this lock in write
 *   mode before doing so.
 *
 * Additional rules:
 * - All holders of the lock should check to make sure that the CPU they
 *   are concerned with are online after they get the lock.
 * - Governor routines that can be called in cpufreq hotplug path should not
 *   take this sem as top level hotplug notifier handler takes this.
 * - Lock should not be held across
 *     __cpufreq_governor(data, CPUFREQ_GOV_STOP);
 */
static DEFINE_PER_CPU(int, cpufreq_policy_cpu);
static DEFINE_PER_CPU(struct rw_semaphore, cpu_policy_rwsem);

#define lock_policy_rwsem(mode, cpu)					\
int lock_policy_rwsem_##mode					\
(int cpu)								\
{									\
	int policy_cpu = per_cpu(cpufreq_policy_cpu, cpu);		\
	BUG_ON(policy_cpu == -1);					\
	down_##mode(&per_cpu(cpu_policy_rwsem, policy_cpu));		\
	if (unlikely(!cpu_online(cpu))) {				\
		up_##mode(&per_cpu(cpu_policy_rwsem, policy_cpu));	\
		return -1;						\
	}								\
									\
	return 0;							\
}

lock_policy_rwsem(read, cpu);

lock_policy_rwsem(write, cpu);

static void unlock_policy_rwsem_read(int cpu)
{
	int policy_cpu = per_cpu(cpufreq_policy_cpu, cpu);
	BUG_ON(policy_cpu == -1);
	up_read(&per_cpu(cpu_policy_rwsem, policy_cpu));
}

void unlock_policy_rwsem_write(int cpu)
{
	int policy_cpu = per_cpu(cpufreq_policy_cpu, cpu);
	BUG_ON(policy_cpu == -1);
	up_write(&per_cpu(cpu_policy_rwsem, policy_cpu));
}


/* internal prototypes */
static int __cpufreq_governor(struct cpufreq_policy *policy,
		unsigned int event);
static unsigned int __cpufreq_get(unsigned int cpu);
static void handle_update(struct work_struct *work);

/**
 * Two notifier lists: the "policy" list is involved in the
 * validation process for a new CPU frequency policy; the
 * "transition" list for kernel code that needs to handle
 * changes to devices when the CPU clock speed changes.
 * The mutex locks both lists.
 */
static BLOCKING_NOTIFIER_HEAD(cpufreq_policy_notifier_list);
static struct srcu_notifier_head cpufreq_transition_notifier_list;

static bool init_cpufreq_transition_notifier_list_called;
static int __init init_cpufreq_transition_notifier_list(void)
{
	srcu_init_notifier_head(&cpufreq_transition_notifier_list);
	init_cpufreq_transition_notifier_list_called = true;
	return 0;
}
pure_initcall(init_cpufreq_transition_notifier_list);

static LIST_HEAD(cpufreq_governor_list);
static DEFINE_MUTEX(cpufreq_governor_mutex);

struct cpufreq_policy *cpufreq_cpu_get(unsigned int cpu)
{
	struct cpufreq_policy *data;
	unsigned long flags;

	if (cpu >= nr_cpu_ids)
		goto err_out;

	/* get the cpufreq driver */
	spin_lock_irqsave(&cpufreq_driver_lock, flags);

	if (!cpufreq_driver)
		goto err_out_unlock;

	if (!try_module_get(cpufreq_driver->owner))
		goto err_out_unlock;


	/* get the CPU */
	data = per_cpu(cpufreq_cpu_data, cpu);

	if (!data)
		goto err_out_put_module;

	if (!kobject_get(&data->kobj))
		goto err_out_put_module;

	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);
	return data;

err_out_put_module:
	module_put(cpufreq_driver->owner);
err_out_unlock:
	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);
err_out:
	return NULL;
}
EXPORT_SYMBOL_GPL(cpufreq_cpu_get);


void cpufreq_cpu_put(struct cpufreq_policy *data)
{
	kobject_put(&data->kobj);
	module_put(cpufreq_driver->owner);
}
EXPORT_SYMBOL_GPL(cpufreq_cpu_put);


/*********************************************************************
 *                     UNIFIED DEBUG HELPERS                         *
 *********************************************************************/
#ifdef CONFIG_CPU_FREQ_DEBUG

/* what part(s) of the CPUfreq subsystem are debugged? */
static unsigned int debug;

/* is the debug output ratelimit'ed using printk_ratelimit? User can
 * set or modify this value.
 */
static unsigned int debug_ratelimit = 1;

/* is the printk_ratelimit'ing enabled? It's enabled after a successful
 * loading of a cpufreq driver, temporarily disabled when a new policy
 * is set, and disabled upon cpufreq driver removal
 */
static unsigned int disable_ratelimit = 1;
static DEFINE_SPINLOCK(disable_ratelimit_lock);

static void cpufreq_debug_enable_ratelimit(void)
{
	unsigned long flags;

	spin_lock_irqsave(&disable_ratelimit_lock, flags);
	if (disable_ratelimit)
		disable_ratelimit--;
	spin_unlock_irqrestore(&disable_ratelimit_lock, flags);
}

static void cpufreq_debug_disable_ratelimit(void)
{
	unsigned long flags;

	spin_lock_irqsave(&disable_ratelimit_lock, flags);
	disable_ratelimit++;
	spin_unlock_irqrestore(&disable_ratelimit_lock, flags);
}

void cpufreq_debug_printk(unsigned int type, const char *prefix,
			const char *fmt, ...)
{
	char s[256];
	va_list args;
	unsigned int len;
	unsigned long flags;

	WARN_ON(!prefix);
	if (type & debug) {
		spin_lock_irqsave(&disable_ratelimit_lock, flags);
		if (!disable_ratelimit && debug_ratelimit
					&& !printk_ratelimit()) {
			spin_unlock_irqrestore(&disable_ratelimit_lock, flags);
			return;
		}
		spin_unlock_irqrestore(&disable_ratelimit_lock, flags);

		len = snprintf(s, 256, KERN_DEBUG "%s: ", prefix);

		va_start(args, fmt);
		len += vsnprintf(&s[len], (256 - len), fmt, args);
		va_end(args);

		printk(s);

		WARN_ON(len < 5);
	}
}
EXPORT_SYMBOL(cpufreq_debug_printk);


module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, "CPUfreq debugging: add 1 to debug core,"
			" 2 to debug drivers, and 4 to debug governors.");

module_param(debug_ratelimit, uint, 0644);
MODULE_PARM_DESC(debug_ratelimit, "CPUfreq debugging:"
					" set to 0 to disable ratelimiting.");

#else /* !CONFIG_CPU_FREQ_DEBUG */

static inline void cpufreq_debug_enable_ratelimit(void) { return; }
static inline void cpufreq_debug_disable_ratelimit(void) { return; }

#endif /* CONFIG_CPU_FREQ_DEBUG */


/*********************************************************************
 *            EXTERNALLY AFFECTING FREQUENCY CHANGES                 *
 *********************************************************************/

/**
 * adjust_jiffies - adjust the system "loops_per_jiffy"
 *
 * This function alters the system "loops_per_jiffy" for the clock
 * speed change. Note that loops_per_jiffy cannot be updated on SMP
 * systems as each CPU might be scaled differently. So, use the arch
 * per-CPU loops_per_jiffy value wherever possible.
 */
#ifndef CONFIG_SMP
static unsigned long l_p_j_ref;
static unsigned int  l_p_j_ref_freq;

static void adjust_jiffies(unsigned long val, struct cpufreq_freqs *ci)
{
	if (ci->flags & CPUFREQ_CONST_LOOPS)
		return;

	if (!l_p_j_ref_freq) {
		l_p_j_ref = loops_per_jiffy;
		l_p_j_ref_freq = ci->old;
		dprintk("saving %lu as reference value for loops_per_jiffy; "
			"freq is %u kHz\n", l_p_j_ref, l_p_j_ref_freq);
	}
	if ((val == CPUFREQ_PRECHANGE  && ci->old < ci->new) ||
	    (val == CPUFREQ_POSTCHANGE && ci->old > ci->new) ||
	    (val == CPUFREQ_RESUMECHANGE || val == CPUFREQ_SUSPENDCHANGE)) {
		loops_per_jiffy = cpufreq_scale(l_p_j_ref, l_p_j_ref_freq,
								ci->new);
		dprintk("scaling loops_per_jiffy to %lu "
			"for frequency %u kHz\n", loops_per_jiffy, ci->new);
	}
}
#else
static inline void adjust_jiffies(unsigned long val, struct cpufreq_freqs *ci)
{
	return;
}
#endif


/**
 * cpufreq_notify_transition - call notifier chain and adjust_jiffies
 * on frequency transition.
 *
 * This function calls the transition notifiers and the "adjust_jiffies"
 * function. It is called twice on all CPU frequency changes that have
 * external effects.
 */
void cpufreq_notify_transition(struct cpufreq_freqs *freqs, unsigned int state)
{
	struct cpufreq_policy *policy;

	BUG_ON(irqs_disabled());

	freqs->flags = cpufreq_driver->flags;
	dprintk("notification %u of frequency transition to %u kHz\n",
		state, freqs->new);

	policy = per_cpu(cpufreq_cpu_data, freqs->cpu);
	switch (state) {

	case CPUFREQ_PRECHANGE:
		/* detect if the driver reported a value as "old frequency"
		 * which is not equal to what the cpufreq core thinks is
		 * "old frequency".
		 */
		if (!(cpufreq_driver->flags & CPUFREQ_CONST_LOOPS)) {
			if ((policy) && (policy->cpu == freqs->cpu) &&
			    (policy->cur) && (policy->cur != freqs->old)) {
				dprintk("Warning: CPU frequency is"
					" %u, cpufreq assumed %u kHz.\n",
					freqs->old, policy->cur);
				freqs->old = policy->cur;
			}
		}
		srcu_notifier_call_chain(&cpufreq_transition_notifier_list,
				CPUFREQ_PRECHANGE, freqs);
		adjust_jiffies(CPUFREQ_PRECHANGE, freqs);
		break;

	case CPUFREQ_POSTCHANGE:
		adjust_jiffies(CPUFREQ_POSTCHANGE, freqs);
		dprintk("FREQ: %lu - CPU: %lu", (unsigned long)freqs->new,
			(unsigned long)freqs->cpu);
		trace_power_frequency(POWER_PSTATE, freqs->new, freqs->cpu);
		trace_cpu_frequency(freqs->new, freqs->cpu);
		srcu_notifier_call_chain(&cpufreq_transition_notifier_list,
				CPUFREQ_POSTCHANGE, freqs);
		if (likely(policy) && likely(policy->cpu == freqs->cpu))
			policy->cur = freqs->new;
		break;
	}
}
EXPORT_SYMBOL_GPL(cpufreq_notify_transition);



/*********************************************************************
 *                          SYSFS INTERFACE                          *
 *********************************************************************/

static struct cpufreq_governor *__find_governor(const char *str_governor)
{
	struct cpufreq_governor *t;

	list_for_each_entry(t, &cpufreq_governor_list, governor_list)
		if (!strnicmp(str_governor, t->name, CPUFREQ_NAME_LEN))
			return t;

	return NULL;
}

/**
 * cpufreq_parse_governor - parse a governor string
 */
static int cpufreq_parse_governor(char *str_governor, unsigned int *policy,
				struct cpufreq_governor **governor)
{
	int err = -EINVAL;

	if (!cpufreq_driver)
		goto out;

	if (cpufreq_driver->setpolicy) {
		if (!strnicmp(str_governor, "performance", CPUFREQ_NAME_LEN)) {
			*policy = CPUFREQ_POLICY_PERFORMANCE;
			err = 0;
		} else if (!strnicmp(str_governor, "powersave",
						CPUFREQ_NAME_LEN)) {
			*policy = CPUFREQ_POLICY_POWERSAVE;
			err = 0;
		}
	} else if (cpufreq_driver->target) {
		struct cpufreq_governor *t;

		mutex_lock(&cpufreq_governor_mutex);

		t = __find_governor(str_governor);

		if (t == NULL) {
			int ret;

			mutex_unlock(&cpufreq_governor_mutex);
			ret = request_module("cpufreq_%s", str_governor);
			mutex_lock(&cpufreq_governor_mutex);

			if (ret == 0)
				t = __find_governor(str_governor);
		}

		if (t != NULL) {
			*governor = t;
			err = 0;
		}

		mutex_unlock(&cpufreq_governor_mutex);
	}
out:
	return err;
}


/**
 * cpufreq_per_cpu_attr_read() / show_##file_name() -
 * print out cpufreq information
 *
 * Write out information from cpufreq_driver->policy[cpu]; object must be
 * "unsigned int".
 */

#define show_one(file_name, object)			\
static ssize_t show_##file_name				\
(struct cpufreq_policy *policy, char *buf)		\
{							\
	return sprintf(buf, "%u\n", policy->object);	\
}

show_one(cpuinfo_min_freq, cpuinfo.min_freq);
show_one(cpuinfo_max_freq, cpuinfo.max_freq);
show_one(cpuinfo_transition_latency, cpuinfo.transition_latency);
show_one(scaling_min_freq, min);
show_one(scaling_max_freq, max);
show_one(scaling_cur_freq, cur);
show_one(policy_min_freq, user_policy.min);
show_one(policy_max_freq, user_policy.max);

static int __cpufreq_set_policy(struct cpufreq_policy *data,
				struct cpufreq_policy *policy);

/**
 * cpufreq_per_cpu_attr_write() / store_##file_name() - sysfs write access
 */
#define store_one(file_name, object)			\
static ssize_t store_##file_name					\
(struct cpufreq_policy *policy, const char *buf, size_t count)		\
{									\
	unsigned int ret = -EINVAL;					\
	struct cpufreq_policy new_policy;				\
									\
	ret = cpufreq_get_policy(&new_policy, policy->cpu);		\
	if (ret)							\
		return -EINVAL;						\
									\
	ret = sscanf(buf, "%u", &new_policy.object);			\
	if (ret != 1)							\
		return -EINVAL;						\
									\
	ret = __cpufreq_set_policy(policy, &new_policy);		\
	policy->user_policy.object = new_policy.object;			\
									\
	return ret ? ret : count;					\
}

store_one(scaling_min_freq, min);
store_one(scaling_max_freq, max);

/**
 * show_cpuinfo_cur_freq - current CPU frequency as detected by hardware
 */
static ssize_t show_cpuinfo_cur_freq(struct cpufreq_policy *policy,
					char *buf)
{
	unsigned int cur_freq = __cpufreq_get(policy->cpu);
	if (!cur_freq)
		return sprintf(buf, "<unknown>");
	return sprintf(buf, "%u\n", cur_freq);
}


/**
 * show_scaling_governor - show the current policy for the specified CPU
 */
static ssize_t show_scaling_governor(struct cpufreq_policy *policy, char *buf)
{
	if (policy->policy == CPUFREQ_POLICY_POWERSAVE)
		return sprintf(buf, "powersave\n");
	else if (policy->policy == CPUFREQ_POLICY_PERFORMANCE)
		return sprintf(buf, "performance\n");
	else if (policy->governor)
		return scnprintf(buf, CPUFREQ_NAME_LEN, "%s\n",
				policy->governor->name);
	return -EINVAL;
}


/**
 * store_scaling_governor - store policy for the specified CPU
 */
static ssize_t store_scaling_governor(struct cpufreq_policy *policy,
					const char *buf, size_t count)
{
	unsigned int ret = -EINVAL;
	char	str_governor[16];
	struct cpufreq_policy new_policy;
#ifdef CONFIG_HOTPLUG_CPU
	int cpu;
#endif

	ret = sscanf(buf, "%15s", str_governor);
	if (ret != 1)
		return -EINVAL;

	// maxwen: try to set govenor to all online cpus
	// else govvener will be set when cpu comes online the next time
#ifdef CONFIG_HOTPLUG_CPU
	for_each_present_cpu(cpu) {
		ret = cpufreq_get_policy(&new_policy, cpu);
		if (ret){
			continue;
		}

		if (cpufreq_parse_governor(str_governor, &new_policy.policy,
						&new_policy.governor)){

			continue;
		}

		/* Do not use cpufreq_set_policy here or the user_policy.max
	   	will be wrongly overridden */
		ret = __cpufreq_set_policy(policy, &new_policy);

		policy->user_policy.policy = policy->policy;
		policy->user_policy.governor = policy->governor;

		if (ret){
			continue;
		}
		printk(KERN_ERR "maxwen:setting govenor %s on cpu %d ok\n", str_governor, cpu);
	}

#endif
		return count;
}

/**
 * show_scaling_driver - show the cpufreq driver currently loaded
 */
static ssize_t show_scaling_driver(struct cpufreq_policy *policy, char *buf)
{
	return scnprintf(buf, CPUFREQ_NAME_LEN, "%s\n", cpufreq_driver->name);
}

/**
 * show_scaling_available_governors - show the available CPUfreq governors
 */
static ssize_t show_scaling_available_governors(struct cpufreq_policy *policy,
						char *buf)
{
	ssize_t i = 0;
	struct cpufreq_governor *t;

	if (!cpufreq_driver->target) {
		i += sprintf(buf, "performance powersave");
		goto out;
	}

	list_for_each_entry(t, &cpufreq_governor_list, governor_list) {
		if (i >= (ssize_t) ((PAGE_SIZE / sizeof(char))
		    - (CPUFREQ_NAME_LEN + 2)))
			goto out;
		i += scnprintf(&buf[i], CPUFREQ_NAME_LEN, "%s ", t->name);
	}
out:
	i += sprintf(&buf[i], "\n");
	return i;
}

static ssize_t show_cpus(const struct cpumask *mask, char *buf)
{
	ssize_t i = 0;
	unsigned int cpu;

	for_each_cpu(cpu, mask) {
		if (i)
			i += scnprintf(&buf[i], (PAGE_SIZE - i - 2), " ");
		i += scnprintf(&buf[i], (PAGE_SIZE - i - 2), "%u", cpu);
		if (i >= (PAGE_SIZE - 5))
			break;
	}
	i += sprintf(&buf[i], "\n");
	return i;
}

/**
 * show_related_cpus - show the CPUs affected by each transition even if
 * hw coordination is in use
 */
static ssize_t show_related_cpus(struct cpufreq_policy *policy, char *buf)
{
	if (cpumask_empty(policy->related_cpus))
		return show_cpus(policy->cpus, buf);
	return show_cpus(policy->related_cpus, buf);
}

/**
 * show_affected_cpus - show the CPUs affected by each transition
 */
static ssize_t show_affected_cpus(struct cpufreq_policy *policy, char *buf)
{
	return show_cpus(policy->cpus, buf);
}

static ssize_t store_scaling_setspeed(struct cpufreq_policy *policy,
					const char *buf, size_t count)
{
	unsigned int freq = 0;
	unsigned int ret;

	if (!policy->governor || !policy->governor->store_setspeed)
		return -EINVAL;

	ret = sscanf(buf, "%u", &freq);
	if (ret != 1)
		return -EINVAL;

	policy->governor->store_setspeed(policy, freq);

	return count;
}

static ssize_t show_scaling_setspeed(struct cpufreq_policy *policy, char *buf)
{
	if (!policy->governor || !policy->governor->show_setspeed)
		return sprintf(buf, "<unsupported>\n");

	return policy->governor->show_setspeed(policy, buf);
}

/**
 * show_scaling_driver - show the current cpufreq HW/BIOS limitation
 */
static ssize_t show_bios_limit(struct cpufreq_policy *policy, char *buf)
{
	unsigned int limit;
	int ret;
	if (cpufreq_driver->bios_limit) {
		ret = cpufreq_driver->bios_limit(policy->cpu, &limit);
		if (!ret)
			return sprintf(buf, "%u\n", limit);
	}
	return sprintf(buf, "%u\n", policy->cpuinfo.max_freq);
}

#ifdef CONFIG_VOLTAGE_CONTROL
/*
 * Tegra3 voltage control via cpufreq by Paul Reioux (faux123)
 * inspired by Michael Huang's voltage control code for OMAP44xx
 * Modded by iodak for p880 (X3)
 */


extern int user_mv_table[MAX_DVFS_FREQS];

static ssize_t show_UV_mV_table(struct cpufreq_policy *policy, char *buf)
{
	int i = 0;
	char *out = buf;
	struct clk *cpu_clk_g = tegra_get_clock_by_name("cpu_g");

	/* find how many actual entries there are */
	i = cpu_clk_g->dvfs->num_freqs;

	for(i--; i >=0; i--) {
		if (cpu_clk_g->dvfs->freqs[i] != cpu_clk_g->dvfs->freqs[i-1]){
		out += sprintf(out, "%lumhz: %i mV\n",
				cpu_clk_g->dvfs->freqs[i]/1000000, //it will show cpu G freqs diferrent from freq table need fix
				cpu_clk_g->dvfs->millivolts[i]);
		}
	}

	return out - buf;
}

static ssize_t store_UV_mV_table(struct cpufreq_policy *policy, char *buf, size_t count)
{
	int i = 0;
	unsigned long volt_cur;
	int ret;
	char size_cur[16];

	struct clk *cpu_clk_g = tegra_get_clock_by_name("cpu_g");

	/* find how many actual entries there are */
	i = cpu_clk_g->dvfs->num_freqs;

	for(i--; i >= 0; i--) {
		if (cpu_clk_g->dvfs->freqs[i] != cpu_clk_g->dvfs->freqs[i-1] &&
		 cpu_clk_g->dvfs->freqs[i]/1000000 != 0){
			ret = sscanf(buf, "%lu", &volt_cur);
			if (ret != 1)
				return -EINVAL;

			if (volt_cur >= 725 && volt_cur <= 1273){
			user_mv_table[i] = volt_cur;
			pr_info("user mv tbl[%i]: %lu\n", i, volt_cur);
			}
			/* Non-standard sysfs interface: advance buf */
			ret = sscanf(buf, "%s", size_cur);
			if (ret == 0)
			return 0;
			buf += (strlen(size_cur)+1);
			}
		}
	/* update dvfs table here */
	cpu_clk_g->dvfs->millivolts = user_mv_table;

	return count;
}

static ssize_t show_lp_UV_mV_table(struct cpufreq_policy *policy, char *buf)
{

	char *out = buf;
	unsigned int freqs_lp [6]={51, 102, 204, 370, 475, 513}; //fake freqs
	struct clk *cpu_clk_lp = tegra_get_clock_by_name("cpu_lp");
	int i = cpu_clk_lp->dvfs->num_freqs-3;

	for(i--; i >= 0; i--){
		out += sprintf(out, "%imhz: %i mV\n", freqs_lp[i], cpu_clk_lp->dvfs->millivolts[i]);
		}

	return out - buf;
}

static ssize_t store_lp_UV_mV_table(struct cpufreq_policy *policy, char *buf, size_t count)
{
	int i;
	struct clk *cpu_clk_lp = tegra_get_clock_by_name("cpu_lp");
	struct clk *clk_emc = tegra_get_clock_by_name("emc");
	unsigned long volt_cur[cpu_clk_lp->dvfs->num_freqs-3];
	int ret = 0;
	
	ret = sscanf(buf, "%lu %lu %lu %lu %lu %lu", &volt_cur[5], &volt_cur[4], &volt_cur[3], &volt_cur[2], &volt_cur[1], &volt_cur[0]);

	if (ret != 6)
	return -EINVAL;
	
	for(i = 0; i < 6; i++){
		if(volt_cur[i] < 900){
		printk(KERN_DEBUG "lp_voltage_control: You set too low voltage (%lu) set min to 900mV\n", volt_cur[i]);
		volt_cur[i] = 900;
		}
		if(volt_cur[i] > 1350){
		printk(KERN_DEBUG "lp_voltage_control: You set too high voltage (%lu) set max to 1350mV\n", volt_cur[i]);
		volt_cur[i] = 1350;
		}

		cpu_clk_lp->dvfs->millivolts[i] = volt_cur[i];
		clk_emc->dvfs->millivolts[i] = volt_cur[i];
		printk(KERN_DEBUG "lp_voltage_control: Voltages are set to: %i mV\n", cpu_clk_lp->dvfs->millivolts[i]);
	}

	return count;
}
#endif

						
#ifdef CONFIG_GPU_OVERCLOCK
static ssize_t show_gpu_overclock(struct cpufreq_policy *policy, char *buf) {

	char *out = buf;
  	struct clk *clk_3d = tegra_get_clock_by_name("3d");
  	unsigned int i;

	for(i = 0; i < 6; i++){
		if (clk_3d->dvfs->freqs[i]/1000000 != 0)
 		out += sprintf(out, "%lu ",clk_3d->dvfs->freqs[i] / 1000000);
		}

	return out - buf;
}

static ssize_t store_gpu_overclock(struct cpufreq_policy *policy, const char *buf, size_t count)
{

	int ret;
	struct clk *clk_vde = tegra_get_clock_by_name("vde");
	struct clk *clk_mpe = tegra_get_clock_by_name("mpe");
	struct clk *clk_2d = tegra_get_clock_by_name("2d");
	struct clk *clk_epp = tegra_get_clock_by_name("epp");
	struct clk *clk_3d = tegra_get_clock_by_name("3d");
	struct clk *clk_3d2 = tegra_get_clock_by_name("3d2");
	struct clk *clk_se = tegra_get_clock_by_name("se");
	struct clk *clk_cbus = tegra_get_clock_by_name("cbus");
	struct clk *clk_host1x = tegra_get_clock_by_name("host1x");
	struct clk *clk_pll_c = tegra_get_clock_by_name("pll_c");
	struct clk *shared_bus_user;
	unsigned int i, v;
	unsigned long freq_cur[6];
	unsigned int stock_voltages[6]={950, 1000, 1050, 1100, 1150, 1200};
	unsigned int stock_pll_freqs[6]={533000, 667000, 667000, 800000, 800000, 1066000};

	ret = sscanf(buf, "%lu %lu %lu %lu %lu %lu", &freq_cur[0], &freq_cur[1], &freq_cur[2], &freq_cur[3], &freq_cur[4], &freq_cur[5]);

			if (ret != 6)
				return -EINVAL;

			mutex_lock(&dvfs_lock);

			for(i = 0; i < 6; i++) {
			if (freq_cur[i] < 200){
				printk(KERN_DEBUG "GPU_OC: You set to low freq (%lu) set min to 200\n", freq_cur[i]);
				freq_cur[i] = 200;
				}
			if (freq_cur[i] > 600){
				printk(KERN_DEBUG "GPU_OC: You set to high freq (%lu) set max to 600\n", freq_cur[i]);
				freq_cur[i] = 600;
				}
			if (freq_cur[i] > 520){
				v = 1250; 
				clk_vde->dvfs->millivolts[i] = v;
				clk_mpe->dvfs->millivolts[i] = v;
				clk_2d->dvfs->millivolts[i] = v;
				clk_epp->dvfs->millivolts[i] = v;
				clk_3d->dvfs->millivolts[i] = v;
				clk_3d2->dvfs->millivolts[i] = v;
				clk_se->dvfs->millivolts[i] = v;
				clk_cbus->dvfs->millivolts[i] = v;
				clk_host1x->dvfs->millivolts[i] = v;
				clk_pll_c->dvfs->millivolts[i] = v;
				printk(KERN_DEBUG "GPU_OC: Voltages are set to: %i mV for clock: %lu MHz\n", clk_3d->dvfs->millivolts[i], freq_cur[i] );
				}
			if (freq_cur[i] <= 520){
				clk_vde->dvfs->millivolts[i] = stock_voltages[i];
				clk_mpe->dvfs->millivolts[i] = stock_voltages[i];
				clk_2d->dvfs->millivolts[i] = stock_voltages[i];
				clk_epp->dvfs->millivolts[i] = stock_voltages[i];
				clk_3d->dvfs->millivolts[i] = stock_voltages[i];
				clk_3d2->dvfs->millivolts[i] = stock_voltages[i];
				clk_se->dvfs->millivolts[i] = stock_voltages[i];
				clk_cbus->dvfs->millivolts[i] = stock_voltages[i];
				clk_host1x->dvfs->millivolts[i] = stock_voltages[i];
				clk_pll_c->dvfs->millivolts[i] = stock_voltages[i];
				printk(KERN_DEBUG "GPU_OC: Voltages are set to: %i mV for clock: %lu MHz\n", clk_3d->dvfs->millivolts[i], freq_cur[i] );
				}
			}
			

			clk_vde->max_rate = freq_cur[5]*1000000;
			clk_mpe->max_rate = freq_cur[5]*1000000;
			clk_2d->max_rate = freq_cur[5]*1000000;
			clk_epp->max_rate = freq_cur[5]*1000000;
			clk_3d->max_rate = freq_cur[5]*1000000;
			clk_3d2->max_rate = freq_cur[5]*1000000;
			clk_se->max_rate = freq_cur[5]*1000000;
			clk_cbus->max_rate = freq_cur[5]*1000000;
			clk_host1x->max_rate = DIV_ROUND_UP((freq_cur[5]*1000000), 2);
			clk_pll_c->max_rate = freq_cur[5]*2000000;
			list_for_each_entry(shared_bus_user, &clk_cbus->shared_bus_list, u.shared_bus_user.node) {
			shared_bus_user->max_rate = clk_cbus->max_rate; 
			}

			for(i = 6; i < 9; i++) {
			clk_vde->dvfs->freqs[i] = freq_cur[5]*1000000;  //need to set them to value of largest rate
			clk_mpe->dvfs->freqs[i] = freq_cur[5]*1000000;  //or silence warning in dvfs.c
			clk_2d->dvfs->freqs[i] = freq_cur[5]*1000000;
			clk_epp->dvfs->freqs[i] = freq_cur[5]*1000000;
			clk_3d->dvfs->freqs[i] = freq_cur[5]*1000000;
			clk_3d2->dvfs->freqs[i] = freq_cur[5]*1000000;
			clk_se->dvfs->freqs[i] = freq_cur[5]*1000000;
			clk_cbus->dvfs->freqs[i] = freq_cur[5]*1000000;
			clk_host1x->dvfs->freqs[i] = DIV_ROUND_UP((freq_cur[5]*1000000), 2);
			if(freq_cur[5]*2000000 <= stock_pll_freqs[5])
				clk_pll_c->dvfs->freqs[i] = stock_pll_freqs[5];
			else
				clk_pll_c->dvfs->freqs[i] = freq_cur[5]*1000000;
			}
				
			for(i = 0; i < 6; i++) {
			clk_vde->dvfs->freqs[i] = freq_cur[i]*1000000;
			clk_mpe->dvfs->freqs[i] = freq_cur[i]*1000000;
			clk_2d->dvfs->freqs[i] = freq_cur[i]*1000000;
			clk_epp->dvfs->freqs[i] = freq_cur[i]*1000000;
			clk_3d->dvfs->freqs[i] = freq_cur[i]*1000000;
			clk_3d2->dvfs->freqs[i] = freq_cur[i]*1000000;
			clk_se->dvfs->freqs[i] = freq_cur[i]*1000000;
			clk_cbus->dvfs->freqs[i] = freq_cur[i]*1000000;
			clk_host1x->dvfs->freqs[i] = DIV_ROUND_UP((freq_cur[i]*1000000), 2);
			if(freq_cur[i]*2000000 <= stock_pll_freqs[i])
				clk_pll_c->dvfs->freqs[i] = stock_pll_freqs[i];
			else
				clk_pll_c->dvfs->freqs[i] = freq_cur[i]*1000000;
			}

			mutex_unlock(&dvfs_lock);

	return count;
}
#endif

static ssize_t show_tegra_cpu_variant(struct cpufreq_policy *policy, char *buf, size_t count)
{
	int cpu_process_id = tegra_cpu_process_id();
	char *out = buf;

	if (cpu_process_id == 1 || cpu_process_id == 0)
	out += sprintf(out, "tegra_variant is %i, CPU is weak sorry :(\n",
			cpu_process_id);
	if (cpu_process_id == 3 || cpu_process_id == 2)
	out += sprintf(out, "tegra_variant is %i, CPU is strong uhuuuu! :)\n",
			cpu_process_id);
	else
	out += sprintf(out, "tegra_variant is %i\n",
			cpu_process_id);
	return out - buf;
}

static ssize_t show_gpu_cur_freq(struct cpufreq_policy *policy, char *buf, size_t count)
{
	struct clk *clk_3d = tegra_get_clock_by_name("3d");
	struct clk *clk_2d = tegra_get_clock_by_name("2d");
	char *out = buf;

	out += sprintf(out, "3d: %lu MHz *** 2d: %lu MHz\n",
				clk_get_rate(clk_3d) / 1000000,
				clk_get_rate(clk_2d) / 1000000);
	return out - buf;
}

cpufreq_freq_attr_ro_perm(cpuinfo_cur_freq, 0400);
cpufreq_freq_attr_ro(cpuinfo_min_freq);
cpufreq_freq_attr_ro(cpuinfo_max_freq);
cpufreq_freq_attr_ro(cpuinfo_transition_latency);
cpufreq_freq_attr_ro(scaling_available_governors);
cpufreq_freq_attr_ro(scaling_driver);
cpufreq_freq_attr_ro(scaling_cur_freq);
cpufreq_freq_attr_ro(bios_limit);
cpufreq_freq_attr_ro(related_cpus);
cpufreq_freq_attr_ro(affected_cpus);
cpufreq_freq_attr_rw(scaling_min_freq);
cpufreq_freq_attr_rw(scaling_max_freq);
cpufreq_freq_attr_rw(scaling_governor);
cpufreq_freq_attr_rw(scaling_setspeed);
cpufreq_freq_attr_ro(policy_min_freq);
cpufreq_freq_attr_ro(policy_max_freq);
#ifdef CONFIG_VOLTAGE_CONTROL
cpufreq_freq_attr_rw(UV_mV_table);
cpufreq_freq_attr_rw(lp_UV_mV_table);
#endif
#ifdef CONFIG_GPU_OVERCLOCK
cpufreq_freq_attr_rw(gpu_overclock);
#endif
cpufreq_freq_attr_ro(tegra_cpu_variant);
cpufreq_freq_attr_ro(gpu_cur_freq);

static struct attribute *default_attrs[] = {
	&cpuinfo_min_freq.attr,
	&cpuinfo_max_freq.attr,
	&cpuinfo_transition_latency.attr,
	&scaling_min_freq.attr,
	&scaling_max_freq.attr,
	&affected_cpus.attr,
	&related_cpus.attr,
	&scaling_governor.attr,
	&scaling_driver.attr,
	&scaling_available_governors.attr,
	&scaling_setspeed.attr,
	&policy_min_freq.attr,
	&policy_max_freq.attr,
#ifdef CONFIG_VOLTAGE_CONTROL
	&UV_mV_table.attr,
	&lp_UV_mV_table.attr,
#endif
#ifdef CONFIG_GPU_OVERCLOCK
	&gpu_overclock.attr,
#endif
	&tegra_cpu_variant.attr,
	&gpu_cur_freq.attr,
	NULL
};

struct kobject *cpufreq_global_kobject;
EXPORT_SYMBOL(cpufreq_global_kobject);

#define to_policy(k) container_of(k, struct cpufreq_policy, kobj)
#define to_attr(a) container_of(a, struct freq_attr, attr)

static ssize_t show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct cpufreq_policy *policy = to_policy(kobj);
	struct freq_attr *fattr = to_attr(attr);
	ssize_t ret = -EINVAL;
	policy = cpufreq_cpu_get(policy->cpu);
	if (!policy)
		goto no_policy;

	if (lock_policy_rwsem_read(policy->cpu) < 0)
		goto fail;

	if (fattr->show)
		ret = fattr->show(policy, buf);
	else
		ret = -EIO;

	unlock_policy_rwsem_read(policy->cpu);
fail:
	cpufreq_cpu_put(policy);
no_policy:
	return ret;
}

static ssize_t store(struct kobject *kobj, struct attribute *attr,
		     const char *buf, size_t count)
{
	struct cpufreq_policy *policy = to_policy(kobj);
	struct freq_attr *fattr = to_attr(attr);
	ssize_t ret = -EINVAL;
	policy = cpufreq_cpu_get(policy->cpu);
	if (!policy)
		goto no_policy;

	if (lock_policy_rwsem_write(policy->cpu) < 0)
		goto fail;

	if (fattr->store)
		ret = fattr->store(policy, buf, count);
	else
		ret = -EIO;

	unlock_policy_rwsem_write(policy->cpu);
fail:
	cpufreq_cpu_put(policy);
no_policy:
	return ret;
}

static void cpufreq_sysfs_release(struct kobject *kobj)
{
	struct cpufreq_policy *policy = to_policy(kobj);
	dprintk("last reference is dropped\n");
	complete(&policy->kobj_unregister);
}

static const struct sysfs_ops sysfs_ops = {
	.show	= show,
	.store	= store,
};

static struct kobj_type ktype_cpufreq = {
	.sysfs_ops	= &sysfs_ops,
	.default_attrs	= default_attrs,
	.release	= cpufreq_sysfs_release,
};

/*
 * Returns:
 *   Negative: Failure
 *   0:        Success
 *   Positive: When we have a managed CPU and the sysfs got symlinked
 */
static int cpufreq_add_dev_policy(unsigned int cpu,
				  struct cpufreq_policy *policy,
				  struct sys_device *sys_dev)
{
	int ret = 0;
#ifdef CONFIG_SMP
	unsigned long flags;
	unsigned int j;

// maxwen: we have already set the policy that we
// want to use in cpufreq_add_dev
// this makes sure that al cpus use the same governor

#if 0
#ifdef CONFIG_HOTPLUG_CPU
	struct cpufreq_governor *gov;

	gov = __find_governor(per_cpu(cpufreq_cpu_governor, cpu));
	if (gov) {
		policy->governor = gov;
		dprintk("Restoring governor %s for cpu %d\n",
		       policy->governor->name, cpu);
	}
#endif
#endif
	for_each_cpu(j, policy->cpus) {
		struct cpufreq_policy *managed_policy;

		if (cpu == j)
			continue;

		/* Check for existing affected CPUs.
		 * They may not be aware of it due to CPU Hotplug.
		 * cpufreq_cpu_put is called when the device is removed
		 * in __cpufreq_remove_dev()
		 */
		managed_policy = cpufreq_cpu_get(j);
		if (unlikely(managed_policy)) {

			/* Set proper policy_cpu */
			unlock_policy_rwsem_write(cpu);
			per_cpu(cpufreq_policy_cpu, cpu) = managed_policy->cpu;

			if (lock_policy_rwsem_write(cpu) < 0) {
				/* Should not go through policy unlock path */
				if (cpufreq_driver->exit)
					cpufreq_driver->exit(policy);
				cpufreq_cpu_put(managed_policy);
				return -EBUSY;
			}

			spin_lock_irqsave(&cpufreq_driver_lock, flags);
			cpumask_copy(managed_policy->cpus, policy->cpus);
			per_cpu(cpufreq_cpu_data, cpu) = managed_policy;
			spin_unlock_irqrestore(&cpufreq_driver_lock, flags);

			dprintk("CPU already managed, adding link\n");
			ret = sysfs_create_link(&sys_dev->kobj,
						&managed_policy->kobj,
						"cpufreq");
			if (ret)
				cpufreq_cpu_put(managed_policy);
			/*
			 * Success. We only needed to be added to the mask.
			 * Call driver->exit() because only the cpu parent of
			 * the kobj needed to call init().
			 */
			if (cpufreq_driver->exit)
				cpufreq_driver->exit(policy);

			if (!ret)
				return 1;
			else
				return ret;
		}
	}
#endif
	return ret;
}


/* symlink affected CPUs */
static int cpufreq_add_dev_symlink(unsigned int cpu,
				   struct cpufreq_policy *policy)
{
	unsigned int j;
	int ret = 0;

	for_each_cpu(j, policy->cpus) {
		struct cpufreq_policy *managed_policy;
		struct sys_device *cpu_sys_dev;

		if (j == cpu)
			continue;
		if (!cpu_online(j))
			continue;

		dprintk("CPU %u already managed, adding link\n", j);
		managed_policy = cpufreq_cpu_get(cpu);
		cpu_sys_dev = get_cpu_sysdev(j);
		ret = sysfs_create_link(&cpu_sys_dev->kobj, &policy->kobj,
					"cpufreq");
		if (ret) {
			cpufreq_cpu_put(managed_policy);
			return ret;
		}
	}
	return ret;
}

static int cpufreq_add_dev_interface(unsigned int cpu,
				     struct cpufreq_policy *policy,
				     struct sys_device *sys_dev)
{
	struct cpufreq_policy new_policy;
	struct freq_attr **drv_attr;
	unsigned long flags;
	int ret = 0;
	unsigned int j;

	/* prepare interface data */
	ret = kobject_init_and_add(&policy->kobj, &ktype_cpufreq,
				   &sys_dev->kobj, "cpufreq");
	if (ret)
		return ret;

	/* set up files for this cpu device */
	drv_attr = cpufreq_driver->attr;
	while ((drv_attr) && (*drv_attr)) {
		ret = sysfs_create_file(&policy->kobj, &((*drv_attr)->attr));
		if (ret)
			goto err_out_kobj_put;
		drv_attr++;
	}
	if (cpufreq_driver->get) {
		ret = sysfs_create_file(&policy->kobj, &cpuinfo_cur_freq.attr);
		if (ret)
			goto err_out_kobj_put;
	}
	if (cpufreq_driver->target) {
		ret = sysfs_create_file(&policy->kobj, &scaling_cur_freq.attr);
		if (ret)
			goto err_out_kobj_put;
	}
	if (cpufreq_driver->bios_limit) {
		ret = sysfs_create_file(&policy->kobj, &bios_limit.attr);
		if (ret)
			goto err_out_kobj_put;
	}

	spin_lock_irqsave(&cpufreq_driver_lock, flags);
	for_each_cpu(j, policy->cpus) {
		if (!cpu_online(j))
			continue;
		per_cpu(cpufreq_cpu_data, j) = policy;
		per_cpu(cpufreq_policy_cpu, j) = policy->cpu;
	}
	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);

	ret = cpufreq_add_dev_symlink(cpu, policy);
	if (ret)
		goto err_out_kobj_put;

	memcpy(&new_policy, policy, sizeof(struct cpufreq_policy));
	/* assure that the starting sequence is run in __cpufreq_set_policy */
	policy->governor = NULL;

	/* set default policy */
	ret = __cpufreq_set_policy(policy, &new_policy);
	policy->user_policy.policy = policy->policy;
	policy->user_policy.governor = policy->governor;

	if (ret) {
		dprintk("setting policy failed\n");
		if (cpufreq_driver->exit)
			cpufreq_driver->exit(policy);
	}
	return ret;

err_out_kobj_put:
	kobject_put(&policy->kobj);
	wait_for_completion(&policy->kobj_unregister);
	return ret;
}


/**
 * cpufreq_add_dev - add a CPU device
 *
 * Adds the cpufreq interface for a CPU device.
 *
 * The Oracle says: try running cpufreq registration/unregistration concurrently
 * with with cpu hotplugging and all hell will break loose. Tried to clean this
 * mess up, but more thorough testing is needed. - Mathieu
 */
static int cpufreq_add_dev(struct sys_device *sys_dev)
{
	unsigned int cpu = sys_dev->id;
	int ret = 0, found = 0;
	struct cpufreq_policy *policy;
	unsigned long flags;
	unsigned int j;
#ifdef CONFIG_HOTPLUG_CPU
	int sibling;
#endif

	if (cpu_is_offline(cpu))
		return 0;

	cpufreq_debug_disable_ratelimit();
	dprintk("adding CPU %u\n", cpu);

#ifdef CONFIG_SMP
	/* check whether a different CPU already registered this
	 * CPU because it is in the same boat. */
	policy = cpufreq_cpu_get(cpu);
	if (unlikely(policy)) {
		cpufreq_cpu_put(policy);
		cpufreq_debug_enable_ratelimit();
		return 0;
	}
#endif

	if (!try_module_get(cpufreq_driver->owner)) {
		ret = -EINVAL;
		goto module_out;
	}

	ret = -ENOMEM;
	policy = kzalloc(sizeof(struct cpufreq_policy), GFP_KERNEL);
	if (!policy)
		goto nomem_out;

	if (!alloc_cpumask_var(&policy->cpus, GFP_KERNEL))
		goto err_free_policy;

	if (!zalloc_cpumask_var(&policy->related_cpus, GFP_KERNEL))
		goto err_free_cpumask;

	policy->cpu = cpu;
	cpumask_copy(policy->cpus, cpumask_of(cpu));

	/* Initially set CPU itself as the policy_cpu */
	per_cpu(cpufreq_policy_cpu, cpu) = cpu;
	ret = (lock_policy_rwsem_write(cpu) < 0);
	WARN_ON(ret);

	init_completion(&policy->kobj_unregister);
	INIT_WORK(&policy->update, handle_update);

	/* Set governor before ->init, so that driver could check it */
#ifdef CONFIG_HOTPLUG_CPU
	for_each_online_cpu(sibling) {
		struct cpufreq_policy *cp = per_cpu(cpufreq_cpu_data, sibling);
		if (cp && cp->governor &&
		    (cpumask_test_cpu(cpu, cp->related_cpus))) {
			policy->governor = cp->governor;
			found = 1;
			break;
		}
	}
#endif
	f (!found){
     policy->governor = CPUFREQ_DEFAULT_GOVERNOR;
  }

  printk(KERN_ERR "maxwen: set govener for cpu %d to %s\n", cpu, policy->governor->name); 
	/* call driver. From then on the cpufreq must be able
	 * to accept all calls to ->verify and ->setpolicy for this CPU
	 */
	ret = cpufreq_driver->init(policy);
	if (ret) {
		dprintk("initialization failed\n");
		goto err_unlock_policy;
	}
	policy->user_policy.min = policy->min;
	policy->user_policy.max = policy->max;

	blocking_notifier_call_chain(&cpufreq_policy_notifier_list,
				     CPUFREQ_START, policy);

	ret = cpufreq_add_dev_policy(cpu, policy, sys_dev);
	if (ret) {
		if (ret > 0)
			/* This is a managed cpu, symlink created,
			   exit with 0 */
			ret = 0;
		goto err_unlock_policy;
	}

	ret = cpufreq_add_dev_interface(cpu, policy, sys_dev);
	if (ret)
		goto err_out_unregister;

	unlock_policy_rwsem_write(cpu);

	kobject_uevent(&policy->kobj, KOBJ_ADD);
	module_put(cpufreq_driver->owner);
	dprintk("initialization complete\n");
	cpufreq_debug_enable_ratelimit();

	return 0;


err_out_unregister:
	spin_lock_irqsave(&cpufreq_driver_lock, flags);
	for_each_cpu(j, policy->cpus)
		per_cpu(cpufreq_cpu_data, j) = NULL;
	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);

	kobject_put(&policy->kobj);
	wait_for_completion(&policy->kobj_unregister);

err_unlock_policy:
	unlock_policy_rwsem_write(cpu);
	free_cpumask_var(policy->related_cpus);
err_free_cpumask:
	free_cpumask_var(policy->cpus);
err_free_policy:
	kfree(policy);
nomem_out:
	module_put(cpufreq_driver->owner);
module_out:
	cpufreq_debug_enable_ratelimit();
	return ret;
}


/**
 * __cpufreq_remove_dev - remove a CPU device
 *
 * Removes the cpufreq interface for a CPU device.
 * Caller should already have policy_rwsem in write mode for this CPU.
 * This routine frees the rwsem before returning.
 */
static int __cpufreq_remove_dev(struct sys_device *sys_dev)
{
	unsigned int cpu = sys_dev->id;
	unsigned long flags;
	struct cpufreq_policy *data;
	struct kobject *kobj;
	struct completion *cmp;
#ifdef CONFIG_SMP
	struct sys_device *cpu_sys_dev;
	unsigned int j;
#endif

	cpufreq_debug_disable_ratelimit();
	dprintk("unregistering CPU %u\n", cpu);

	spin_lock_irqsave(&cpufreq_driver_lock, flags);
	data = per_cpu(cpufreq_cpu_data, cpu);

	if (!data) {
		spin_unlock_irqrestore(&cpufreq_driver_lock, flags);
		cpufreq_debug_enable_ratelimit();
		unlock_policy_rwsem_write(cpu);
		return -EINVAL;
	}
	per_cpu(cpufreq_cpu_data, cpu) = NULL;


#ifdef CONFIG_SMP
	/* if this isn't the CPU which is the parent of the kobj, we
	 * only need to unlink, put and exit
	 */
	if (unlikely(cpu != data->cpu)) {
		dprintk("removing link\n");
		cpumask_clear_cpu(cpu, data->cpus);
		spin_unlock_irqrestore(&cpufreq_driver_lock, flags);
		kobj = &sys_dev->kobj;
		cpufreq_cpu_put(data);
		cpufreq_debug_enable_ratelimit();
		unlock_policy_rwsem_write(cpu);
		sysfs_remove_link(kobj, "cpufreq");
		return 0;
	}
#endif

#ifdef CONFIG_SMP

#ifdef CONFIG_HOTPLUG_CPU
	strncpy(per_cpu(cpufreq_cpu_governor, cpu), data->governor->name,
			CPUFREQ_NAME_LEN);
#endif

	/* if we have other CPUs still registered, we need to unlink them,
	 * or else wait_for_completion below will lock up. Clean the
	 * per_cpu(cpufreq_cpu_data) while holding the lock, and remove
	 * the sysfs links afterwards.
	 */
	if (unlikely(cpumask_weight(data->cpus) > 1)) {
		for_each_cpu(j, data->cpus) {
			if (j == cpu)
				continue;
			per_cpu(cpufreq_cpu_data, j) = NULL;
		}
	}

	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);

	if (unlikely(cpumask_weight(data->cpus) > 1)) {
		for_each_cpu(j, data->cpus) {
			if (j == cpu)
				continue;
			dprintk("removing link for cpu %u\n", j);
#ifdef CONFIG_HOTPLUG_CPU
			strncpy(per_cpu(cpufreq_cpu_governor, j),
				data->governor->name, CPUFREQ_NAME_LEN);
#endif
			cpu_sys_dev = get_cpu_sysdev(j);
			kobj = &cpu_sys_dev->kobj;
			unlock_policy_rwsem_write(cpu);
			sysfs_remove_link(kobj, "cpufreq");
			lock_policy_rwsem_write(cpu);
			cpufreq_cpu_put(data);
		}
	}
#else
	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);
#endif

	if (cpufreq_driver->target)
		__cpufreq_governor(data, CPUFREQ_GOV_STOP);

	kobj = &data->kobj;
	cmp = &data->kobj_unregister;
	unlock_policy_rwsem_write(cpu);
	kobject_put(kobj);

	/* we need to make sure that the underlying kobj is actually
	 * not referenced anymore by anybody before we proceed with
	 * unloading.
	 */
	dprintk("waiting for dropping of refcount\n");
	wait_for_completion(cmp);
	dprintk("wait complete\n");

	lock_policy_rwsem_write(cpu);
	if (cpufreq_driver->exit)
		cpufreq_driver->exit(data);
	unlock_policy_rwsem_write(cpu);

	cpufreq_debug_enable_ratelimit();

#ifdef CONFIG_HOTPLUG_CPU
	/* when the CPU which is the parent of the kobj is hotplugged
	 * offline, check for siblings, and create cpufreq sysfs interface
	 * and symlinks
	 */
	if (unlikely(cpumask_weight(data->cpus) > 1)) {
		/* first sibling now owns the new sysfs dir */
		cpumask_clear_cpu(cpu, data->cpus);
		cpufreq_add_dev(get_cpu_sysdev(cpumask_first(data->cpus)));

		/* finally remove our own symlink */
		lock_policy_rwsem_write(cpu);
		__cpufreq_remove_dev(sys_dev);
	}
#endif

	free_cpumask_var(data->related_cpus);
	free_cpumask_var(data->cpus);
	kfree(data);

	return 0;
}


static int cpufreq_remove_dev(struct sys_device *sys_dev)
{
	unsigned int cpu = sys_dev->id;
	int retval;

	if (cpu_is_offline(cpu))
		return 0;

	if (unlikely(lock_policy_rwsem_write(cpu)))
		BUG();

	retval = __cpufreq_remove_dev(sys_dev);
	return retval;
}


static void handle_update(struct work_struct *work)
{
	struct cpufreq_policy *policy =
		container_of(work, struct cpufreq_policy, update);
	unsigned int cpu = policy->cpu;
	dprintk("handle_update for cpu %u called\n", cpu);
	cpufreq_update_policy(cpu);
}

/**
 *	cpufreq_out_of_sync - If actual and saved CPU frequency differs, we're in deep trouble.
 *	@cpu: cpu number
 *	@old_freq: CPU frequency the kernel thinks the CPU runs at
 *	@new_freq: CPU frequency the CPU actually runs at
 *
 *	We adjust to current frequency first, and need to clean up later.
 *	So either call to cpufreq_update_policy() or schedule handle_update()).
 */
static void cpufreq_out_of_sync(unsigned int cpu, unsigned int old_freq,
				unsigned int new_freq)
{
	struct cpufreq_freqs freqs;

	dprintk("Warning: CPU frequency out of sync: cpufreq and timing "
	       "core thinks of %u, is %u kHz.\n", old_freq, new_freq);

	freqs.cpu = cpu;
	freqs.old = old_freq;
	freqs.new = new_freq;
	cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
	cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
}


/**
 * cpufreq_quick_get - get the CPU frequency (in kHz) from policy->cur
 * @cpu: CPU number
 *
 * This is the last known freq, without actually getting it from the driver.
 * Return value will be same as what is shown in scaling_cur_freq in sysfs.
 */
unsigned int cpufreq_quick_get(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	unsigned int ret_freq = 0;

	if (policy) {
		ret_freq = policy->cur;
		cpufreq_cpu_put(policy);
	}

	return ret_freq;
}
EXPORT_SYMBOL(cpufreq_quick_get);

/**
 * cpufreq_quick_get_max - get the max reported CPU frequency for this CPU
 * @cpu: CPU number
 *
 * Just return the max possible frequency for a given CPU.
 */
unsigned int cpufreq_quick_get_max(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	unsigned int ret_freq = 0;

	if (policy) {
		ret_freq = policy->max;
		cpufreq_cpu_put(policy);
	}

	return ret_freq;
}
EXPORT_SYMBOL(cpufreq_quick_get_max);


static unsigned int __cpufreq_get(unsigned int cpu)
{
	struct cpufreq_policy *policy = per_cpu(cpufreq_cpu_data, cpu);
	unsigned int ret_freq = 0;

	if (!cpufreq_driver->get)
		return ret_freq;

	ret_freq = cpufreq_driver->get(cpu);

	if (ret_freq && policy->cur &&
		!(cpufreq_driver->flags & CPUFREQ_CONST_LOOPS)) {
		/* verify no discrepancy between actual and
					saved value exists */
		if (unlikely(ret_freq != policy->cur)) {
			cpufreq_out_of_sync(cpu, policy->cur, ret_freq);
			schedule_work(&policy->update);
		}
	}

	return ret_freq;
}

/**
 * cpufreq_get - get the current CPU frequency (in kHz)
 * @cpu: CPU number
 *
 * Get the CPU current (static) CPU frequency
 */
unsigned int cpufreq_get(unsigned int cpu)
{
	unsigned int ret_freq = 0;
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

	if (!policy)
		goto out;

	if (unlikely(lock_policy_rwsem_read(cpu)))
		goto out_policy;

	ret_freq = __cpufreq_get(cpu);

	unlock_policy_rwsem_read(cpu);

out_policy:
	cpufreq_cpu_put(policy);
out:
	return ret_freq;
}
EXPORT_SYMBOL(cpufreq_get);

static struct sysdev_driver cpufreq_sysdev_driver = {
	.add		= cpufreq_add_dev,
	.remove		= cpufreq_remove_dev,
};


/**
 * cpufreq_bp_suspend - Prepare the boot CPU for system suspend.
 *
 * This function is only executed for the boot processor.  The other CPUs
 * have been put offline by means of CPU hotplug.
 */
static int cpufreq_bp_suspend(void)
{
	int ret = 0;

	int cpu = smp_processor_id();
	struct cpufreq_policy *cpu_policy;

	dprintk("suspending cpu %u\n", cpu);

	/* If there's no policy for the boot CPU, we have nothing to do. */
	cpu_policy = cpufreq_cpu_get(cpu);
	if (!cpu_policy)
		return 0;

	if (cpufreq_driver->suspend) {
		ret = cpufreq_driver->suspend(cpu_policy);
		if (ret)
			printk(KERN_ERR "cpufreq: suspend failed in ->suspend "
					"step on CPU %u\n", cpu_policy->cpu);
	}

	cpufreq_cpu_put(cpu_policy);
	return ret;
}

/**
 * cpufreq_bp_resume - Restore proper frequency handling of the boot CPU.
 *
 *	1.) resume CPUfreq hardware support (cpufreq_driver->resume())
 *	2.) schedule call cpufreq_update_policy() ASAP as interrupts are
 *	    restored. It will verify that the current freq is in sync with
 *	    what we believe it to be. This is a bit later than when it
 *	    should be, but nonethteless it's better than calling
 *	    cpufreq_driver->get() here which might re-enable interrupts...
 *
 * This function is only executed for the boot CPU.  The other CPUs have not
 * been turned on yet.
 */
static void cpufreq_bp_resume(void)
{
	int ret = 0;

	int cpu = smp_processor_id();
	struct cpufreq_policy *cpu_policy;

	dprintk("resuming cpu %u\n", cpu);

	/* If there's no policy for the boot CPU, we have nothing to do. */
	cpu_policy = cpufreq_cpu_get(cpu);
	if (!cpu_policy)
		return;

	if (cpufreq_driver->resume) {
		ret = cpufreq_driver->resume(cpu_policy);
		if (ret) {
			printk(KERN_ERR "cpufreq: resume failed in ->resume "
					"step on CPU %u\n", cpu_policy->cpu);
			goto fail;
		}
	}

	schedule_work(&cpu_policy->update);

fail:
	cpufreq_cpu_put(cpu_policy);
}

static struct syscore_ops cpufreq_syscore_ops = {
	.suspend	= cpufreq_bp_suspend,
	.resume		= cpufreq_bp_resume,
};


/*********************************************************************
 *                     NOTIFIER LISTS INTERFACE                      *
 *********************************************************************/

/**
 *	cpufreq_register_notifier - register a driver with cpufreq
 *	@nb: notifier function to register
 *      @list: CPUFREQ_TRANSITION_NOTIFIER or CPUFREQ_POLICY_NOTIFIER
 *
 *	Add a driver to one of two lists: either a list of drivers that
 *      are notified about clock rate changes (once before and once after
 *      the transition), or a list of drivers that are notified about
 *      changes in cpufreq policy.
 *
 *	This function may sleep, and has the same return conditions as
 *	blocking_notifier_chain_register.
 */
int cpufreq_register_notifier(struct notifier_block *nb, unsigned int list)
{
	int ret;

	WARN_ON(!init_cpufreq_transition_notifier_list_called);

	switch (list) {
	case CPUFREQ_TRANSITION_NOTIFIER:
		ret = srcu_notifier_chain_register(
				&cpufreq_transition_notifier_list, nb);
		break;
	case CPUFREQ_POLICY_NOTIFIER:
		ret = blocking_notifier_chain_register(
				&cpufreq_policy_notifier_list, nb);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}
EXPORT_SYMBOL(cpufreq_register_notifier);


/**
 *	cpufreq_unregister_notifier - unregister a driver with cpufreq
 *	@nb: notifier block to be unregistered
 *      @list: CPUFREQ_TRANSITION_NOTIFIER or CPUFREQ_POLICY_NOTIFIER
 *
 *	Remove a driver from the CPU frequency notifier list.
 *
 *	This function may sleep, and has the same return conditions as
 *	blocking_notifier_chain_unregister.
 */
int cpufreq_unregister_notifier(struct notifier_block *nb, unsigned int list)
{
	int ret;

	switch (list) {
	case CPUFREQ_TRANSITION_NOTIFIER:
		ret = srcu_notifier_chain_unregister(
				&cpufreq_transition_notifier_list, nb);
		break;
	case CPUFREQ_POLICY_NOTIFIER:
		ret = blocking_notifier_chain_unregister(
				&cpufreq_policy_notifier_list, nb);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}
EXPORT_SYMBOL(cpufreq_unregister_notifier);


/*********************************************************************
 *                              GOVERNORS                            *
 *********************************************************************/


int __cpufreq_driver_target(struct cpufreq_policy *policy,
			    unsigned int target_freq,
			    unsigned int relation)
{
	int retval = -EINVAL;

	dprintk("target for CPU %u: %u kHz, relation %u\n", policy->cpu,
		target_freq, relation);
	trace_cpu_scale(policy->cpu, policy->cur, POWER_CPU_SCALE_START);
	if (cpu_online(policy->cpu) && cpufreq_driver->target)
		retval = cpufreq_driver->target(policy, target_freq, relation);
	trace_cpu_scale(policy->cpu, target_freq, POWER_CPU_SCALE_DONE);

	return retval;
}
EXPORT_SYMBOL_GPL(__cpufreq_driver_target);

int cpufreq_driver_target(struct cpufreq_policy *policy,
			  unsigned int target_freq,
			  unsigned int relation)
{
	int ret = -EINVAL;

	policy = cpufreq_cpu_get(policy->cpu);
	if (!policy)
		goto no_policy;

	if (unlikely(lock_policy_rwsem_write(policy->cpu)))
		goto fail;

	ret = __cpufreq_driver_target(policy, target_freq, relation);

	unlock_policy_rwsem_write(policy->cpu);

fail:
	cpufreq_cpu_put(policy);
no_policy:
	return ret;
}
EXPORT_SYMBOL_GPL(cpufreq_driver_target);

int __cpufreq_driver_getavg(struct cpufreq_policy *policy, unsigned int cpu)
{
	int ret = 0;

	if (!(cpu_online(cpu) && cpufreq_driver->getavg))
		return 0;

	policy = cpufreq_cpu_get(policy->cpu);
	if (!policy)
		return -EINVAL;

	ret = cpufreq_driver->getavg(policy, cpu);

	cpufreq_cpu_put(policy);
	return ret;
}
EXPORT_SYMBOL_GPL(__cpufreq_driver_getavg);

/*
 * when "event" is CPUFREQ_GOV_LIMITS
 */

static int __cpufreq_governor(struct cpufreq_policy *policy,
					unsigned int event)
{
	int ret;

	/* Only must be defined when default governor is known to have latency
	   restrictions, like e.g. conservative or ondemand.
	   That this is the case is already ensured in Kconfig
	*/
#ifdef CONFIG_CPU_FREQ_GOV_PERFORMANCE
	struct cpufreq_governor *gov = &cpufreq_gov_performance;
#else
	struct cpufreq_governor *gov = NULL;
#endif

	if (policy->governor->max_transition_latency &&
	    policy->cpuinfo.transition_latency >
	    policy->governor->max_transition_latency) {
		if (!gov)
			return -EINVAL;
		else {
			printk(KERN_WARNING "%s governor failed, too long"
			       " transition latency of HW, fallback"
			       " to %s governor\n",
			       policy->governor->name,
			       gov->name);
			policy->governor = gov;
		}
	}

	if (!try_module_get(policy->governor->owner))
		return -EINVAL;

	dprintk("__cpufreq_governor for CPU %u, event %u\n",
						policy->cpu, event);
	ret = policy->governor->governor(policy, event);

	/* we keep one module reference alive for
			each CPU governed by this CPU */
	if ((event != CPUFREQ_GOV_START) || ret)
		module_put(policy->governor->owner);
	if ((event == CPUFREQ_GOV_STOP) && !ret)
		module_put(policy->governor->owner);

	return ret;
}


int cpufreq_register_governor(struct cpufreq_governor *governor)
{
	int err;

	if (!governor)
		return -EINVAL;

	mutex_lock(&cpufreq_governor_mutex);

	err = -EBUSY;
	if (__find_governor(governor->name) == NULL) {
		err = 0;
		list_add(&governor->governor_list, &cpufreq_governor_list);
	}

	mutex_unlock(&cpufreq_governor_mutex);
	return err;
}
EXPORT_SYMBOL_GPL(cpufreq_register_governor);


void cpufreq_unregister_governor(struct cpufreq_governor *governor)
{
#ifdef CONFIG_HOTPLUG_CPU
	int cpu;
#endif

	if (!governor)
		return;

#ifdef CONFIG_HOTPLUG_CPU
	for_each_present_cpu(cpu) {
		if (cpu_online(cpu))
			continue;
		if (!strcmp(per_cpu(cpufreq_cpu_governor, cpu), governor->name))
			strcpy(per_cpu(cpufreq_cpu_governor, cpu), "\0");
	}
#endif

	mutex_lock(&cpufreq_governor_mutex);
	list_del(&governor->governor_list);
	mutex_unlock(&cpufreq_governor_mutex);
	return;
}
EXPORT_SYMBOL_GPL(cpufreq_unregister_governor);



/*********************************************************************
 *                          POLICY INTERFACE                         *
 *********************************************************************/

/**
 * cpufreq_get_policy - get the current cpufreq_policy
 * @policy: struct cpufreq_policy into which the current cpufreq_policy
 *	is written
 *
 * Reads the current cpufreq policy.
 */
int cpufreq_get_policy(struct cpufreq_policy *policy, unsigned int cpu)
{
	struct cpufreq_policy *cpu_policy;
	if (!policy)
		return -EINVAL;

	cpu_policy = cpufreq_cpu_get(cpu);
	if (!cpu_policy)
		return -EINVAL;

	memcpy(policy, cpu_policy, sizeof(struct cpufreq_policy));

	cpufreq_cpu_put(cpu_policy);
	return 0;
}
EXPORT_SYMBOL(cpufreq_get_policy);


/*
 * data   : current policy.
 * policy : policy to be set.
 */
static int __cpufreq_set_policy(struct cpufreq_policy *data,
				struct cpufreq_policy *policy)
{
	int ret = 0;
	unsigned int qmin, qmax;
	unsigned int pmin = policy->min;
	unsigned int pmax = policy->max;

	qmin = min((unsigned int)pm_qos_request(PM_QOS_CPU_FREQ_MIN),
		   data->user_policy.max);
	qmax = max((unsigned int)pm_qos_request(PM_QOS_CPU_FREQ_MAX),
		   data->user_policy.min);

	cpufreq_debug_disable_ratelimit();
	dprintk("setting new policy for CPU %u: %u - %u (%u - %u) kHz\n",
		policy->cpu, pmin, pmax, qmin, qmax);

	/* clamp the new policy to PM QoS limits */
	policy->min = max(pmin, qmin);
	policy->max = min(pmax, qmax);

	memcpy(&policy->cpuinfo, &data->cpuinfo,
				sizeof(struct cpufreq_cpuinfo));

	if (policy->min > data->user_policy.max ||
	    policy->max < data->user_policy.min) {
		ret = -EINVAL;
		goto error_out;
	}

	/* verify the cpu speed can be set within this limit */
	ret = cpufreq_driver->verify(policy);
	if (ret)
		goto error_out;

	/* adjust if necessary - all reasons */
	blocking_notifier_call_chain(&cpufreq_policy_notifier_list,
			CPUFREQ_ADJUST, policy);

	/* adjust if necessary - hardware incompatibility*/
	blocking_notifier_call_chain(&cpufreq_policy_notifier_list,
			CPUFREQ_INCOMPATIBLE, policy);

	/* verify the cpu speed can be set within this limit,
	   which might be different to the first one */
	ret = cpufreq_driver->verify(policy);
	if (ret)
		goto error_out;

	/* notification of the new policy */
	blocking_notifier_call_chain(&cpufreq_policy_notifier_list,
			CPUFREQ_NOTIFY, policy);

	data->min = policy->min;
	data->max = policy->max;

	dprintk("new min and max freqs are %u - %u kHz\n",
					data->min, data->max);

	if (cpufreq_driver->setpolicy) {
		data->policy = policy->policy;
		dprintk("setting range\n");
		ret = cpufreq_driver->setpolicy(policy);
	} else {
		if (policy->governor != data->governor) {
			/* save old, working values */
			struct cpufreq_governor *old_gov = data->governor;

			dprintk("governor switch\n");

			/* end old governor */
			if (data->governor)
				__cpufreq_governor(data, CPUFREQ_GOV_STOP);

			/* start new governor */
			data->governor = policy->governor;
			if (__cpufreq_governor(data, CPUFREQ_GOV_START)) {
				/* new governor failed, so re-start old one */
				dprintk("starting governor %s failed\n",
							data->governor->name);
				if (old_gov) {
					data->governor = old_gov;
					__cpufreq_governor(data,
							   CPUFREQ_GOV_START);
				}
				ret = -EINVAL;
				goto error_out;
			}
			/* might be a policy change, too, so fall through */
		}
		dprintk("governor: change or update limits\n");
		__cpufreq_governor(data, CPUFREQ_GOV_LIMITS);
	}

error_out:
	/* restore the limits that the policy requested */
	policy->min = pmin;
	policy->max = pmax;
	cpufreq_debug_enable_ratelimit();
	return ret;
}

/**
 *	cpufreq_update_policy - re-evaluate an existing cpufreq policy
 *	@cpu: CPU which shall be re-evaluated
 *
 *	Useful for policy notifiers which have different necessities
 *	at different times.
 */
int cpufreq_update_policy(unsigned int cpu)
{
	struct cpufreq_policy *data = cpufreq_cpu_get(cpu);
	struct cpufreq_policy policy;
	int ret;

	if (!data) {
		ret = -ENODEV;
		goto no_policy;
	}

	if (unlikely(lock_policy_rwsem_write(cpu))) {
		ret = -EINVAL;
		goto fail;
	}

	dprintk("updating policy for CPU %u\n", cpu);
	memcpy(&policy, data, sizeof(struct cpufreq_policy));
	policy.min = data->user_policy.min;
	policy.max = data->user_policy.max;
	policy.policy = data->user_policy.policy;
	policy.governor = data->user_policy.governor;

	/* BIOS might change freq behind our back
	  -> ask driver for current freq and notify governors about a change */
	if (cpufreq_driver->get) {
		policy.cur = cpufreq_driver->get(cpu);
		if (!data->cur) {
			dprintk("Driver did not initialize current freq");
			data->cur = policy.cur;
		} else {
			if (data->cur != policy.cur)
				cpufreq_out_of_sync(cpu, data->cur,
								policy.cur);
		}
	}

	ret = __cpufreq_set_policy(data, &policy);

	unlock_policy_rwsem_write(cpu);

fail:
	cpufreq_cpu_put(data);
no_policy:
	return ret;
}
EXPORT_SYMBOL(cpufreq_update_policy);

/*
 *	cpufreq_set_gov - set governor for a cpu
 *	@cpu: CPU whose governor needs to be changed
 *	@target_gov: new governor to be set
 */
int cpufreq_set_gov(char *target_gov, unsigned int cpu)
{
	int ret = 0;
	struct cpufreq_policy new_policy;
	struct cpufreq_policy *cur_policy;

	if (target_gov == NULL)
		return -EINVAL;

	/* Get current governor */
	cur_policy = cpufreq_cpu_get(cpu);
	if (!cur_policy)
		return -EINVAL;

	if (lock_policy_rwsem_read(cur_policy->cpu) < 0) {
		ret = -EINVAL;
		goto err_out;
	}

	if (cur_policy->governor)
		ret = strncmp(cur_policy->governor->name, target_gov,
					strlen(target_gov));
	else {
		unlock_policy_rwsem_read(cur_policy->cpu);
		ret = -EINVAL;
		goto err_out;
	}
	unlock_policy_rwsem_read(cur_policy->cpu);

	if (!ret) {
		pr_debug(" Target governer & current governer is same\n");
		ret = -EINVAL;
		goto err_out;
	} else {
		new_policy = *cur_policy;
		if (cpufreq_parse_governor(target_gov, &new_policy.policy,
				&new_policy.governor)) {
			ret = -EINVAL;
			goto err_out;
		}

		if (lock_policy_rwsem_write(cur_policy->cpu) < 0) {
			ret = -EINVAL;
			goto err_out;
		}

		ret = __cpufreq_set_policy(cur_policy, &new_policy);

		cur_policy->user_policy.policy = cur_policy->policy;
		cur_policy->user_policy.governor = cur_policy->governor;

		unlock_policy_rwsem_write(cur_policy->cpu);
	}
err_out:
	cpufreq_cpu_put(cur_policy);
	return ret;
}
EXPORT_SYMBOL(cpufreq_set_gov);

/*
 *	cpufreq_current_gov - return current governor for the cpu
 *	@cpu: CPU whose governor needs to be changed
 *	@buf: buffer for current governor
 */
ssize_t cpufreq_current_gov(char *buf, unsigned int cpu)
{
	int ret = 0;
	struct cpufreq_policy *policy;

	if (cpu >= nr_cpu_ids)
		return -EINVAL;

	/* Get current governor */
	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return -EINVAL;

	if (lock_policy_rwsem_read(policy->cpu) < 0) {
		ret = -EINVAL;
		goto err_out;
	}

	if (policy->policy == CPUFREQ_POLICY_POWERSAVE) {
		ret = sprintf(buf, "powersave\n");
	} else if (policy->policy == CPUFREQ_POLICY_PERFORMANCE) {
		ret = sprintf(buf, "performance\n");
	} else if (policy->governor) {
		ret = scnprintf(buf, CPUFREQ_NAME_LEN, "%s",
				policy->governor->name);
	} else {
		/* No gov set for this online cpu.
		 * If we are here, require serious
		 * debugging hence setting as pr_error.
		 */
		pr_err("No gov for online cpu:%d\n", cpu);
		ret = -EINVAL;
	}
	unlock_policy_rwsem_read(policy->cpu);
err_out:
	cpufreq_cpu_put(policy);
	return ret;

}
EXPORT_SYMBOL(cpufreq_current_gov);

static int __cpuinit cpufreq_cpu_callback(struct notifier_block *nfb,
					unsigned long action, void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;
	struct sys_device *sys_dev;

	sys_dev = get_cpu_sysdev(cpu);
	if (sys_dev) {
		switch (action) {
		case CPU_ONLINE:
		case CPU_ONLINE_FROZEN:
			cpufreq_add_dev(sys_dev);
			break;
		case CPU_DOWN_PREPARE:
		case CPU_DOWN_PREPARE_FROZEN:
			if (unlikely(lock_policy_rwsem_write(cpu)))
				BUG();

			__cpufreq_remove_dev(sys_dev);
			break;
		case CPU_DOWN_FAILED:
		case CPU_DOWN_FAILED_FROZEN:
			cpufreq_add_dev(sys_dev);
			break;
		}
	}
	return NOTIFY_OK;
}

static struct notifier_block __refdata cpufreq_cpu_notifier = {
    .notifier_call = cpufreq_cpu_callback,
};

/*********************************************************************
 *               REGISTER / UNREGISTER CPUFREQ DRIVER                *
 *********************************************************************/

/**
 * cpufreq_register_driver - register a CPU Frequency driver
 * @driver_data: A struct cpufreq_driver containing the values#
 * submitted by the CPU Frequency driver.
 *
 *   Registers a CPU Frequency driver to this core code. This code
 * returns zero on success, -EBUSY when another driver got here first
 * (and isn't unregistered in the meantime).
 *
 */
int cpufreq_register_driver(struct cpufreq_driver *driver_data)
{
	unsigned long flags;
	int ret;

	if (!driver_data || !driver_data->verify || !driver_data->init ||
	    ((!driver_data->setpolicy) && (!driver_data->target)))
		return -EINVAL;

	dprintk("trying to register driver %s\n", driver_data->name);

	if (driver_data->setpolicy)
		driver_data->flags |= CPUFREQ_CONST_LOOPS;

	spin_lock_irqsave(&cpufreq_driver_lock, flags);
	if (cpufreq_driver) {
		spin_unlock_irqrestore(&cpufreq_driver_lock, flags);
		return -EBUSY;
	}
	cpufreq_driver = driver_data;
	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);

	ret = sysdev_driver_register(&cpu_sysdev_class,
					&cpufreq_sysdev_driver);
	if (ret)
		goto err_null_driver;

	if (!(cpufreq_driver->flags & CPUFREQ_STICKY)) {
		int i;
		ret = -ENODEV;

		/* check for at least one working CPU */
		for (i = 0; i < nr_cpu_ids; i++)
			if (cpu_possible(i) && per_cpu(cpufreq_cpu_data, i)) {
				ret = 0;
				break;
			}

		/* if all ->init() calls failed, unregister */
		if (ret) {
			dprintk("no CPU initialized for driver %s\n",
							driver_data->name);
			goto err_sysdev_unreg;
		}
	}

	register_hotcpu_notifier(&cpufreq_cpu_notifier);
	dprintk("driver %s up and running\n", driver_data->name);
	cpufreq_debug_enable_ratelimit();

	return 0;
err_sysdev_unreg:
	sysdev_driver_unregister(&cpu_sysdev_class,
			&cpufreq_sysdev_driver);
err_null_driver:
	spin_lock_irqsave(&cpufreq_driver_lock, flags);
	cpufreq_driver = NULL;
	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(cpufreq_register_driver);


/**
 * cpufreq_unregister_driver - unregister the current CPUFreq driver
 *
 *    Unregister the current CPUFreq driver. Only call this if you have
 * the right to do so, i.e. if you have succeeded in initialising before!
 * Returns zero if successful, and -EINVAL if the cpufreq_driver is
 * currently not initialised.
 */
int cpufreq_unregister_driver(struct cpufreq_driver *driver)
{
	unsigned long flags;

	cpufreq_debug_disable_ratelimit();

	if (!cpufreq_driver || (driver != cpufreq_driver)) {
		cpufreq_debug_enable_ratelimit();
		return -EINVAL;
	}

	dprintk("unregistering driver %s\n", driver->name);

	sysdev_driver_unregister(&cpu_sysdev_class, &cpufreq_sysdev_driver);
	unregister_hotcpu_notifier(&cpufreq_cpu_notifier);

	spin_lock_irqsave(&cpufreq_driver_lock, flags);
	cpufreq_driver = NULL;
	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(cpufreq_unregister_driver);

static int cpu_freq_notify(struct notifier_block *b,
			   unsigned long l, void *v);

static struct notifier_block min_freq_notifier = {
	.notifier_call = cpu_freq_notify,
};
static struct notifier_block max_freq_notifier = {
	.notifier_call = cpu_freq_notify,
};

static int cpu_freq_notify(struct notifier_block *b,
			   unsigned long l, void *v)
{
	int cpu;
	pr_info("PM QoS PM_QOS_CPU_FREQ %s %lu\n",
		b == &min_freq_notifier ? "min" : "max", l);
	for_each_online_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
		if (policy) {
			cpufreq_update_policy(policy->cpu);
			cpufreq_cpu_put(policy);
		}
	}
	return NOTIFY_OK;
}

//                                                                                     
static struct pm_qos_request_list cpu_freq_min_req;
static struct pm_qos_request_list cpu_freq_max_req;

int cpufreq_set_min_freq(void *data, s32 val)
{
	pm_qos_update_request(&cpu_freq_min_req, val);
	return 0;
}

int cpufreq_set_max_freq(void *data, s32 val)
{
	pm_qos_update_request(&cpu_freq_max_req, val);
	return 0;
}
//                                                                                     

static int __init cpufreq_core_init(void)
{
	int cpu;
	int rc;

	for_each_possible_cpu(cpu) {
		per_cpu(cpufreq_policy_cpu, cpu) = -1;
		init_rwsem(&per_cpu(cpu_policy_rwsem, cpu));
	}

	cpufreq_global_kobject = kobject_create_and_add("cpufreq",
						&cpu_sysdev_class.kset.kobj);
	BUG_ON(!cpufreq_global_kobject);
	register_syscore_ops(&cpufreq_syscore_ops);
	rc = pm_qos_add_notifier(PM_QOS_CPU_FREQ_MIN,
				 &min_freq_notifier);
	BUG_ON(rc);
	rc = pm_qos_add_notifier(PM_QOS_CPU_FREQ_MAX,
				 &max_freq_notifier);
	BUG_ON(rc);
#ifdef CONFIG_MACH_X3
//                                                                                     
	pm_qos_add_request(&cpu_freq_min_req, PM_QOS_CPU_FREQ_MIN,
			   PM_QOS_DEFAULT_VALUE);
	pm_qos_add_request(&cpu_freq_max_req, PM_QOS_CPU_FREQ_MAX,
			   PM_QOS_DEFAULT_VALUE);
//                                                                                     
#endif
	return 0;
}
core_initcall(cpufreq_core_init);
