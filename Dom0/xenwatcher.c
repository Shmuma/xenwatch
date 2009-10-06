#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/timer.h>
#include <linux/list.h>

#include <xen/xenbus.h>

#define MAJOR_VERSION 0
#define MINOR_VERSION 1


struct xw_domain_info_t {
	struct list_head list;
	int domain_id;
	int page_ref;
};


/* Domains update interval */
#define XW_UPDATE_INTERVAL (10*HZ)

/* timer routine */
static void xw_update_domains (unsigned long);

static struct proc_dir_entry *xw_dir;
static struct xw_domain_info *domains;

static const char* xw_name = "xenwatcher";

DEFINE_TIMER (xw_update_timer, xw_update_domains, 0, 0);



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



#if 0
static int xw_read_data (char *page, char **start, off_t off, int count, int *eof, void *data)
{
	char **res;
	unsigned int num, i, pref_len;
	int len, domid, page_ref;
	char *buf;
	int buf_len = 128;
	char *pref;

	buf = kmalloc (buf_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* iterate over xen store */
	res = xenbus_directory (XBT_NIL, "/local/domain", "", &num);
	if (IS_ERR (res))
		return PTR_ERR (res);

	len = 0;
	for (i = 0; i < num; i++) {
		if (sscanf (res[i], "%d", &domid)) {
			sprintf (buf, "%d/xenwatch/page_ref", domid);
			/* get page reference */
			pref = xenbus_read (XBT_NIL, "/local/domain", buf, &pref_len);
			if (!IS_ERR (pref)) {
				if (sscanf (pref, "%d", &page_ref))
					len += dump_shared_page (page + len, domid, page_ref);
				kfree (pref);
			}
		}
	}

	kfree (buf);
	kfree (res);

	return proc_calc_metrics (page, start, off, count, eof, len);
}
#endif


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


/* Timer routine. Iterates over all virtual machines (using XenStore) and obtains shared page
 * reference index. */
static void xw_update_domains (unsigned long data)
{
	char **doms, *buf, *pref;
	unsigned int c_doms, i, pref_len, domid, page_ref;

	buf = kmalloc (128, GFP_KERNEL);
	if (!buf)
		goto error;

	/* iterate over all domains in XS */
	doms = xenbus_directory (XBT_NIL, "/local/domain", "", &c_doms);
	if (IS_ERR (doms))
		goto error1;

	for (i = 0; i < c_doms; i++) {
		if (sscanf (doms[i], "%u", &domid) <= 0)
			continue;

		sprintf (buf, "%d/device/xenwatch/page_ref", domid);
		pref = xenbus_read (XBT_NIL, "/local/domain", buf, &pref_len);
		if (!IS_ERR (pref)) {
			if (sscanf (pref, "%d", &page_ref)) {
				/* handle ref,domain entry */
				printk (KERN_INFO "Handle domain %u, ref %u\n", domid, page_ref);
			}
			kfree (pref);
		}
	}

	kfree (doms);
error1:
	kfree (buf);
error:
	recharge_timer ();
}


static int __init xw_init (void)
{
	/* register /proc/xenwatcher/data entry */
	xw_dir = proc_mkdir (xw_name, NULL);
	if (!xw_dir) {
		printk (KERN_WARNING "%s: failed to register /proc entry\n", xw_name);
		return -EINVAL;
	}

//	create_proc_read_entry ("data", 0, xw_dir, xw_read_data, NULL);
	create_proc_read_entry ("version", 0, xw_dir, xw_read_version, NULL);

	recharge_timer ();

        return 0;
}



static void __exit xw_exit (void)
{
	/* destroy timer */
	del_timer_sync (&xw_update_timer);

	/* destroy /proc/xenwatcher/data entry */
//	remove_proc_entry ("data", xw_dir);
	remove_proc_entry ("version", xw_dir);

	/* remove all domain entries */

	remove_proc_entry (xw_name, NULL);
}



module_init (xw_init);
module_exit (xw_exit);

MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("Max Lapan <max.lapan@gmail.com>");
MODULE_DESCRIPTION ("XenWatcher Dom0 part");
