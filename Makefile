patched_kernel=1

all: domu dom0

domu: DomU/xenwatch.ko

dom0: Dom0/xenwatcher.ko

DomU/xenwatch.ko: DomU/xenwatch.c DomU/xenwatch.h
	(cd DomU && PATCHED_KERNEL=$(patched_kernel) ./b.sh)

Dom0/xenwatcher.ko: Dom0/xenwatcher.c DomU/xenwatch.h
	(cd Dom0 && ./b.sh)
	cp Dom0/xenwatcher.ko .

clean:
	(cd DomU && ./c.sh)
	(cd Dom0 && ./c.sh)
	rm -f xenwatcher.ko

update: domu dom0
	scp DomU/xenwatch.ko kernel:
