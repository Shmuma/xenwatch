#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/timer.h>
#include <linux/time.h>
#include <linux/sched.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/kernel_stat.h>
#include <linux/jiffies.h>
#include <linux/swap.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/statfs.h>
#include <linux/genhd.h>
#include <linux/magic.h>
#include <linux/swap.h>

#define DEBUG 0
#define PATCHED_KERNEL 1

#include <asm/page.h>

#include <xen/xenbus.h>
#include <xen/grant_table.h>

#include "xenwatch.h"


#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC             0x01021994
#endif


/* Update shared page contents every second */
#define XW_UPDATE_INTERVAL (1*HZ)

/* timer routine */
static void xw_update_page (unsigned long);


/* Shared page with monitoring state */
static struct page *shared_page;

static int grant_ref;

/* Module prefix. Used in log messages. */
static const char *xw_prefix = "XenWatch";

#define XENSTORE_PATH "device/xenwatch"

#define PAGES2BYTES(x) ((u64)(x) << PAGE_SHIFT)


/* Timer which fires every second and update data in shared page */
DEFINE_TIMER (xw_update_timer, xw_update_page, 0, 0);


static void init_page (void)
{
	struct xenwatch_state *xw = page_address (shared_page);

	xw->len = sizeof (struct xenwatch_state);
	atomic_set (&xw->lock, 0);
	xw->counter = 0;
}


/* Schedules next timer callback */
inline void recharge_timer (void)
{
	mod_timer (&xw_update_timer, round_jiffies (jiffies + XW_UPDATE_INTERVAL));
}


inline u32 calc_percent (u32 old, u32 new, u32 ts_delta)
{
	u32 tmp = (new - old) * 10000 / ts_delta;
	return (tmp > 10000) ? 10000 : tmp;
}


static void gather_root_data (struct xenwatch_state *xw)
{
	struct nameidata nd;
	struct kstatfs kstat;

	xw->root_size = 0;
	xw->root_free = 0;
	xw->root_inodes = 0;
	xw->root_inodes_free = 0;

	if (path_lookup ("/", 0, &nd))
		printk (KERN_INFO "xenwatch: Root lookup error\n");
	else {
		if (!nd.path.dentry->d_sb->s_op->statfs (nd.path.dentry, &kstat)) {
			xw->root_size = (u64)kstat.f_blocks * kstat.f_bsize;
			xw->root_free = (u64)kstat.f_bfree * kstat.f_bsize;
			xw->root_inodes = (u64)kstat.f_files;
			xw->root_inodes_free = (u64)kstat.f_ffree;
		}

		path_put (&nd.path);
	}
}


/* Timer routine. Gather monitoring data and update it in shared page. */
static void xw_update_page (unsigned long data)
{
	struct xenwatch_state *xw = page_address (shared_page);
	struct net_device *net_dev;
	struct xenwatch_state_network *xw_net = (struct xenwatch_state_network*)((char*)xw + sizeof (struct xenwatch_state));
	struct net_device_stats *stats;
	u8 index;
	u32 old_ts;
	int i;
	cputime64_t user, system, wait, idle;
	struct sysinfo si;
#if PATCHED_KERNEL
	struct timespec uptime;
#endif
	if (!xw)
		goto exit;

	xw_page_lock (xw);
	old_ts = xw->ts_ms;

	xw->ts_ms = jiffies_to_msecs (jiffies);
	xw->la_1  = avenrun[0];
	xw->la_5  = avenrun[1];
	xw->la_15 = avenrun[2];

#if DEBUG
	printk (KERN_INFO "XenWatch: LA: %llu, %llu, %llu\n", xw->la_1, xw->la_5, xw->la_15);
#endif

	/* iterate over network devices */
	index = 0;
	for_each_netdev (&init_net, net_dev) {
		if (net_dev->type == ARPHRD_ETHER) {
			stats = net_dev->get_stats (net_dev);
			xw_net[index].rx_bytes = stats->rx_bytes;
			xw_net[index].tx_bytes = stats->tx_bytes;
			xw_net[index].rx_packets = stats->rx_packets;
			xw_net[index].tx_packets = stats->tx_packets;
			xw_net[index].dropped_packets = stats->rx_dropped + stats->tx_dropped;
			xw_net[index].error_packets = stats->rx_errors + stats->tx_errors;
			index++;
		}
	}
	xw->network_interfaces = index;

	/* CPU time */
	user = system = wait = idle = cputime64_zero;
	for_each_possible_cpu (i) {
		user = cputime64_add (user, kstat_cpu (i).cpustat.user);
		system = cputime64_add (system, kstat_cpu (i).cpustat.system);
		idle = cputime64_add (idle, kstat_cpu (i).cpustat.idle);
		wait = cputime64_add (wait, kstat_cpu (i).cpustat.iowait);
	}

	if (old_ts && xw->ts_ms != old_ts) {
		u32 delta = xw->ts_ms - old_ts;

		/* we have previous values, calculate percents */
		xw->p_user = calc_percent (xw->user, cputime_to_msecs (user), delta);
		xw->p_system = calc_percent (xw->system, cputime_to_msecs (system), delta);
		xw->p_wait = calc_percent (xw->wait, cputime_to_msecs (wait), delta);
		xw->p_idle = calc_percent (xw->idle, cputime_to_msecs (idle), delta);
	}

	/* Calculate used times in miliseconds */
	xw->user = cputime_to_msecs (user);
	xw->system = cputime_to_msecs (system);
	xw->wait = cputime_to_msecs (wait);
	xw->idle = cputime_to_msecs (idle);

	/* memory */
	si_meminfo (&si);
#if PATCHED_KERNEL
	si_swapinfo (&si);
#else
	si.freeswap = si.totalswap = 0;
#endif
	xw->mem_total   = PAGES2BYTES (si.totalram);
	xw->mem_free    = PAGES2BYTES (si.freeram);
	xw->mem_buffers = PAGES2BYTES (si.bufferram);
	xw->mem_cached  = PAGES2BYTES (global_page_state(NR_FILE_PAGES) - si.bufferram);
	xw->freeswap    = PAGES2BYTES (si.freeswap);
	xw->totalswap   = PAGES2BYTES (si.totalswap);

	/* / space info */
	gather_root_data (xw);

	/* uptime */
#if PATCHED_KERNEL
        do_posix_clock_monotonic_gettime (&uptime);
        monotonic_to_bootbased (&uptime);
	xw->uptime = uptime.tv_sec;
#else
	xw->uptime = 0;
#endif

	/* total length of data */
	xw->len = sizeof (struct xenwatch_state) + index * sizeof (struct xenwatch_state_network);

#if DEBUG
	printk (KERN_INFO "Total data length: %d\n", xw->len);
#endif
	xw->counter++;
	xw_page_unlock (xw);
exit:
	recharge_timer ();
}


static int __init xw_init (void)
{
	/* allocate shared page */
	shared_page = alloc_page (GFP_KERNEL || __GFP_ZERO);

	if (!shared_page) {
		printk (KERN_ERR "%s: cannot allocate shared page\n", xw_prefix);
		return -ENOMEM;
	}

	init_page ();

	/* start timer */
	recharge_timer ();

	/* publish page information via the XenStore */
	grant_ref = gnttab_grant_foreign_access (0, virt_to_mfn (page_address (shared_page)), 0);
	if (grant_ref <= 0)
		goto fail;

	xenbus_printf (XBT_NIL, XENSTORE_PATH, "page_ref", "%d", grant_ref);
        return 0;

fail:
	xenbus_rm (XBT_NIL, XENSTORE_PATH, "");
	del_timer_sync (&xw_update_timer);
	__free_page (shared_page);
	return grant_ref;
}


static void __exit xw_exit (void)
{
	/* destroy timer */
	del_timer_sync (&xw_update_timer);

	/* remove page information from XenStore */
	xenbus_rm (XBT_NIL, XENSTORE_PATH, "");
	gnttab_end_foreign_access_ref (grant_ref, 0);

	/* free page */
	__free_page (shared_page);
}


module_init (xw_init);
module_exit (xw_exit);

MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("Max Lapan <max.lapan@gmail.com>");
MODULE_DESCRIPTION ("Xen Watch DomU module");
