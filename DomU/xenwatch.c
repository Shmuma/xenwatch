#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
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
exit:
	recharge_timer ();
}



static int __init xw_init (void)
{
	int res;

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
