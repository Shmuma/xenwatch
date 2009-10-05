#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

#include "xenwatch.h"


/* Update shared page contents every second */
#define XW_UPDATE_INTERVAL (1*HZ)

/* timer routine */
static void xw_update_page (unsigned long);


/* Shared page with monitoring state */
static struct page *shared_page;

/* Module prefix. Used in log messages. */
static const char *xw_prefix = "XenWatch";

/* Timer which fires every second and update data in shared page */
DEFINE_TIMER (xw_update_timer, xw_update_page, 0, 0);


static void init_page (void)
{
	struct xenwatch_state *xw = page_address (shared_page);

	xw->len = sizeof (struct xenwatch_state);
	xw->lock = SPIN_LOCK_UNLOCKED;
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

	if (!xw)
		goto exit;

	spin_lock (&xw->lock);
	xw->la_1 = avenrun[0];
	xw->la_5 = avenrun[1];
	xw->la_15 = avenrun[2];
	spin_unlock (&xw->lock);

	printk (KERN_INFO "xw_timer %lu, %lu, %lu\n", avenrun[0], avenrun[1], avenrun[2]);
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
        return 0;
}



static void __exit xw_exit (void)
{
	/* destroy timer */
	del_timer_sync (&xw_update_timer);

	/* remove page information from XenStore */

	/* free page */
	__free_page (shared_page);
}


module_init (xw_init);
module_exit (xw_exit);

MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("Max Lapan <max.lapan@gmail.com>");
MODULE_DESCRIPTION ("Xen Watch DomU module");

