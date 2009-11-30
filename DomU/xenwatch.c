#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/kernel_stat.h>


#include <xen/xenbus.h>
#include <xen/grant_table.h>

#include "xenwatch.h"


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


/* Timer which fires every second and update data in shared page */
DEFINE_TIMER (xw_update_timer, xw_update_page, 0, 0);


static void init_page (void)
{
	struct xenwatch_state *xw = page_address (shared_page);

	xw->len = sizeof (struct xenwatch_state);
	atomic_set (&xw->lock, 0);
}


/* Schedules next timer callback */
inline void recharge_timer (void)
{
	mod_timer (&xw_update_timer, round_jiffies (jiffies + XW_UPDATE_INTERVAL));
}


/* Timer routine. Gather monitoring data and update it in shared page. */
static void xw_update_page (unsigned long data)
{
	struct xenwatch_state *xw = page_address (shared_page);
	struct net_device *net_dev;
	struct xenwatch_state_network *xw_net = (struct xenwatch_state_network*)((char*)xw + sizeof (struct xenwatch_state));
	struct net_device_stats *stats;
	u8 index;
	int i;
	cputime64_t user, system, wait, idle;

	if (!xw)
		goto exit;

	xw_page_lock (xw);
	xw->la_1 = avenrun[0];
	xw->la_5 = avenrun[1];
	xw->la_15 = avenrun[2];

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

	/* Save previous values */
	xw->p_user = xw->user;
	xw->p_system = xw->system;
	xw->p_wait = xw->wait;
	xw->p_idle = xw->idle;

	/* Calculate used times in miliseconds */
	xw->user = cputime_to_msecs (user);
	xw->system = cputime_to_msecs (system);
	xw->wait = cputime_to_msecs (wait);
	xw->idle = cputime_to_msecs (idle);

	/* total length of data */
	xw->len = sizeof (struct xenwatch_state) + index * sizeof (struct xenwatch_state_network);
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
