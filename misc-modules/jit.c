/*
 * jit.c -- the just-in-time module
 *
 * Copyright (C) 2001,2003 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001,2003 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 * $Id: jit.c,v 1.16 2004/09/26 07:02:43 gregkh Exp $
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>

#include <linux/timekeeping.h>

#include <linux/time.h>
#include <linux/timer.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>

#include <linux/version.h>

#include <asm/hardirq.h>
/*
 * This module is a silly one: it only embeds short code fragments
 * that show how time delays can be handled in the kernel.
 */

int delay = HZ; /* the default delay, expressed in jiffies */

module_param(delay, int, 0);

MODULE_AUTHOR("Alessandro Rubini");
MODULE_LICENSE("Dual BSD/GPL");

/* use these as data pointers, to implement four files in one function */
enum jit_files {
	JIT_BUSY,
	JIT_SCHED,
	JIT_QUEUE,
	JIT_SCHEDTO
};

/*
 * This function prints one line of data, after sleeping one second.
 * It can sleep in different ways, according to the data pointer
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
int jit_fn(char *buf, char **start, off_t offset,
	      int len, int *eof, void *data)
#else
ssize_t jit_fn(struct file * filp, char *buf,
	   size_t count,loff_t *offp ) 
#endif
{
	unsigned long j0, j1; /* jiffies */
	wait_queue_head_t wait;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
	char * data;
	int len;
	data = PDE_DATA(file_inode(filp));
#endif

	init_waitqueue_head (&wait);
	j0 = jiffies;
	j1 = j0 + delay;

	switch((long)data) {
		case JIT_BUSY:
			while (time_before(jiffies, j1))
				cpu_relax();
			break;
		case JIT_SCHED:
			while (time_before(jiffies, j1)) {
				schedule();
			}
			break;
		case JIT_QUEUE:
			wait_event_interruptible_timeout(wait, 0, delay);
			break;
		case JIT_SCHEDTO:
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout (delay);
			break;
	}
	j1 = jiffies; /* actual value after we delayed */

	len = sprintf(buf, "%9li %9li\n", j0, j1);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
	*start = buf;
#endif
	return len;
}

/*
 * This file, on the other hand, returns the current time forever
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
int jit_currentime(char *buf, char **start, off_t offset,
                   int len, int *eof, void *data)
#else
ssize_t jit_currentime(struct file * filp, char * buf, 
		   size_t count, loff_t * offp)
#endif
{
#ifndef _LINUX_KTIME_H
	struct timeval tv1;
	struct timespec tv2;
#else   
	ktime_t kt;
	struct timespec64 ts;
	s64 kt_s,kt_us;
#endif
	unsigned long j1;
	u64 j2;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
	char * data;
	int len;
	data = PDE_DATA(file_inode(filp));
#endif
	/* get them four */
	j1 = jiffies;
	j2 = get_jiffies_64();
#ifndef _LINUX_KTIME_H	
	do_gettimeofday(&tv1);
	tv2 = get_kernel_time();
#else
	kt = ktime_get_real();
	kt_us = ktime_to_us(kt);
	kt_s = kt_us / 1000000;
	kt_us = kt_us % 1000000;
	ktime_get_coarse_real_ts64(&ts);
#endif


	/* print */
	len=0;
	len += sprintf(buf,"0x%08lx 0x%016Lx %10i.%06i\n"
		       "%40i.%09i\n",
		       j1, j2,
#ifndef _LINUX_KTIME_H
		       (int) tv1.tv_sec, (int) tv2.tv_usec,
		       (int) tv2.tv_sec, (int) tv2.tv_nsec);
#else
		       (int) kt_s	,(int) kt_us,
		       (int) ts.tv_sec	,(int) ts.tv_nsec);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
	*start = buf;
#endif
	return len;
}

/*
 * The timer example follows
 */

int tdelay = 10;
module_param(tdelay, int, 0);

/* This data structure used as "data" for the timer and tasklet functions */
struct jit_data {
	struct timer_list timer;
	struct tasklet_struct tlet;
	int hi; /* tasklet or tasklet_hi */
	wait_queue_head_t wait;
	unsigned long prevjiffies;
	unsigned char *buf;
	int loops;
};
#define JIT_ASYNC_LOOPS 5

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,14,0)
void jit_timer_fn(unsigned long arg)
{
	struct jit_data *data = (struct jit_data *)arg;
#else
void jit_timer_fn(struct timer_list * list)
{
	struct jit_data * data = from_timer(data, list, timer);
#endif
	unsigned long j = jiffies;
	data->buf += sprintf(data->buf, "%9li  %3li     %i    %6i   %i   %s\n",
			     j, j - data->prevjiffies, in_interrupt() ? 1 : 0,
			     current->pid, smp_processor_id(), current->comm);

	if (--data->loops) {
		data->timer.expires += tdelay;
		data->prevjiffies = j;
		add_timer(&data->timer);
	} else {
		wake_up_interruptible(&data->wait);
	}
}

/* the /proc function: allocate everything to allow concurrency */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
int jit_timer(char *buf, char **start, off_t offset,
	      int len, int *eof, void *unused_data)
#else
ssize_t jit_timer(struct file * filp, char *buf,
	   size_t count,loff_t *offp )
#endif
{
	struct jit_data *data;
	char *buf2 = buf;
	unsigned long j = jiffies;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,14,0)
	init_timer(&data->timer);
#endif
	init_waitqueue_head (&data->wait);

	/* write the first lines in the buffer */
	buf2 += sprintf(buf2, "   time   delta  inirq    pid   cpu command\n");
	buf2 += sprintf(buf2, "%9li  %3li     %i    %6i   %i   %s\n",
			j, 0L, in_interrupt() ? 1 : 0,
			current->pid, smp_processor_id(), current->comm);

	/* fill the data for our timer function */
	data->prevjiffies = j;
	data->buf = buf2;
	data->loops = JIT_ASYNC_LOOPS;
	
	/* register the timer */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,14,0)
	data->timer.data = (unsigned long)data;
	data->timer.function = jit_timer_fn;
	data->timer.expires = j + tdelay; /* parameter */
	add_timer(&data->timer);
#else
	data->timer.expires = j + tdelay;
	timer_setup(&data->timer,jit_timer_fn,0);
#endif

	/* wait for the buffer to fill */
	wait_event_interruptible(data->wait, !data->loops);
	if (signal_pending(current))
		return -ERESTARTSYS;
	buf2 = data->buf;
	kfree(data);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
	*eof = 1;
#endif
	return buf2 - buf;
}

void jit_tasklet_fn(unsigned long arg)
{
	struct jit_data *data = (struct jit_data *)arg;
	unsigned long j = jiffies;
	data->buf += sprintf(data->buf, "%9li  %3li     %i    %6i   %i   %s\n",
			     j, j - data->prevjiffies, in_interrupt() ? 1 : 0,
			     current->pid, smp_processor_id(), current->comm);

	if (--data->loops) {
		data->prevjiffies = j;
		if (data->hi)
			tasklet_hi_schedule(&data->tlet);
		else
			tasklet_schedule(&data->tlet);
	} else {
		wake_up_interruptible(&data->wait);
	}
}

/* the /proc function: allocate everything to allow concurrency */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
int jit_tasklet(char *buf, char **start, off_t offset,
	      int len, int *eof, void *arg)
#else
ssize_t jit_tasklet(struct file * filp, char *buf,
	   size_t count,loff_t *offp )
#endif
{
	struct jit_data *data;
	char *buf2 = buf;
	unsigned long j = jiffies;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
	long hi;
	hi = (long)PDE_DATA(file_inode(filp));
#else
	long hi = (long)arg;
#endif

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	init_waitqueue_head (&data->wait);

	/* write the first lines in the buffer */
	buf2 += sprintf(buf2, "   time   delta  inirq    pid   cpu command\n");
	buf2 += sprintf(buf2, "%9li  %3li     %i    %6i   %i   %s\n",
			j, 0L, in_interrupt() ? 1 : 0,
			current->pid, smp_processor_id(), current->comm);

	/* fill the data for our tasklet function */
	data->prevjiffies = j;
	data->buf = buf2;
	data->loops = JIT_ASYNC_LOOPS;
	
	/* register the tasklet */
	tasklet_init(&data->tlet, jit_tasklet_fn, (unsigned long)data);
	data->hi = hi;
	if (hi)
		tasklet_hi_schedule(&data->tlet);
	else
		tasklet_schedule(&data->tlet);

	/* wait for the buffer to fill */
	wait_event_interruptible(data->wait, !data->loops);

	if (signal_pending(current))
		return -ERESTARTSYS;
	buf2 = data->buf;
	kfree(data);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
	*eof = 1;
#endif
	return buf2 - buf;
}



int __init jit_init(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
	create_proc_read_entry("currentime", 0, NULL, jit_currentime, NULL);
	create_proc_read_entry("jitbusy", 0, NULL, jit_fn, (void *)JIT_BUSY);
	create_proc_read_entry("jitsched",0, NULL, jit_fn, (void *)JIT_SCHED);
	create_proc_read_entry("jitqueue",0, NULL, jit_fn, (void *)JIT_QUEUE);
	create_proc_read_entry("jitschedto", 0, NULL, jit_fn, (void *)JIT_SCHEDTO);

	create_proc_read_entry("jitimer", 0, NULL, jit_timer, NULL);
	create_proc_read_entry("jitasklet", 0, NULL, jit_tasklet, NULL);
	create_proc_read_entry("jitasklethi", 0, NULL, jit_tasklet, (void *)1);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(5,6,0)
	struct file_operations fops_currentime = {
	.read = jit_currentime
	};
	struct file_operations fops_fn = {
	.read = jit_fn
	};
	struct file_operations fops_timer = {
	.read = jit_timer
	};
	struct file_operations fops_tasklet = {
	.read = jit_tasklet
	};

	proc_create_data("currentime", 0, NULL, &fops_currentime, NULL);
	proc_create_data("jitbusy", 0, NULL, &fops_fn, (void *)JIT_BUSY);
	proc_create_data("jitsched",0, NULL, &fops_fn, (void *)JIT_SCHED);
	proc_create_data("jitqueue",0, NULL, &fops_fn, (void *)JIT_QUEUE);
	proc_create_data("jitschedto", 0, NULL, &fops_fn, (void *)JIT_SCHEDTO);

	proc_create_data("jitimer", 0, NULL, &fops_timer, NULL);
	proc_create_data("jitasklet", 0, NULL, &fops_tasklet, NULL);
	proc_create_data("jitasklethi", 0, NULL, &fops_tasklet, (void *)1);
#else
	struct proc_ops pops_currentime = {
	.proc_read = jit_currentime
	};
	struct proc_ops pops_fn = {
	.proc_read = jit_fn
	};
	struct proc_ops pops_timer = {
	.proc_read = jit_timer
	};
	struct proc_ops pops_tasklet = {
	.proc_read = jit_tasklet
	};

	proc_create_data("currentime", 0, NULL, &pops_currentime, NULL);
	proc_create_data("jitbusy", 0, NULL, &pops_fn, (void *)JIT_BUSY);
	proc_create_data("jitsched",0, NULL, &pops_fn, (void *)JIT_SCHED);
	proc_create_data("jitqueue",0, NULL, &pops_fn, (void *)JIT_QUEUE);
	proc_create_data("jitschedto", 0, NULL, &pops_fn, (void *)JIT_SCHEDTO);

	proc_create_data("jitimer", 0, NULL, &pops_timer, NULL);
	proc_create_data("jitasklet", 0, NULL, &pops_tasklet, NULL);
	proc_create_data("jitasklethi", 0, NULL, &pops_tasklet, (void *)1);
#endif
	return 0; /* success */
}

void __exit jit_cleanup(void)
{
	remove_proc_entry("currentime", NULL);
	remove_proc_entry("jitbusy", NULL);
	remove_proc_entry("jitsched", NULL);
	remove_proc_entry("jitqueue", NULL);
	remove_proc_entry("jitschedto", NULL);

	remove_proc_entry("jitimer", NULL);
	remove_proc_entry("jitasklet", NULL);
	remove_proc_entry("jitasklethi", NULL);
}

module_init(jit_init);
module_exit(jit_cleanup);
