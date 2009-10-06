#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/timer.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>

#include <xen/xenbus.h>

#define MAJOR_VERSION 0
#define MINOR_VERSION 1


struct xw_domain_info {
	struct list_head list;
	int domain_id;
	int page_ref;
};


/* Domains update interval */
#define XW_UPDATE_INTERVAL (10*HZ)

/* timer routine */
static void xw_update_tf (unsigned long);

/* workqueue routine */
static void xw_update_domains (struct work_struct *);

static struct proc_dir_entry *xw_dir;

static spinlock_t domains_lock = SPIN_LOCK_UNLOCKED;
static LIST_HEAD (domains);

static const char* xw_name = "xenwatcher";

DEFINE_TIMER (xw_update_timer, xw_update_tf, 0, 0);

DECLARE_WORK (xw_update_worker, &xw_update_domains);




static int proc_calc_metrics (char *page, char **start, off_t off,
                              int count, int *eof, int len)
{
        if (len <= off+count)
                *eof = 1;
        *start = page + off;
        len -= off;
        if (len < 0)
                len = 0;
        if (len > count)
                len = count;
        return len;
}


static int xw_read_version (char *page, char **start, off_t off, int count, int *eof, void *data)
{
	int len;
	len = sprintf (page, "XenWatcher %d.%d\n", MAJOR_VERSION, MINOR_VERSION);
	return proc_calc_metrics (page, start, off, count, eof, len);
}


/* Schedules next timer callback */
inline void recharge_timer (void)
{
	mod_timer (&xw_update_timer, round_jiffies (jiffies + XW_UPDATE_INTERVAL));
}


/* Timer routine. Schedules update work to be done (xenbus_XXX routines cannot be called from
 * interrupt context) */
static void xw_update_tf (unsigned long data)
{
	schedule_work (&xw_update_worker);
	recharge_timer ();
}


static void xw_update_domains (struct work_struct *args)
{
	char **doms, *buf, *pref;
	unsigned int c_doms, i, pref_len, domid, page_ref;
	struct xw_domain_info *di, *di_private = NULL;

	printk (KERN_INFO "Work Queue\n");

	buf = kmalloc (128, GFP_KERNEL);
	if (!buf)
		return;

	/* iterate over all domains in XS */
	doms = xenbus_directory (XBT_NIL, "/local/domain", "", &c_doms);
	if (IS_ERR (doms))
		goto error;

	for (i = 0; i < c_doms; i++) {
		if (sscanf (doms[i], "%u", &domid) <= 0)
			continue;

		sprintf (buf, "%d/device/xenwatch/page_ref", domid);
		pref = xenbus_read (XBT_NIL, "/local/domain", buf, &pref_len);
		if (!IS_ERR (pref)) {
			if (sscanf (pref, "%d", &page_ref)) {
				printk (KERN_INFO "Handle domain %u, ref %u\n", domid, page_ref);

				spin_lock (&domains_lock);
				di = domain_lookup (domid);
				if (!di) {
					/* new domain */
					di = kmalloc (sizeof (struct xw_domain_info), GFP_KERNEL);
					if (!di)
						printk (KERN_WARNING "%s: memory allocation error\n", xw_name);
					else {
						di->domain_id = domid;
						di->page_ref = page_ref;
						INIT_LIST_HEAD (&di->list);
					}
				}
				else {
					if (di->page_ref != page_ref)
						di->page_ref = page_ref;
					/* remove domain from list to find deleted domains */
					list_del_init (&di->list);
				}

				if (di) {
					/* add domain_info into own private list to find all */
					if (!di_private)
						di_private = di;
					else
						list_add (&di->list, &di_private->list);
				}
				spin_unlock (&domains_lock);
			}
			kfree (pref);
		}
	}

	spin_lock (&domains_lock);
	if (!list_empty (&domains)) {
		/* iterate over all remaining domains in list and remove their /proc entries */
		struct list_head *p, *n;

		list_for_each_safe (p, domains) {
			di = list_entry (p, struct xw_domain_info, list);
			/* delete proc entry */
		}
	}

	/* move list entries on their place */
	spin_unlock (&domains_lock);

	kfree (doms);
error:
	kfree (buf);
}



static int __init xw_init (void)
{
	/* register /proc/xenwatcher/data entry */
	xw_dir = proc_mkdir (xw_name, NULL);
	if (!xw_dir) {
		printk (KERN_WARNING "%s: failed to register /proc entry\n", xw_name);
		return -EINVAL;
	}

	create_proc_read_entry ("version", 0, xw_dir, xw_read_version, NULL);

	recharge_timer ();

        return 0;
}



static void __exit xw_exit (void)
{
	/* destroy timer */
	del_timer_sync (&xw_update_timer);

	remove_proc_entry ("version", xw_dir);

	/* remove all domain entries */


	remove_proc_entry (xw_name, NULL);
}



module_init (xw_init);
module_exit (xw_exit);

MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("Max Lapan <max.lapan@gmail.com>");
MODULE_DESCRIPTION ("XenWatcher Dom0 part");
