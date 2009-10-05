#ifndef __XENWATCH_H__
#define __XENWATCH_H__

#include <linux/spinlock.h>

struct xenwatch_state {
	int len;			 /* Length of structure */
	spinlock_t lock;		 /* Protection of page */
	unsigned long la_1, la_5, la_15; /* Load vaerage fixed-point values */
};

#endif /* __XENWATCH_H__ */
