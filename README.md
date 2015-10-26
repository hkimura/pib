pib - Pseudo InfiniBand HCA driver
==================================

pib is a software-based InfiniBand HCA driver.
It provides InfiniBand functions without real IB HCA & fabric.
pib aims to simulate InfiniBand behavior accurately but not to get speed.

pib contains the three components.

- pib.ko  - Linux kernel module
- libpib  - Userspace plug-in module for libibverbs
- pibnetd - IB switch emulator for multi-host-mode

Features
========

In single-host-mode, pib creates up to 4 InfiniBand HCA (The default is 2).
These IB devices are pib_0, pib_1, pib_2 and pib_3.
Each HCA contains up to 32 ports (The default is 2).

In addition, pib creates one internal InfiniBand switch too.
All ports of pib's HCA are connected to this switch.

The current version of pib enables to drive the following interface:

* kernel-level Verbs (in-linux kernel)
* kernel-level MAD (in-linux kernel)
* uVerbs (libibverbs)
* uMAD (libibmad & libibumad)
* Subnet Manager (opensm)
* IPoIB (in-linux kernel)
* RDMA Connection Manager (librdmacm)
* IB diagnostic utilities (infiniband-diags)

Debugging support features:

* Inspect IB objects (ucontext, PD, MR, SRQ, CQ, AH, QP)
* Trace API invocations, packet sending/receiving, async events/errors
* Inject a specified error (QP/CQ/SRQ Error)
* Select some implementation dependent behaviour and enforce error checking.
* Show a warning of pitfalls that IB programs should avoid.

Other features:

* The maximum size of inline data is 2048 bytes.

Limitation
==========

The current version is EXPERIMENTAL.

The following features are not supported:

- Unreliable Connected (UC)
- Fast Memory Region (FMR)
- Memory Windows (MW)
- SEND Invalidate operation
- Virtual Lane (VL)
- Flow control

Supported OS
============

pib supports the following Linux:

* Red Hat Enterprise Linux 6.x
* CentOS 6.x 

pib conflicts with Mellanox OFED.
Mustn't install an environment to deploy Mellanox OFED.

Preparation
===========

The following software packages are required for building pib:

* rdma
* libibverbs
* kernel-devel
* opensm
* opensm-libs

The following packages are recommended:

* libibverbs-devel (for developing Verbs API programs)
* libibverbs-utils
* librdmacm
* librdmacm-utils
* librdmacm-devel (for developing RDMA API programs)
* infiniband-diags (IB diagnostic tools)

Building
========

First, acquire the source code by cloning the git repository.

    $ git clone https://github.com/nminoru/pib.git

pib.ko
------

If you want to compile the pib.ko kernel module from source code, input the following commands.

    $ cd pib/driver/
    $ make
    # make modules_install

If you want to create binary RPM file, input the following commands.

First, create libpib's source RPM from source code.

    $ cp -r pib/driver pib-0.4.5
    $ tar czvf $(HOME)/rpmbuild/SOURCES/pib-0.4.5.tar.gz pib-0.4.5/
    $ cp pib/driver/pib.conf $(HOME)/rpmbuild/SOURCES/
    $ cp pib/driver/pib.files $(HOME)/rpmbuild/SOURCES/
    $ rpmbuild -bs pib/driver/pib.spec

Next, build the binary RPM from the source RPM.

    $ rpmbuild --rebuild $(HOME)/rpmbuild/SRPMS/pib-0.4.5-1.el6.src.rpm

Finally, install the built binary RPM.

    # rpm -ihv $(HOME)/rpmbuild/RPMS/x86_64/kmod-pib-0.4.5-1.el6.x86_64.rpm

libpib
------

The libpib userspace plug-in module will be installed from the binary RPM. 

    $ cp -r pib/libpib libpib-0.0.6
    $ tar czvf $(HOME)/rpmbuild/SOURCES/libpib-0.0.6.tar.gz libpib-0.0.6/
    $ rpmbuild -bs pib/libpib/libpib.spec

    $ rpmbuild --rebuild $(HOME)/rpmbuild/SRPMS/libpib-0.0.6-1.el6.src.rpm

    # rpm -ihv $(HOME)/rpmbuild/RPMS/x86_64/libpib-0.0.6-1.el6.x86_64.rpm

pibnetd
-------

If you want to compile the pibnetd daemon from source code, input the following commands.

    $ cd pib/pibnet/
    $ make
    # install -m 755 -D pibnetd                     /usr/sbin/pibnetd
    # install -m 755 -D scripts/redhat-pibnetd.init /etc/rc.d/init.d/pibnetd

If you want to create binary RPM file, input the following commands.

    $ cp -r pib/pibnetd pibnetd-0.4.1
    $ tar czvf $(HOME)/rpmbuild/SOURCES/pibnetd-0.4.1.tar.gz pibnetd-0.4.1/
    $ rpmbuild -bs pib/pibnetd/pibnetd.spec

    $ rpmbuild --rebuild $(HOME)/rpmbuild/SRPMS/pibnetd-0.4.1-1.el6.src.rpm

    # rpm -ihv $(HOME)/rpmbuild/RPMS/x86_64/pibnetd-0.4.1-1.el6.x86_64.rpm

Download
--------

You can get source and binary RPMs for RHEL6 or CentOS6 on this link http://www.nminoru.jp/~nminoru/network/infiniband/src/

Loading (single-host-mode)
==========================

First, load some modules which pib.ko is dependent on.

    # /etc/rc.d/init.d/rdma start

Next, load pib.ko.

    # modprobe pib

Finally, run opensm

    # /etc/rc.d/init.d/opensm start

pib.ko options
--------------

* debug_level
* num_hca
* phys_port_cnt
* behavior
* manner_warn
* manner_err
* addr

Loading (multi-host-mode)
=========================

In multi-host-mode mode, pib enables to connect up to 32 hosts (To be precise, up to 32 ports).

       Host A           Host X           Host B
     (10.0.0.1)       (10.0.0.2)       (10.0.0.3)
    +----------+     +-----------+     +----------+
    | +------+ |     | +-------+ |     | +------+ |
    | |pib.ko| |-----| |pibnetd| |-----| |pib.ko| |
    | +------+ |     | +-------+ |     | +------+ |
    |          |     |           |     | +------+ |
    |          |     |           |     | |opensm| |
    |          |     |           |     | +------+ | 
    +----------+     +-----------+     +----------+ 

First, run pibnetd on a host.

    # /etc/rc.d/init.d/pibnetd start

Next, load pib.ko by running modprobe command with the _addr_ parameter specified by the pibnetd's IP address.

    # /etc/rc.d/init.d/rdma start
    # modprobe pib addr=10.0.0.2

On th default parameters, pib creates 2 IB devices of 2 ports.
You had better limit 1 IB device of 1 port by specifying the _num_hca_ and _phys_port_cnt_ parameters in multi-host-mode.

    # modprobe pib addr=10.0.0.2 num_hca=1 phys_port_cnt=1

Finally, run opensm on one of hosts that load pib.ko.

    # /etc/rc.d/init.d/opensm start

Running
=======

For instance, ibv_devinfo (includes libibverbs-utils package) show such an result.

    $ ibv_devinfo
    hca_id: pib_0
            transport:                      InfiniBand (0)
            fw_ver:                         0.2.000
            node_guid:                      000c:2925:551e:0400
            sys_image_guid:                 000c:2925:551e:0200
            vendor_id:                      0x0001
            vendor_part_id:                 1
            hw_ver:                         0x0
            phys_port_cnt:                  2

Performance counter
-------------------

    # perfquery

Debugging support
=================

pib provides some debugging functions via debugfs to help developing IB programs.

First ensure that debugfs is mounted.

    # mount -t debugfs none /sys/kernel/debug

A list of available debugging functions can be found in /sys/kernel/debug/pib/pib_X/.

See detailed information on DEBUGFS.md.


FAQ
===

Failed to call ibv_reg_mr() with more than 64 KB
------------------------------------------------

pib permits an unprivileged program to use InfiniBand userspace verbs.
However Linux operating system limits the maximum memory size that an unprivileged process may lock via mlock() and ibv_reg_mr() calls mlock() internally.
This default max-locked-memory is only 64 K bytes.

To avoid this trouble, run your program under privileged mode or increase max-locked-memory limit for unprivileged user.

If you choose the latter, add the following two lines in the file /etc/security/limits.conf and then reboot.

    * soft memlock unlimited
    * hard memlock unlimited

Or you can also set it temporarily to do `ulimit -l unlimited`.

Other gotchas (added by Hideaki)
----------
### Commands to start/stop pib (RHEL7/Fedora21~)

    sudo systemctl stop pibnetd
    sudo systemctl stop opensm
    sudo modprobe -r pib

    sudo systemctl start rdma
    sudo modprobe pib
    sudo systemctl start opensm
    # see below for why
    sudo chmod 666 /dev/infiniband/issm* /dev/infiniband/umad*


    # now, try ibhosts and ibv_devinfo

### ibhosts fails with: can't open UMAD port
Was getting an error on ibhosts "can't open UMAD port ((null):0)".

    [kimurhid@hkimura-z820 infiniband]$ ibhosts
    src/query_smp.c:228; can't open UMAD port ((null):0)
    /usr/sbin/ibnetdiscover: iberror: failed: discover failed

Turns out that it's a permission issue of device files.
Check file permissions under /dev/infiniband/

    [kimurhid@hkimura-z820 infiniband]$ ls -al /dev/infiniband/
    total 0
    drwxr-xr-x.  2 root root      300 Oct 25 17:55 .
    drwxr-xr-x. 22 root root     3780 Oct 23 16:49 ..
    crw-------.  1 root root 231,  64 Oct 25 17:55 issm0
    crw-------.  1 root root 231,  65 Oct 25 17:55 issm1
    crw-------.  1 root root 231,  66 Oct 25 17:55 issm2
    crw-------.  1 root root 231,  67 Oct 25 17:55 issm3
    crw-rw-rw-.  1 root root  10,  57 Oct 23 16:49 rdma_cm
    crw-rw-rw-.  1 root root 231, 224 Oct 25 17:55 ucm0
    crw-rw-rw-.  1 root root 231, 225 Oct 25 17:55 ucm1
    crw-------.  1 root root 231,   0 Oct 25 17:55 umad0
    crw-------.  1 root root 231,   1 Oct 25 17:55 umad1
    crw-------.  1 root root 231,   2 Oct 25 17:55 umad2
    crw-------.  1 root root 231,   3 Oct 25 17:55 umad3
    crw-rw-rw-.  1 root root 231, 192 Oct 25 17:55 uverbs0
    crw-rw-rw-.  1 root root 231, 193 Oct 25 17:55 uverbs1

umad* and issm* are not accessible to normal users, thus ibhosts failed.
Change them to 666.

    [kimurhid@hkimura-z820 infiniband]$ sudo chmod 666 issm* umad*
    [kimurhid@hkimura-z820 infiniband]$ ibhosts
    Ca      : 0x000af74093e80400 ports 2 "hkimura-z820 pib_1"
    Ca      : 0x000af74093e80300 ports 2 "hkimura-z820 pib_0"

I don't know whether this is a good practice.
  http://lists.openfabrics.org/pipermail/users/2013-February/000095.html
I do believe, however, this is a norm nowadays.
All of my genuine Infiniband environments have these files under 666 (except issm).

Future work
===========

IB functions
------------

* Fast Memory Registration(FMR)
* Peer-Direct
* Alternate path
* Unreliable Connection(UC)
* Extended Reliable Connected (XRC)
* Memory Window

Debugging support
-----------------

* Packet filtering

Software components
-------------------

* MPI
* User Direct Access Programming Library (uDAPL)
* iSCSI Extensions for RDMA (iSER)
* SCSI RDMA Protocol (SRP)

Other
-----

* Systemd init script support
* Other Linux distributions support
* Kernel update package
* IPv6 support
* Translate Japanese into English in comments of source codes :-)

Contact
=======

[https://twitter.com/nminoru_jp](https://twitter.com/nminoru_jp)

<nminoru1975@gmail.com>

License
=======

GPL version 2 or BSD license
