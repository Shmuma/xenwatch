#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/timer.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/mm.h>

#include <xen/xenbus.h>
#include <xen/interface/grant_table.h>

#include "../DomU/xenwatch.h"


#define MAJOR_VERSION 0
#define MINOR_VERSION 2


struct xw_domain_info {
	struct list_head list;
	int domain_id;
	char *domain_name;
	int page_ref;
	struct proc_dir_entry *proc_dir;
	struct page *page;
	grant_handle_t grant_handle; /* valid only when page mapped */
	uint64_t page_bus_addr;
};


/* Domains update interval */
#define XW_UPDATE_INTERVAL (1*HZ)

#define LOAD_INT(x) ((x) >> FSHIFT)
#define LOAD_FRAC(x) LOAD_INT(((x) & (FIXED_1-1)) * 100)


static void xw_update_tf (unsigned long);			/* timer routine */
static void xw_update_domains (struct work_struct *);		/* workqueue routine */

static struct xw_domain_info* create_di (unsigned int domid, unsigned int page_ref);
static void destroy_di (struct xw_domain_info *di);

static int xw_read_la (char *page, char **start, off_t off, int count, int *eof, void *data);
static int xw_read_network (char *page, char **start, off_t off, int count, int *eof, void *data);

static struct xenwatch_state* map_state (struct xw_domain_info *di);
static int unmap_state (struct xw_domain_info *di);


static struct proc_dir_entry *xw_dir;

static spinlock_t domains_lock = SPIN_LOCK_UNLOCKED;
static LIST_HEAD (domains);

static const char* xw_name = "xenwatcher";
static const char* xw_version = "xenwatch_version";

static const char* xs_local_dir = "/local/domain";

DEFINE_TIMER (xw_update_timer, xw_update_tf, 0, 0);

DECLARE_WORK (xw_update_worker, &xw_update_domains);



static struct xenwatch_state* map_state (struct xw_domain_info *di)
{
	struct gnttab_map_grant_ref op;

	/* Map shared page */
	memset (&op, 0, sizeof (op));
	op.host_addr = (unsigned long)page_address (di->page);
	op.flags = GNTMAP_host_map;
	op.ref = di->page_ref;
	op.dom = di->domain_id;

	if (HYPERVISOR_grant_table_op (GNTTABOP_map_grant_ref, &op, 1)) {
		printk (KERN_ERR "%s: failed to map shared page from domain %u, ref %u", xw_name, di->domain_id, di->page_ref);
		return NULL;
	}

	di->grant_handle = op.handle;
	di->page_bus_addr = op.dev_bus_addr;

	return (struct xenwatch_state *)page_address (di->page);
}


static int unmap_state (struct xw_domain_info *di)
{
	struct gnttab_unmap_grant_ref u_op;

	/* unmap page */
	memset (&u_op, 0, sizeof (u_op));
	u_op.host_addr = (unsigned long)page_address (di->page);
	u_op.dev_bus_addr = di->page_bus_addr;
	u_op.handle = di->grant_handle;

	if (HYPERVISOR_grant_table_op (GNTTABOP_unmap_grant_ref, &u_op, 1)) {
		printk (KERN_ERR "%s: failed to unmap shared page for domain %u, ref %u\n", xw_name, di->domain_id, di->page_ref);
		return 0;
	}

	return 1;
}



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


static int xw_read_la (char *page, char **start, off_t off, int count, int *eof, void *data)
{
	struct xw_domain_info *di = (struct xw_domain_info *)data;
	struct xenwatch_state *xw_state;
	int len;

	xw_state = map_state (di);
	xw_page_lock (xw_state);
	len = sprintf (page, "%llu.%02llu %llu.%02llu %llu.%02llu\n",
		       LOAD_INT (xw_state->la_1), LOAD_FRAC (xw_state->la_1),
		       LOAD_INT (xw_state->la_5), LOAD_FRAC (xw_state->la_5),
		       LOAD_INT (xw_state->la_15), LOAD_FRAC (xw_state->la_15));
	xw_page_unlock (xw_state);
	unmap_state (di);

	return proc_calc_metrics (page, start, off, count, eof, len);
}


static int xw_read_network (char *page, char **start, off_t off, int count, int *eof, void *data)
{
	struct xw_domain_info *di = (struct xw_domain_info *)data;
	struct xenwatch_state *xw_state;
	struct xenwatch_state_network *xw_net;
	int len = 0, i;

	xw_state = map_state (di);
	xw_page_lock (xw_state);

	len += sprintf (page, "interface,rx_bytes,tx_bytes,rx_packets,tx_packets,dropped,error\n");
	xw_net = (struct xenwatch_state_network*)((char*)xw_state + sizeof (*xw_state));

	for (i = 0; i < xw_state->network_interfaces; i++)
		len += sprintf (page+len, "eth%d,%llu,%llu,%llu,%llu,%llu,%llu\n", i,
				xw_net->rx_bytes, xw_net->tx_bytes,
				xw_net->rx_packets, xw_net->tx_packets,
				xw_net->dropped_packets, xw_net->error_packets);

	xw_page_unlock (xw_state);
	unmap_state (di);

	return proc_calc_metrics (page, start, off, count, eof, len);
}


static int xw_read_cpu (char *page, char **start, off_t off, int count, int *eof, void *data)
{
	struct xw_domain_info *di = (struct xw_domain_info *)data;
	struct xenwatch_state *xw_state;
	int len = 0;

	xw_state = map_state (di);
	xw_page_lock (xw_state);

	len += sprintf (page, "user,prev_user,system,prev_system,wait,prev_wait,idle,prev_idle\n%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
			xw_state->user, xw_state->p_user,
			xw_state->system, xw_state->p_system,
			xw_state->wait, xw_state->p_wait,
			xw_state->idle, xw_state->p_idle);

	xw_page_unlock (xw_state);
	unmap_state (di);

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


static struct xw_domain_info* domain_lookup (unsigned int domid)
{
	struct list_head *p;
	struct xw_domain_info *di;

	list_for_each (p, &domains) {
		di = list_entry (p, struct xw_domain_info, list);
		if (domid == di->domain_id)
			return di;
	}

	return NULL;
}


static void xw_update_domains (struct work_struct *args)
{
	char **doms, *buf, *pref, *dom_name;
	unsigned int c_doms, i, pref_len, domid, page_ref;
	struct xw_domain_info *di;
	LIST_HEAD (doms_private);
	struct list_head *p, *n;

	buf = kmalloc (128, GFP_KERNEL);
	if (!buf)
		return;

	/* iterate over all domains in XS */
	doms = xenbus_directory (XBT_NIL, xs_local_dir, "", &c_doms);
	if (IS_ERR (doms))
		goto error;

	for (i = 0; i < c_doms; i++) {
		if (sscanf (doms[i], "%u", &domid) <= 0)
			continue;

		sprintf (buf, "%d/name", domid);
		dom_name = xenbus_read (XBT_NIL, xs_local_dir, buf, &pref_len);
		if (IS_ERR (dom_name)) {
			printk (KERN_WARNING "Error reading name of domain %d\n", domid);
			continue;
		}

		sprintf (buf, "%d/device/xenwatch/page_ref", domid);
		pref = xenbus_read (XBT_NIL, xs_local_dir, buf, &pref_len);
		if (!IS_ERR (pref)) {
			if (sscanf (pref, "%d", &page_ref)) {
				spin_lock (&domains_lock);
				di = domain_lookup (domid);
				if (!di) {
					di = create_di (domid, page_ref);
					if (!di)
						printk (KERN_WARNING "%s: memory allocation error\n", xw_name);
				}
				else {
					if (di->page_ref != page_ref)
						di->page_ref = page_ref;
					/* remove domain from list to find deleted domains */
					list_del_init (&di->list);
				}

				/* add domain_info into own private list to find all */
				if (di)
					list_add (&di->list, &doms_private);
				spin_unlock (&domains_lock);
			}
			kfree (pref);
		}
	}

	spin_lock (&domains_lock);
	if (!list_empty (&domains)) {
		/* iterate over all remaining domains in list and remove their /proc entries */
		list_for_each_safe (p, n,  &domains) {
			di = list_entry (p, struct xw_domain_info, list);
			list_del (p);
			destroy_di (di);
		}
	}

	/* move list entries on their place */
	list_for_each_safe (p, n, &doms_private) {
		list_del (p);
		list_add (p, &domains);
	}
	spin_unlock (&domains_lock);

	kfree (doms);
error:
	kfree (buf);
}


static struct xw_domain_info* create_di (unsigned int domid, unsigned int page_ref)
{
	struct xw_domain_info *di;
	char buf[128];
	int len;

	/* new domain */
	di = kmalloc (sizeof (struct xw_domain_info), GFP_KERNEL);
	if (!di)
		return NULL;

	di->domain_id = domid;
	di->page_ref = page_ref;

	/* get name of domain */
	sprintf (buf, "%d/name", domid);
	di->domain_name = xenbus_read (XBT_NIL, xs_local_dir, buf, &len);
	if (IS_ERR (di->domain_name)) {
		printk (KERN_WARNING "Error reading name of domain %d\n", domid);
		goto error2;
	}

	INIT_LIST_HEAD (&di->list);
	di->proc_dir = proc_mkdir (di->domain_name, xw_dir);
	create_proc_read_entry ("la", 0, di->proc_dir, xw_read_la, di);
	create_proc_read_entry ("network", 0, di->proc_dir, xw_read_network, di);
	create_proc_read_entry ("cpu", 0, di->proc_dir, xw_read_cpu, di);
	di->page = alloc_page (GFP_KERNEL);
	if (!di->page)
		goto error;
	return di;

error:
	remove_proc_entry ("la", di->proc_dir);
	remove_proc_entry ("network", di->proc_dir);
	remove_proc_entry ("cpu", di->proc_dir);
	remove_proc_entry (di->domain_name, xw_dir);
	kfree (di->domain_name);
error2:
	kfree (di);
	return NULL;
}


static void destroy_di (struct xw_domain_info *di)
{
	remove_proc_entry ("la", di->proc_dir);
	remove_proc_entry ("network", di->proc_dir);
	remove_proc_entry ("cpu", di->proc_dir);
	remove_proc_entry (di->proc_dir->name, di->proc_dir->parent);
//	__free_page (di->page);
	kfree (di->domain_name);
	kfree (di);
}


static int __init xw_init (void)
{
	/* register /proc/xenwatcher/data entry */
	xw_dir = proc_mkdir (xw_name, NULL);
	if (!xw_dir) {
		printk (KERN_WARNING "%s: failed to register /proc entry\n", xw_name);
		return -EINVAL;
	}

	create_proc_read_entry (xw_version, 0, xw_dir, xw_read_version, NULL);

	recharge_timer ();

	printk (KERN_INFO "XenWatcher %d.%d initialized\n", MAJOR_VERSION, MINOR_VERSION);

        return 0;
}


static void __exit xw_exit (void)
{
	struct list_head *p, *n;
	struct xw_domain_info *di;

	printk (KERN_INFO "XenWatcher unloading\n");

	/* destroy timer */
	del_timer_sync (&xw_update_timer);
	flush_scheduled_work ();

	printk (KERN_INFO "Work synched\n");

	remove_proc_entry (xw_version, xw_dir);

	printk (KERN_INFO "Proc removed\n");

	/* remove all domain entries */
	list_for_each_safe (p, n, &domains) {
		di = list_entry (p, struct xw_domain_info, list);
		list_del (p);
		destroy_di (di);
	}

	printk (KERN_INFO "DI removed\n");

	remove_proc_entry (xw_name, NULL);

	printk (KERN_INFO "Unload finished\n");

}



module_init (xw_init);
module_exit (xw_exit);

MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("Max Lapan <max.lapan@gmail.com>");
MODULE_DESCRIPTION ("XenWatcher Dom0 part");
