#ifndef __XENWATCH_H__
#define __XENWATCH_H__

#include <linux/types.h>

/*
 * The layout of data in shared info page is follows:
 * 1. struct xenwatch_state -- contains generic information about state and amount of variable-size objects
 * 2. array of struct xenwatch_state_net -- information about network interfaces
 * 3. array of struct xenwatch_state_disk -- information about disks
 */

struct xenwatch_state {
	u32 len;				/* Length of structure				*/
	atomic_t lock;				/* Page protection (1 if used, 0 if free)	*/
	u32 ts_ms;				/* timestamp in miliseconds			*/
	u64 la_1, la_5, la_15;			/* Load average fixed-point values		*/
	u8 network_interfaces;			/* count of network interfaces			*/
	u32 user, system, wait, idle;		/* previous times in miliseconds		*/
	u32 p_user, p_system, p_wait, p_idle;	/* CPU usage in percents*100			*/
} __attribute__ ((packed));



struct xenwatch_state_network {
	u64 rx_bytes, tx_bytes, rx_packets, tx_packets, dropped_packets, error_packets;
} __attribute__ ((packed));




inline void xw_page_lock (struct xenwatch_state *xw)
{
	while (atomic_add_unless (&xw->lock, 1, 1));
}


inline void xw_page_unlock (struct xenwatch_state *xw)
{
	atomic_dec (&xw->lock);
}


#endif /* __XENWATCH_H__ */
