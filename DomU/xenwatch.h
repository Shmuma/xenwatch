#ifndef __XENWATCH_H__
#define __XENWATCH_H__

#include <linux/types.h>

/*
 * The layout of data in shared info page is follows:
 * 1. struct xenwatch_state -- contains generic information about state and amount of variable-size objects
 * 2. array of struct xenwatch_state_net -- information about network interfaces
 */

struct xenwatch_state {
	u32 len;				/* Length of structure				*/
	u64 counter;				/* some measurements are not performed every 1s */
	u32 ts_ms;				/* timestamp in miliseconds			*/
	u64 la_1, la_5, la_15;			/* Load average fixed-point values		*/
	u32 uptime;
	u32 network_interfaces;			/* count of network interfaces			*/
	u32 user, system, wait, idle;		/* previous times in miliseconds		*/
	u32 p_user, p_system, p_wait, p_idle;	/* CPU usage in percents*100			*/
	u64 mem_total, mem_free;		/* Memory size in bytes				*/
	u64 mem_buffers, mem_cached;
	u64 freeswap, totalswap;
	u64 root_size, root_free, root_inodes, root_inodes_free;
} __attribute__ ((packed));


struct xenwatch_state_network {
	u64 rx_bytes, tx_bytes, rx_packets, tx_packets, dropped_packets, error_packets;
} __attribute__ ((packed));



struct xenwatch_state_network*
get_network_info (struct xenwatch_state *xw, int index)
{
	int ofs = sizeof (struct xenwatch_state) + sizeof (struct xenwatch_state_network) * index;

	return (struct xenwatch_state_network*)(((char*)xw) + ofs);
}


#endif /* __XENWATCH_H__ */
