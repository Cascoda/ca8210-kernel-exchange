# ca8210-kernel-exchange
Glue code for linking Cascoda's API code to the ca8210 Linux driver

Either the `kernel_exchange_init()` function **or** the `kernel_exchange_init_withhandler(kernel_exchange_errorhandler callback)` function must be called by your application to link the cascoda-api functions to this code.

In order to build a useable library for the ca8210 after cloning the repository, run the commands:

```
git submodule update --init
make
```
Then include kernel_exchange.h and cascoda-api/include/cascoda_api.h in your C source, and link with the libca8210.a library.

In order to expose the ca8210 device node to this library the Linux debug file system must be mounted. debugfs can be mounted with the command:
```
mount -t debugfs none /sys/kernel/debug
```
