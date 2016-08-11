# ca8210-kernel-exchange
Glue code for linking Cascoda's API code to the ca8210 Linux driver

The `kernel_exchange_init()` function must be called by your application to link the cascoda-api functions to this code.

In order to expose the ca8210 device node to this library the Linux debug file system must be mounted. debugfs can be mounted with the command:
```
mount -t debugfs none /sys/kernel/debug
```
