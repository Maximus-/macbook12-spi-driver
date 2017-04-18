Very simple, work in progress input driver for the SPI keyboard / trackpad found on 12" MacBooks. 

Using it:
---------
To get this driver to work on a 2016 12" MacBook, you'll need to boot the kernel with `intremap=nosid`. Also, you'll need to modify your DSDT as described by Leif Liddy at https://bugzilla.kernel.org/attachment.cgi?id=206671. (but don't worry about applying patches. Also see https://wiki.archlinux.org/index.php/DSDT for a quicker way to load custom DSDTs without recompiling)

This should result in the intel-lpss driver attaching itself to the SPI controller, and exposing the `APP000D` device.

For this to work with the 2015 model, simply make sure that none of the intel-lpss kernel modules are installed. 

What works:
-----------
* Basic Typing
* FN keys
* Driver unloading (no more hanging)
* Basic touchpad functionality (even right click, handled by libinput) (except for the 2015 12" Macbook)
* MT touchpad functionality (two finger scroll, probably others) (except for the 2015 12" Macbook)
* Interrupts!
* Suspend / resume

What doesn't work:
------------------
* Key rollover (properly)
* Wakeup on keypress / touchpad
 
Known bugs:
-----------
* Occasionally, the SPI device can get itself into a state where it causes an interrupt storm. There should be a way of resetting it, or better yet avoiding this state altogether.
* You shouldn't have to modify your DSDT to get it running.
* For some reason, doing operations at 8MHz (the speed that the slave device should run at) fail. 400kHz works, but this should be investigated.

Interupts:
----------
Interrupts are now working! This means that the driver is no longer polled, and should no longer be a massive battery drain. For more information on how the driver receives interrupts, see the discussion [here](https://github.com/cb22/macbook12-spi-driver/pull/1)

Touchpad:
---------
The touchpad protocol is the same as the bcm5974 driver. Perhaps there is a nice way of utilizing it? For now, bits of code have just been copy and pasted.

Some useful threads:
--------------------
* https://bugzilla.kernel.org/show_bug.cgi?id=108331
* https://bugzilla.kernel.org/show_bug.cgi?id=99891
