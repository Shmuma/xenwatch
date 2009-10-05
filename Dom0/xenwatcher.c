#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>

#define MAJOR_VERSION 0
#define MINOR_VERSION 1


static struct proc_dir_entry *xw_dir;

static const char* xw_name = "xenwatcher";



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



static int xw_read_data (char *page, char **start, off_t off, int count, int *eof, void *data)
{
	return 0;
}



static int xw_read_version (char *page, char **start, off_t off, int count, int *eof, void *data)
{
	int len;
	len = sprintf (page, "XenWatcher %d.%d\n", MAJOR_VERSION, MINOR_VERSION);
	return proc_calc_metrics (page, start, off, count, eof, len);
}



static int __init xw_init (void)
{
	/* register /proc/xenwatcher/data entry */
	xw_dir = proc_mkdir (xw_name, NULL);
	if (!xw_dir) {
		printk (KERN_WARNING "%s: failed to register /proc entry\n", xw_name);
		return -EINVAL;
	}

	create_proc_read_entry ("data", 0, xw_dir, xw_read_data, NULL);
	create_proc_read_entry ("version", 0, xw_dir, xw_read_version, NULL);

        return 0;
}


static void __exit xw_exit (void)
{
	/* destroy /proc/xenwatcher/data entry */
	remove_proc_entry ("data", xw_dir);
	remove_proc_entry ("version", xw_dir);
	remove_proc_entry (xw_name, NULL);
}


module_init (xw_init);
module_exit (xw_exit);

MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("Max Lapan <max.lapan@gmail.com>");
MODULE_DESCRIPTION ("XenWatcher Dom0 part");
