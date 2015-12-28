/*********************************************************
 * Copyright (C) 1998 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation version 2 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 *********************************************************/

#include "driver-config.h"

#define EXPORT_SYMTAB

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/poll.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/mm.h>
#include "compat_skbuff.h"
#include <linux/sockios.h>
#include "compat_sock.h"

#define __KERNEL_SYSCALLS__
#include <asm/io.h>

#include <linux/proc_fs.h>
#include <linux/file.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <net/tcp.h>
#include <net/ipv6.h>

#ifdef CONFIG_NET_RADIO
#   include <linux/wireless.h>
#endif
#include "vmnetInt.h"
#include "compat_spinlock.h"
#include "compat_netdevice.h"
#include "vnetInt.h"
#include "smac.h"

#define VNET_BRIDGE_HISTORY    48

/*
 * Bytes reserved before start of packet.  As Ethernet header has 14 bytes,
 * to get aligned IP header we must skip 2 bytes before packet.  Not that it
 * matters a lot for us, but using 2 is compatible with what newer 2.6.x
 * kernels do.
 */
#ifndef NET_IP_ALIGN
#define NET_IP_ALIGN	2
#endif

#if LOGLEVEL >= 4
static struct timeval vnetTime;
#endif

typedef struct VNetBridge VNetBridge;

struct VNetBridge {
   struct notifier_block    notifier;       // for device state changes
   char                     name[VNET_NAME_LEN]; // name of net device (e.g., "eth0")
   struct net_device       *dev;            // device structure for 'name'
   struct sock             *sk;             // socket associated with skb's
   struct packet_type       pt;             // used to add packet handler
   Bool                     enabledPromisc; // track if promisc enabled
   Bool                     warnPromisc;    // tracks if warning has been logged
   Bool                     forceSmac;      // whether to use smac unconditionally
   struct sk_buff          *history[VNET_BRIDGE_HISTORY];  // avoid duplicate packets
   spinlock_t		    historyLock;    // protects 'history'
   VNetPort                 port;           // connection to virtual hub
   Bool                     wirelessAdapter; // connected to wireless adapter?
   struct SMACState        *smac;           // device structure for wireless
   VNetEvent_Sender        *eventSender;    // event sender
};

typedef PacketStatus (* SMACINT SMACFunc)(struct SMACState *, SMACPackets *);

static int  VNetBridgeUp(VNetBridge *bridge, Bool rtnlLock);
static void VNetBridgeDown(VNetBridge *bridge, Bool rtnlLock);

static int  VNetBridgeNotify(struct notifier_block *this, u_long msg,
			     void *data);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 14) && \
    !defined(VMW_TL10S64_WORKAROUND)
static int VNetBridgeReceiveFromDev(struct sk_buff *skb,
                                    struct net_device *dev,
                                    struct packet_type *pt);
#else
static int VNetBridgeReceiveFromDev(struct sk_buff *skb,
                                    struct net_device *dev,
                                    struct packet_type *pt,
                                    struct net_device *real_dev);
#endif

static void VNetBridgeFree(VNetJack *this);
static void VNetBridgeReceiveFromVNet(VNetJack *this, struct sk_buff *skb);
static Bool VNetBridgeCycleDetect(VNetJack *this, int generation);
static Bool VNetBridgeIsDeviceWireless(struct net_device *dev);
static void VNetBridgePortsChanged(VNetJack *this);
static int  VNetBridgeIsBridged(VNetJack *this);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
static int  VNetBridgeProcShow(struct seq_file *seqf, void *data);
#else
static int  VNetBridgeProcRead(char *page, char **start, off_t off,
                               int count, int *eof, void *data);
#endif
static void VNetBridgeComputeHeaderPosIPv6(struct sk_buff *skb);
static PacketStatus VNetCallSMACFunc(struct SMACState *state,
                                     struct sk_buff **skb, void *startOfData,
                                     SMACFunc func, unsigned int len);


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeStartPromisc --
 *
 *      Set IFF_PROMISC on the peer interface.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The peer interface IFF_PROMISC flag may be changed.
 *
 *----------------------------------------------------------------------
 */

static void
VNetBridgeStartPromisc(VNetBridge *bridge,      // IN:
                       Bool rtnlLock)           // IN: Acquire RTNL lock
{
   struct net_device *dev = bridge->dev;

   /*
    * Disable wireless cards from going into promiscous mode because those
    * cards which do support RF monitoring would not be able to function
    * correctly i.e. they would not be able to send data packets.
    */
   if (rtnlLock) {
      rtnl_lock();
   }
   if (!bridge->enabledPromisc && !bridge->wirelessAdapter) {
      dev_set_promiscuity(dev, 1);
      bridge->enabledPromisc = TRUE;
      bridge->warnPromisc = FALSE;
      LOG(0, (KERN_NOTICE "bridge-%s: enabled promiscuous mode\n",
	      bridge->name));
   }
   if (rtnlLock) {
      rtnl_unlock();
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeStopPromisc --
 *
 *      Restore saved IFF_PROMISC on the peer interface.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The peer interface IFF_PROMISC flag may be changed.
 *
 *----------------------------------------------------------------------
 */

static void
VNetBridgeStopPromisc(VNetBridge *bridge,       // IN:
                      Bool rtnlLock)            // IN: Acquire RTNL lock
{
   struct net_device *dev = bridge->dev;

   if (rtnlLock) {
      rtnl_lock();
   }
   if (bridge->enabledPromisc && !bridge->wirelessAdapter) {
      dev_set_promiscuity(dev, -1);
      bridge->enabledPromisc = FALSE;
      LOG(0, (KERN_NOTICE "bridge-%s: disabled promiscuous mode\n",
	      bridge->name));
   }
   if (rtnlLock) {
      rtnl_unlock();
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeCheckPromisc --
 *
 *      Make sure IFF_PROMISC on the peer interface is set.
 *
 *      This can be called periodically.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Hopefully enables promiscuous mode again if it should have been enabled.
 *
 *----------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
VNetBridgeCheckPromisc(VNetBridge *bridge)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0)
   if (bridge->enabledPromisc && !bridge->wirelessAdapter) {
      struct net_device *dev = bridge->dev;
      Bool devPromisc = (dev->flags & IFF_PROMISC) != 0;

      if (!devPromisc) {
         if (!bridge->warnPromisc) {
            bridge->warnPromisc = TRUE;
            LOG(0, (KERN_NOTICE "bridge-%s: someone disabled promiscuous mode\n"
	            "Your Ethernet driver is not compatible with VMware's bridged networking.\n",
                    bridge->name));
         }
         rtnl_lock();
         dev_set_promiscuity(dev, 0);
         rtnl_unlock();
      }
   }
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeDevCompatible --
 *
 *      Check whether bridge and network device are compatible.
 *
 * Results:
 *      Non-zero if device is good enough for bridge.  Zero otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER int
VNetBridgeDevCompatible(VNetBridge *bridge,      // IN: Bridge
                        struct net_device *net)  // IN: Network device
{
#ifdef VMW_NETDEV_HAS_NET
   if (compat_dev_net(net) != &init_net) {
      return 0;
   }
#endif
   return strcmp(net->name, bridge->name) == 0;
}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
static int proc_bridge_open(struct inode *inode, struct file *file)
{
       return single_open(file, VNetBridgeProcShow, PDE_DATA(inode));
}

static const struct file_operations proc_bridge_fops = {
       .open           = proc_bridge_open,
       .read           = seq_read,
       .llseek         = seq_lseek,
       .release        = seq_release,
};
#endif

/*
 *----------------------------------------------------------------------
 *
 * VNetBridge_Create --
 *
 *      Creates a bridge. Allocates struct, allocates internal device,
 *      initializes port/jack, and creates a proc entry. Finally, creates an
 *      event sender and register itself with the kernel for device state
 *      change notifications.
 *
 *      At this time the bridge is not yet plugged into the hub, because this
 *      will be done by the caller, i.e. the driver. But we need to know the
 *      hub in order to create an event sender. This allows for enabling
 *      the notification mechanism, which will instantly start firing, which in
 *      turn will bring up the bridge (if present), which eventually will
 *      inject bridge events. Moreover, the bridge will start injecting
 *      packets, which will be dropped on the floor. All in all, this is not
 *      that elegant. Alternatively, we could (i) plug into the hub inside of
 *      this function, which would require adding a few parameters, (ii) split
 *      the function into a create part and a registration part. Both ways are
 *      not consistent with how driver.c plugs the ports into the hub.
 *
 * Results:
 *      Errno. Also returns an allocated jack to connect to,
 *      NULL on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VNetBridge_Create(const char *devName, // IN:  name of device (e.g., "eth0")
                  uint32 flags,        // IN:  configuration flags
                  VNetJack *hubJack,   // IN:  the future hub
                  VNetPort **ret)      // OUT: port to virtual hub
{
   VNetBridge *bridge = NULL;
   static unsigned id = 0;
   int retval = 0;

   *ret = NULL;

   /*
    * Its an error if device name is empty.
    */

   if (devName[0] == '\0') {
      retval = -EINVAL;
      goto out;
   }

   /* complain about unknown/unsupported flags */
   if (flags & ~VNET_BRFLAG_FORCE_SMAC) {
      retval = -EINVAL;
      goto out;
   }

   /*
    * Allocate bridge structure
    */

   bridge = kmalloc(sizeof *bridge, GFP_USER);
   if (bridge == NULL) {
      retval = -ENOMEM;
      goto out;
   }
   memset(bridge, 0, sizeof *bridge);
   spin_lock_init(&bridge->historyLock);
   memcpy(bridge->name, devName, sizeof bridge->name);
   NULL_TERMINATE_STRING(bridge->name);

   /*
    * Initialize jack.
    */

   bridge->port.id = id++;
   bridge->port.next = NULL;

   bridge->port.jack.peer = NULL;
   bridge->port.jack.numPorts = 1;
   VNetSnprintf(bridge->port.jack.name, sizeof bridge->port.jack.name,
		"bridge%u", bridge->port.id);
   bridge->port.jack.private = bridge;
   bridge->port.jack.index = 0;
   bridge->port.jack.procEntry = NULL;
   bridge->port.jack.free = VNetBridgeFree;
   bridge->port.jack.rcv = VNetBridgeReceiveFromVNet;
   bridge->port.jack.cycleDetect = VNetBridgeCycleDetect;
   bridge->port.jack.portsChanged = VNetBridgePortsChanged;
   bridge->port.jack.isBridged = VNetBridgeIsBridged;

   /*
    * Make proc entry for this jack.
    */

   retval = VNetProc_MakeEntry(bridge->port.jack.name, S_IFREG,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
                               &bridge->port.jack.procEntry, &proc_bridge_fops, bridge);
#else
                               &bridge->port.jack.procEntry);
#endif
   if (retval) {
      if (retval == -ENXIO) {
         bridge->port.jack.procEntry = NULL;
      } else {
         goto out;
      }
   } else {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
      bridge->port.jack.procEntry->read_proc = VNetBridgeProcRead;
      bridge->port.jack.procEntry->data = bridge;
#endif
   }

   /*
    * Rest of fields.
    */

   bridge->port.flags = IFF_RUNNING;

   memset(bridge->port.paddr, 0, sizeof bridge->port.paddr);
   memset(bridge->port.ladrf, 0, sizeof bridge->port.ladrf);

   bridge->port.paddr[0] = VMX86_STATIC_OUI0;
   bridge->port.paddr[1] = VMX86_STATIC_OUI1;
   bridge->port.paddr[2] = VMX86_STATIC_OUI2;

   bridge->port.fileOpRead = NULL;
   bridge->port.fileOpWrite = NULL;
   bridge->port.fileOpIoctl = NULL;
   bridge->port.fileOpPoll = NULL;

   /* misc. configuration */
   bridge->forceSmac = (flags & VNET_BRFLAG_FORCE_SMAC) ? TRUE : FALSE;

   /* create event sender */
   retval = VNetHub_CreateSender(hubJack, &bridge->eventSender);
   if (retval != 0) {
      goto out;
   }

   /*
    * on RHEL3 Linux 2.4.21-47 (others maybe too) the notifier does not fire
    * and bring up the bridge as expected, thus we bring it up manually
    * *before* registering the notifier (PR306435)
    */
   VNetBridgeUp(bridge, TRUE);

   /*
    * register notifier for network device state change notifications, the
    * notifier will fire right away, and the notifier handler will bring up
    * the bridge (see exception above)
    */
   bridge->notifier.notifier_call = VNetBridgeNotify;
   bridge->notifier.priority = 0;
   register_netdevice_notifier(&bridge->notifier);

   /* return bridge */
   *ret = &bridge->port;
   LOG(1, (KERN_DEBUG "bridge-%s: attached\n", bridge->name));
   return 0;

out:
   if (bridge != NULL) {
      kfree(bridge);
   }
   return retval;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeFree --
 *
 *      Unregister from device state notifications, disable the bridge,
 *      destroy sender, remove proc entry, cleanup smac, and deallocate
 *      struct.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
VNetBridgeFree(VNetJack *this) // IN: jack to free
{
   VNetBridge *bridge = (VNetBridge*)this->private;

   /* unregister notifier */
   if (bridge->notifier.notifier_call != NULL) {
      int err;

      err = compat_unregister_netdevice_notifier(&bridge->notifier);
      if (err != 0) {
         LOG(0, (KERN_NOTICE "Can't unregister netdevice notifier (%d)\n",
                 err));
      }
      bridge->notifier.notifier_call = NULL;
   }

   /* disable bridge */
   if (bridge->dev != NULL) {
      LOG(1, (KERN_DEBUG "bridge-%s: disabling the bridge\n", bridge->name));
      VNetBridgeDown(bridge, TRUE);
   }

   /* destroy event sender */
   VNetEvent_DestroySender(bridge->eventSender);
   bridge->eventSender = NULL;

   /* remove /proc entry */
   if (this->procEntry) {
      VNetProc_RemoveEntry(this->procEntry);
   }

   if (bridge->smac){
      SMAC_CleanupState(&(bridge->smac));
   }

   /* free bridge */
   LOG(1, (KERN_DEBUG "bridge-%s: detached\n", bridge->name));
   kfree(bridge);
}


/*
 *----------------------------------------------------------------------
 *
 * VNetCallSMACFunc --
 *
 *      Wrapper for SMAC functions. The skb must be linear.
 *
 * Results:
 *      Packet Status.
 *
 * Side effects:
 *      The skb buffer is freed if not successful otherwise it points to
 *      the clone.
 *
 *----------------------------------------------------------------------
 */

static PacketStatus
VNetCallSMACFunc(struct SMACState *state, // IN: pointer to state
                 struct sk_buff **skb,    // IN/OUT: packet to process
                 void *startOfData,       // IN: points to start of data
                 SMACFunc func,           // IN: function to be called
                 unsigned int len)        // IN: length including ETH header
{
   SMACPackets packets = { {0} };
   PacketStatus status;

   SKB_LINEAR_ASSERT(*skb);

   packets.orig.skb = *skb;
   packets.orig.startOfData = startOfData;
   packets.orig.len = len;

   status = func(state, &packets);
   if (status != PacketStatusForwardPacket) {
      dev_kfree_skb(*skb);
      return status;
   }

   if (packets.clone.skb) {
      dev_kfree_skb(*skb);
      *skb = packets.clone.skb;
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeReceiveFromVNet --
 *
 *      This jack is receiving a packet from a vnet.  This function
 *      sends down (i.e., out on the host net device) if the packet
 *      isn't destined for the host, and it sends up (i.e.,
 *      simulates a receive for the host) if the packet
 *      satisfies the host's packet filter.
 *
 *      When the function sends up it keeps a reference to the
 *      packet in a history list so that we can avoid handing
 *      a VM a copy of its own packet.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Frees skb.  Checks if host device is still using
 *      promiscuous mode.
 *
 *----------------------------------------------------------------------
 */

void
VNetBridgeReceiveFromVNet(VNetJack        *this, // IN: jack
                          struct sk_buff  *skb)  // IN: pkt to receive
{
   VNetBridge *bridge = (VNetBridge*)this->private;
   struct net_device *dev = bridge->dev;
   uint8 dest[ETH_ALEN];
   struct sk_buff *clone;

   LOG(3, (KERN_DEBUG "bridge-%s: transmit %d\n",
           bridge->name, (int) skb->len));

   if (!dev) {
      dev_kfree_skb(skb);
      return;
   }

   /*
    * skb might be freed by wireless code, so need to keep
    * a local copy of the MAC rather than a pointer to it.
    */

   memcpy(dest, SKB_2_DESTMAC(skb), ETH_ALEN);

   /*
    * Check promiscuous bit periodically
    */

   VNetBridgeCheckPromisc(bridge);

#ifdef notdef
   // xxx;
   /*
    * We need to send the packet both up to the host and down
    * to the interface.
    * However, we ignore packets destined only for this hub.
    */

   for (i = 0; i < VNET_PORTS_PER_HUB; i++) {
      VNetPort *p = &port->hub->port[i];
      if (UP_AND_RUNNING(p->flags) && MAC_EQ(dest, p->paddr)) {
	 return;
      }
   }
#endif

   /*
    * SMAC processing. SMAC interfaces that the skb is linear, so ensure that
    * this is the case prior to calling out.
    */

   if (bridge->smac) {
      if (compat_skb_is_nonlinear(skb) && compat_skb_linearize(skb)) {
         LOG(4, (KERN_NOTICE "bridge-%s: couldn't linearize, packet dropped\n",
                 bridge->name));
         return;
      }
      if (VNetCallSMACFunc(bridge->smac, &skb, skb->data,
                           SMAC_CheckPacketToHost, skb->len) !=
          PacketStatusForwardPacket) {
         LOG(4, (KERN_NOTICE "bridge-%s: packet dropped\n", bridge->name));
	 return;
      }
   }

   /*
    * Send down (imitate packet_sendmsg)
    *
    * Do this only if the packet is not addressed to the peer,
    * and the packet size is not too big.
    */

   dev_lock_list();
   if (MAC_EQ(dest, dev->dev_addr) ||
       skb->len > dev->mtu + dev->hard_header_len) {
      dev_unlock_list();
   } else {
#     if 0 // XXX we should do header translation
      if ((dev->flags & IFF_SOFTHEADERS) != 0) {
	 if (skb->len > dev->mtu) {
	    clone = NULL;
	 } else {
	    clone = dev_alloc_skb(skb->len + dev->hard_header_len, GFP_ATOMIC);
	 }
	 if (clone != NULL) {
	    skb_reserve(clone, dev->hard_header_len);
	    if (dev->hard_header != NULL) {
	       dev->hard_header(clone, dev, ETH_P_IP, NULL, NULL, skb->len);
	    }
	    memcpy(skb_put(clone, skb->len), skb->data, skb->len);
	 }
      }
#     endif
      clone = skb_clone(skb, GFP_ATOMIC);
      if (clone == NULL) {
	 dev_unlock_list();
      } else {
         skb_set_owner_w(clone, bridge->sk);
	 clone->protocol = ((struct ethhdr *)skb->data)->h_proto; // XXX
	 if ((dev->flags & IFF_UP) != 0) {
	    dev_unlock_list();
	    DEV_QUEUE_XMIT(clone, dev, 0);
	 } else {
	    dev_unlock_list();
	    dev_kfree_skb(clone);
	 }
      }
   }

   /*
    * Send up (imitate Ethernet receive)
    *
    * Do this if the packet is addressed to the peer (or is broadcast, etc.).
    *
    * This packet will get back to us, via VNetBridgeReceive.
    * We save it so we can recognize it (and its clones) again.
    */

   if (VNetPacketMatch(dest, dev->dev_addr, allMultiFilter, dev->flags)) {
      clone = skb_clone(skb, GFP_ATOMIC);
      if (clone) {
	 unsigned long flags;
	 int i;

	 atomic_inc(&clone->users);

	 clone->dev = dev;
	 clone->protocol = eth_type_trans(clone, dev);
	 spin_lock_irqsave(&bridge->historyLock, flags);
	 for (i = 0; i < VNET_BRIDGE_HISTORY; i++) {
	    if (bridge->history[i] == NULL) {
	       bridge->history[i] = clone;
#	       if LOGLEVEL >= 3
	       {
		  int j;
		  int count = 0;
		  for (j = 0; j < VNET_BRIDGE_HISTORY; j++) {
		     if (bridge->history[j] != NULL) {
			count++;
		     }
		  }
		  LOG(3, (KERN_DEBUG "bridge-%s: host slot %d history %d\n",
			  bridge->name, i, count));
	       }
#	       endif
	       break;
	    }
	 }
	 if (i >= VNET_BRIDGE_HISTORY) {
	    LOG(1, (KERN_NOTICE "bridge-%s: history full\n",
		    bridge->name));

	    for (i = 0; i < VNET_BRIDGE_HISTORY; i++) {
	       struct sk_buff *s = bridge->history[i];

	       /*
		* We special case 0 to avoid races with another thread on
		* another cpu wanting to use the 0 entry. This could happen
		* when we release the lock to free the former entry.
		* See bug 11231 for details.
		*/
	       if (i == 0) {
		  bridge->history[0] = clone;
	       } else {
		  bridge->history[i] = NULL;
	       }
	       if (s) {
	       	  spin_unlock_irqrestore(&bridge->historyLock, flags);
		  dev_kfree_skb(s);
		  spin_lock_irqsave(&bridge->historyLock, flags);
	       }
	    }
	 }
         spin_unlock_irqrestore(&bridge->historyLock, flags);

         /*
          * We used to cli() before calling netif_rx() here. It was probably
          * unneeded (as we never did it in netif.c, and the code worked). In
          * any case, now that we are using netif_rx_ni(), we should certainly
          * not do it, or netif_rx_ni() will deadlock on the cli() lock --hpreg
          */

	 netif_rx_ni(clone);
#	 if LOGLEVEL >= 4
	 do_gettimeofday(&vnetTime);
#	 endif
      }
   }

   // xxx;
   dev_kfree_skb(skb);
}


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeCycleDetect --
 *
 *      Cycle detection algorithm.
 *
 * Results:
 *      TRUE if a cycle was detected, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
VNetBridgeCycleDetect(VNetJack *this,       // IN: jack
                      int       generation) // IN: generation
{
   VNetBridge *bridge = (VNetBridge*)this->private;
   return VNetCycleDetectIf(bridge->name, generation);
}


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgePortsChanged --
 *
 *      The number of ports connected to this jack has change, react
 *      accordingly by starting/stopping promiscuous mode based on
 *      whether any peers exist.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Promiscuous mode may be started or stopped.
 *
 *----------------------------------------------------------------------
 */

void
VNetBridgePortsChanged(VNetJack *this) // IN: jack
{
   VNetBridge *bridge = (VNetBridge*)this->private;
   if (bridge->dev) {
      if (VNetGetAttachedPorts(this)) {
         VNetBridgeStartPromisc(bridge, TRUE);
      } else {
         VNetBridgeStopPromisc(bridge, TRUE);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeIsBridged --
 *
 *      Reports if the bridged interface is up or down.
 *
 * Results:
 *      1 - we are bridged but the interface is not up
 *      2 - we are bridged and the interface is up
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VNetBridgeIsBridged(VNetJack *this) // IN: jack
{
   VNetBridge *bridge = (VNetBridge*)this->private;
   if (bridge->dev) {
      return 2;
   } else {
      return 1;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeIsDeviceWireless --
 *
 *      Check if the device is a wireless adapter, depending on the version
 *      of the wireless extension present in the kernel.
 *
 * Results:
 *      TRUE if the device is wireless, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Bool
VNetBridgeIsDeviceWireless(struct net_device *dev) //IN: sock
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 22)
#  if defined(CONFIG_WIRELESS_EXT)
   return dev->ieee80211_ptr != NULL || dev->wireless_handlers != NULL;
#  else
   return dev->ieee80211_ptr != NULL;
#  endif
#elif defined(CONFIG_WIRELESS_EXT)
   return dev->wireless_handlers != NULL;
#elif !defined(CONFIG_NET_RADIO)
   return FALSE;
#elif defined WIRELESS_EXT && WIRELESS_EXT > 19
   return dev->wireless_handlers != NULL;
#elif defined WIRELESS_EXT && WIRELESS_EXT > 12
   return dev->wireless_handlers != NULL || dev->get_wireless_stats != NULL;
#else
   return dev->get_wireless_stats != NULL;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeSendLinkStateEvent --
 *
 *      Sends a link state event.
 *
 * Results:
 *      Returns 0 if successful, or a negative value if an error occurs.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
VNetBridgeSendLinkStateEvent(VNetBridge *bridge, // IN: the bridge
                             uint32 adapter,     // IN: the adapter
                             Bool up)            // IN: the link state
{
   VNet_LinkStateEvent event;
   int res;

   event.header.size = sizeof event;
   res = VNetEvent_GetSenderId(bridge->eventSender, &event.header.senderId);
   if (res != 0) {
      LOG(1, (KERN_NOTICE "bridge-%s: can't send link state event, "
              "getSenderId failed (%d)\n", bridge->name, res));
      return res;
   }
   event.header.eventId = 0;
   event.header.classSet = VNET_EVENT_CLASS_BRIDGE;
   event.header.type = VNET_EVENT_TYPE_LINK_STATE;
   event.adapter = adapter;
   event.up = up;
   res = VNetEvent_Send(bridge->eventSender, &event.header);
   if (res != 0) {
      LOG(1, (KERN_NOTICE "bridge-%s: can't send link state event, send "
              "failed (%d)\n", bridge->name, res));
   }
   return res;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeUp --
 *
 *      Bring a bridge up.  Gets peer's device structure, verifies
 *      that interface is up, checks the header length,
 *      allocates a socket, adds a packet handler to the network
 *      stack, and then places the peer's device in promiscuous
 *      mode.
 *
 * Results:
 *      errno.
 *
 * Side effects:
 *      Bridging may be brought up with a peer interface.
 *
 *----------------------------------------------------------------------
 */

static int
VNetBridgeUp(VNetBridge *bridge, // IN: bridge struct
             Bool rtnlLock)      // IN: acquire RTNL lock
{
   int retval = 0;

   if (bridge->dev != NULL) {
      LOG(0, (KERN_NOTICE "bridge-%s: already up\n", bridge->name));
      goto out;
   }

   /*
    * Get peer device structure
    */

   dev_lock_list();
   bridge->dev = DEV_GET(bridge);
   LOG(2, (KERN_DEBUG "bridge-%s: got dev %p\n",
	   bridge->name, bridge->dev));
   if (bridge->dev == NULL) {
      dev_unlock_list();
      retval = -ENODEV;
      goto out;
   }
   if (!(bridge->dev->flags & IFF_UP)) {
      LOG(2, (KERN_DEBUG "bridge-%s: interface %s is not up\n",
              bridge->name, bridge->dev->name));
      dev_unlock_list();
      retval = -ENODEV;
      goto out;
   }

   /*
    * At a minimum, the header size should be the same as ours.
    *
    * XXX we should either do header translation or ensure this
    * is an Ethernet.
    */

   if (bridge->dev->hard_header_len != ETH_HLEN) {
      LOG(1, (KERN_DEBUG "bridge-%s: can't bridge with %s, bad header length %d\n",
	      bridge->name, bridge->dev->name, bridge->dev->hard_header_len));
      dev_unlock_list();
      retval = -EINVAL;
      goto out;
   }

   /*
    * Get a socket to play with
    *
    * We set the dead field so we don't get a call back from dev_kfree_skb().
    * (The alternative is to support the callback.)
    */

   bridge->sk = compat_sk_alloc(bridge, GFP_ATOMIC);
   if (bridge->sk == NULL) {
      dev_unlock_list();
      retval = -ENOMEM;
      goto out;
   }
   sock_init_data(NULL, bridge->sk);
   SET_SK_DEAD(bridge->sk);

   if (VNetBridgeIsDeviceWireless(bridge->dev)) {
      LOG(1, (KERN_NOTICE "bridge-%s: device is wireless, enabling SMAC\n",
              bridge->name));
      bridge->wirelessAdapter = TRUE;
   }

   /*
    * If it is a wireless adapter initialize smac struct.
    */

   if (bridge->wirelessAdapter || bridge->forceSmac) {
      SMAC_InitState(&(bridge->smac));
      if (bridge->smac) {
         /*
          * Store the MAC address of the adapter
          */

         SMAC_SetMac(bridge->smac, bridge->dev->dev_addr);
      }
   }

   /*
    * Link up with the peer device by adding a
    * packet handler to the networking stack.
    */

   bridge->pt.func = VNetBridgeReceiveFromDev;
   bridge->pt.type = htons(ETH_P_ALL);
   bridge->pt.dev = bridge->dev;

   /*
    * TurboLinux10 uses 2.6.0-test5, which we do not support, so special case it,
    * 2.6.0 with tl_kernel_version_h is 2.6.0-pre5...
    */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0) || \
   (LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 0) && defined(__tl_kernel_version_h__))
   bridge->pt.data = bridge->sk;
#else
   bridge->pt.af_packet_priv = bridge->sk;
#endif
   bridge->enabledPromisc = FALSE;
   bridge->warnPromisc = FALSE;
   dev_add_pack(&bridge->pt);
   dev_unlock_list();

   /*
    * Put in promiscuous mode if need be.
    */

   compat_mutex_lock(&vnetStructureMutex);
   if (VNetGetAttachedPorts(&bridge->port.jack)) {
      VNetBridgeStartPromisc(bridge, rtnlLock);
   }
   compat_mutex_unlock(&vnetStructureMutex);

   /* send link state up event */
   retval = VNetBridgeSendLinkStateEvent(bridge, bridge->dev->ifindex, TRUE);
   if (retval != 0) {
      LOG(1, (KERN_NOTICE "bridge-%s: can't send link state event (%d)\n",
              bridge->name, retval));
      goto out;
   }

   LOG(1, (KERN_DEBUG "bridge-%s: up\n", bridge->name));

   /*
    * Return
    */

out:
   if (retval != 0) {
      if (bridge->sk != NULL) {
	 sk_free(bridge->sk);
	 bridge->sk = NULL;
      }
      bridge->dev = NULL;
   }
   return retval;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeDown --
 *
 *      Bring a bridge down.  Stops promiscuous mode, removes the
 *      packet handler from the network stack, and frees the
 *      socket.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Bridging is brought down.
 *
 *----------------------------------------------------------------------
 */

static void
VNetBridgeDown(VNetBridge *bridge, // IN: bridge
               Bool rtnlLock)      // IN: acquire RTNL lock
{
   int retval;

   if (bridge->dev == NULL) {
      LOG(0, (KERN_NOTICE "bridge-%s: already down\n", bridge->name));
      return;
   }

   /* send link state down event */
   retval = VNetBridgeSendLinkStateEvent(bridge, bridge->dev->ifindex, FALSE);
   if (retval != 0) {
      LOG(1, (KERN_NOTICE "bridge-%s: can't send link state event (%d)\n",
              bridge->name, retval));
   }

   VNetBridgeStopPromisc(bridge, rtnlLock);
   if (bridge->smac){
      SMAC_SetMac(bridge->smac, NULL);
   }
   bridge->dev = NULL;
   dev_remove_pack(&bridge->pt);
   sk_free(bridge->sk);
   bridge->sk = NULL;

   LOG(1, (KERN_DEBUG "bridge-%s: down\n", bridge->name));
}


/*
 *-----------------------------------------------------------------------------
 *
 * VNetBridgeNotifyLogBridgeUpError --
 *
 *      Logs a bridge up error for the notify function following this function.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
VNetBridgeNotifyLogBridgeUpError(int errno,        // IN: the error number
                                 char *bridgeName, // IN: the bridge name
                                 char *devName)    // IN: the device name
{
   switch (errno) {
      case -ENODEV:
         LOG(0, (KERN_WARNING "bridge-%s: interface %s not found or not "
                 "up\n", bridgeName, devName));
	 break;
      case -EINVAL:
         LOG(0, (KERN_WARNING "bridge-%s: interface %s is not a valid "
                 "Ethernet interface\n", bridgeName, devName));
         break;
      case -ENOMEM:
         LOG(0, (KERN_WARNING "bridge-%s: failed to allocate memory\n",
                 bridgeName));
         break;
      default:
         /* This should never happen --hpreg */
         LOG(0, (KERN_WARNING "bridge-%s: failed to enable the bridge to "
                 "interface %s (error %d)\n", bridgeName, devName,
                 -errno));
         break;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * VNetBridgeNotify --
 *
 *      Callback on peer device state change.  The function brings
 *      the bridge up/down in response to changes in the peer device.
 *
 * Results:
 *      NOTIFY_DONE
 *
 * Side effects:
 *      Promiscuous mode is changed when bridge brought up/down.
 *
 *-----------------------------------------------------------------------------
 */

static int
VNetBridgeNotify(struct notifier_block *this, // IN: callback data (bridge)
                 u_long msg,                  // IN: type of event
                 void *data)                  // IN: device pertaining to event
{
   VNetBridge *bridge = list_entry(this, VNetBridge, notifier);
   struct net_device *dev = (struct net_device *) data;

   switch (msg) {
   case NETDEV_UNREGISTER:
      LOG(2, (KERN_DEBUG "bridge-%s: interface %s is unregistering\n",
              bridge->name, dev->name));
      if (dev == bridge->dev) {
         /* This should never happen --hpreg */
         LOG(0, (KERN_WARNING "bridge-%s: interface %s unregistered without "
                 "going down! Disabling the bridge\n", bridge->name,
                 dev->name));
         VNetBridgeDown(bridge, FALSE);
      }
      break;

   case NETDEV_DOWN:
      LOG(2, (KERN_DEBUG "bridge-%s: interface %s is going down\n",
              bridge->name, dev->name));
      if (dev == bridge->dev) {
         LOG(1, (KERN_DEBUG "bridge-%s: disabling the bridge on dev down\n",
                 bridge->name));
         VNetBridgeDown(bridge, FALSE);
      }
      break;

   case NETDEV_UP:
      LOG(2, (KERN_DEBUG "bridge-%s: interface %s is going up\n",
              bridge->name, dev->name));
      if (bridge->dev == NULL && VNetBridgeDevCompatible(bridge, dev)) {
         int errno;

         LOG(1, (KERN_DEBUG "bridge-%s: enabling the bridge on dev up\n",
                 bridge->name));
	 errno = VNetBridgeUp(bridge, FALSE);
         if (errno != 0) {
            VNetBridgeNotifyLogBridgeUpError(errno, bridge->name, dev->name);
         }
      }
      break;

   default:
      LOG(2, (KERN_DEBUG "bridge-%s: interface %s is sending notification "
              "0x%lx\n", bridge->name, dev->name, msg));
      break;
   }

   return NOTIFY_DONE;
}


/*
 *----------------------------------------------------------------------
 *
 * RangeInLinearSKB --
 *
 *      Checks if the given number of bytes from a given offset resides
 *      within the linear part of the skb.  If not then attempts to
 *      linearize the skb.
 *
 * Results:
 *      Returns TRUE if the range of bytes is already in the linear
 *      portion or if linearize succeeded.  Otherwise, returns FALSE if
 *      the linearize operation fails.
 *
 * Side effects:
 *      As in skb_linearize().
 *
 *----------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER Bool
RangeInLinearSKB(struct sk_buff *skb, // IN:
                 unsigned int start,  // IN:  Start offset
                 unsigned int length) // IN:  How many bytes
{
   if (LIKELY(!compat_skb_is_nonlinear(skb) ||
              start + length <= compat_skb_headlen(skb))) {
      /*
       * Nothing to do.
       */

      return TRUE;
   }

   return compat_skb_linearize(skb) == 0;
}


/*
 * Not all kernel versions have NEXTHDR_MOBILITY defined.
 */

#ifndef NEXTHDR_MOBILITY
#  define NEXTHDR_MOBILITY 135 /* Mobility header. */
#endif


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeComputeHeaderPosIPv6 --
 *
 *      Compute correct position of transport header in IPv6 packets.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Transport header pointer updated to point to the PDU contained
 *      in the packet.
 *
 *----------------------------------------------------------------------
 */

static void
VNetBridgeComputeHeaderPosIPv6(struct sk_buff *skb) // IN:
{
   struct ipv6hdr *ipv6Hdr;
   unsigned int offset; /* Offset from skb->data. */
   unsigned int headerLen; /* Length of current header. */
   uint8 nextHeader;

   /*
    * Check if the start of the network header is within the linear part of
    * skb.  If not, then linearize the skb.
    */

   if (UNLIKELY(compat_skb_network_header(skb) < skb->data ||
                compat_skb_network_header(skb) >= skb->data +
                                                  compat_skb_headlen(skb))) {
      if (compat_skb_linearize(skb)) {
         return; /* Bail out. */
      }
   }

   offset = compat_skb_network_offset(skb);
   if (!RangeInLinearSKB(skb, offset, sizeof *ipv6Hdr)) {
      return; /* Bail out. */
   }

   ipv6Hdr = (struct ipv6hdr *)compat_skb_network_header(skb);
   headerLen = sizeof *ipv6Hdr;
   offset += headerLen; /* End of IPv6 header (not including extensions). */

   /*
    * All IPv6 extension headers begin with a "next header" field (one byte),
    * and most of them have a "header length" field (as the 2nd byte).  In each
    * iteration, we find the length of the extension header and add it to
    * offset from the beginning of skb.  And, in each iteration we update the
    * next header variable.  When we return from the following for loop, offset
    * would have incremented by the length of each of the extension header,
    * and next header type will be something else than an IPv6 extension header
    * signifying that we have walked through the entire IPv6 header.  We set
    * the transport header's offset to the value of this offset before exiting
    * the for loop.
    */

   nextHeader = ipv6Hdr->nexthdr;
   for (;;) {
      switch (nextHeader) {
      case NEXTHDR_HOP:
      case NEXTHDR_ROUTING:
      case NEXTHDR_AUTH:
      case NEXTHDR_DEST:
      case NEXTHDR_MOBILITY:
         /*
          * We need to check two bytes in the option header:  next header and
          * header extension length.
          */

         if (!RangeInLinearSKB(skb, offset, 2)) {
            return; /* Bail out. */
         }
         headerLen = skb->data[offset + 1];
         if (nextHeader == NEXTHDR_AUTH) {
            headerLen = (headerLen + 2) << 2; /* See RFC 2402. */
         } else {
            headerLen = (headerLen + 1) << 3; /* See ipv6_optlen(). */
         }

         break;

      case NEXTHDR_FRAGMENT:
      case NEXTHDR_ESP:
      case NEXTHDR_NONE:
         /*
          * We stop walking if we find the fragment header (NEXTHDR_FRAGMENT).
          * If the payload is encrypted we may not know the start of the
          * transport header [1].  So, we just return.  Same applies when
          * nothing follows this header (NEXTHDR_NONE).
          * [1]:  http://www.cu.ipv6tf.org/literatura/chap8.pdf
          */

         return;

       default:
         /*
          * We have walked through all IPv6 extension headers.  Let's set the
          * transport header and return.
          */

         compat_skb_set_transport_header(skb, offset);
         return;
      }

      nextHeader = skb->data[offset];
      offset += headerLen;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeComputeHeaderPos --
 *
 *      Compute correct position for UDP/TCP header.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      transport header pointer updated to point to the tcp/udp header.
 *
 *----------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
VNetBridgeComputeHeaderPos(struct sk_buff *skb) // IN: buffer to examine
{
   /* Maybe some kernel gets it right... */
   if (compat_skb_network_header_len(skb)) {
      return;
   }
   switch (be16_to_cpu(skb->protocol)) {
      case ETH_P_IP: {
         struct iphdr *ipHdr = compat_skb_ip_header(skb);

         compat_skb_set_transport_header(skb, compat_skb_network_offset(skb) +
                                              ipHdr->ihl * 4);
         break;
      }

      case ETH_P_IPV6:
         VNetBridgeComputeHeaderPosIPv6(skb);
         break;

      default:
         LOG(3, (KERN_DEBUG "Unknown EII protocol %04X: csum at %d\n",
                 be16_to_cpu(skb->protocol), compat_skb_csum_offset(skb)));
         break;
   }
}


/*
 * We deal with three types of kernels:
 * New kernels: skb_shinfo() has gso_size member, and there is
 *              skb_gso_segment() helper to split GSO skb into flat ones.
 * Older kernels: skb_shinfo() has tso_size member, and there is
 *                no helper.
 * Oldest kernels: without any segmentation offload support.
 */
#if defined(NETIF_F_GSO) || LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 18)
#define VNetBridgeIsGSO(skb) skb_shinfo(skb)->gso_size
#define VNetBridgeGSOSegment(skb) skb_gso_segment(skb, 0)
#elif defined(NETIF_F_TSO)
#define VNetBridgeIsGSO(skb) skb_shinfo(skb)->tso_size


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeGSOSegment --
 *
 *	Split a large TCP/IPv4 sk_buff into multiple sk_buffs of
 *	size skb_shinfo(skb)->tso_size
 *	Called from VNetBridgeSendLargePacket().
 *
 * Results:
 *	List of skbs created.
 *
 * Side effects:
 *	The incoming packet is split into multiple packets.
 *
 *----------------------------------------------------------------------
 */

static struct sk_buff *
VNetBridgeGSOSegment(struct sk_buff *skb)        // IN: packet to split
{
   struct sk_buff *segs = NULL;
   struct sk_buff **next = &segs;
   int bytesPerPacket, bytesLeft;
   int macHdrLen, ipHdrLen, tcpHdrLen, allHdrLen;
   int curByteOffset;
   uint16 ipID;
   uint32 seqNo;

   if (((struct ethhdr *)compat_skb_mac_header(skb))->h_proto != htons(ETH_P_IP)) {
      return ERR_PTR(-EPFNOSUPPORT);
   }

   if (compat_skb_ip_header(skb)->protocol != IPPROTO_TCP) {
      return ERR_PTR(-EPROTONOSUPPORT);
   }

   macHdrLen = compat_skb_network_header(skb) - compat_skb_mac_header(skb);
   ipHdrLen = compat_skb_ip_header(skb)->ihl << 2;
   tcpHdrLen = compat_skb_tcp_header(skb)->doff << 2;
   allHdrLen = macHdrLen + ipHdrLen + tcpHdrLen;

   ipID = ntohs(compat_skb_ip_header(skb)->id);
   seqNo = ntohl(compat_skb_tcp_header(skb)->seq);

   /* Host TCP stack populated this (MSS) for the host NIC driver */
   bytesPerPacket = skb_shinfo(skb)->tso_size;

   bytesLeft = skb->len - allHdrLen;
   curByteOffset = allHdrLen;

   while (bytesLeft) {
      struct sk_buff *newSkb;
      int payloadSize = (bytesLeft < bytesPerPacket) ? bytesLeft : bytesPerPacket;

      newSkb = dev_alloc_skb(payloadSize + allHdrLen + NET_IP_ALIGN);
      if (!newSkb) {
         while (segs) {
            newSkb = segs;
            segs = segs->next;
            newSkb->next = NULL;
            dev_kfree_skb(newSkb);
         }
         return ERR_PTR(-ENOMEM);
      }
      skb_reserve(newSkb, NET_IP_ALIGN);
      newSkb->dev = skb->dev;
      newSkb->protocol = skb->protocol;
      newSkb->pkt_type = skb->pkt_type;
      newSkb->ip_summed = VM_TX_CHECKSUM_PARTIAL;

      /*
       * MAC+IP+TCP copy
       * This implies that ALL fields in the IP and TCP headers are copied from
       * the original skb. This is convenient: we'll only fix up fields that
       * need to be changed below
       */
      memcpy(skb_put(newSkb, allHdrLen), skb->data, allHdrLen);

      /* Fix up pointers to different layers */
      compat_skb_reset_mac_header(newSkb);
      compat_skb_set_network_header(newSkb, macHdrLen);
      compat_skb_set_transport_header(newSkb, macHdrLen + ipHdrLen);

      /* Payload copy */
      skb_copy_bits(skb, curByteOffset, compat_skb_tail_pointer(newSkb), payloadSize);
      skb_put(newSkb, payloadSize);

      curByteOffset+=payloadSize;
      bytesLeft -= payloadSize;

      /* Fix up IP hdr */
      compat_skb_ip_header(newSkb)->tot_len = htons(payloadSize + tcpHdrLen + ipHdrLen);
      compat_skb_ip_header(newSkb)->id = htons(ipID);
      compat_skb_ip_header(newSkb)->check = 0;
      /* Recompute new IP checksum */
      compat_skb_ip_header(newSkb)->check =
              ip_fast_csum(compat_skb_network_header(newSkb),
                           compat_skb_ip_header(newSkb)->ihl);

      /* Fix up TCP hdr */
      compat_skb_tcp_header(newSkb)->seq = htonl(seqNo);
      /* Clear FIN/PSH if not last packet */
      if (bytesLeft > 0) {
         compat_skb_tcp_header(newSkb)->fin = 0;
         compat_skb_tcp_header(newSkb)->psh = 0;
      }
      /* Recompute partial TCP checksum */
      compat_skb_tcp_header(newSkb)->check =
         ~csum_tcpudp_magic(compat_skb_ip_header(newSkb)->saddr,
                            compat_skb_ip_header(newSkb)->daddr,
                            payloadSize+tcpHdrLen, IPPROTO_TCP, 0);

      /* Offset of field */
      newSkb->csum = offsetof(struct tcphdr, check);

      /* Join packet to the list of segments */
      *next = newSkb;
      next = &newSkb->next;

      /* Bump up our counters */
      ipID++;
      seqNo += payloadSize;

   }
   return segs;
}
#else
#define VNetBridgeIsGSO(skb) (0)
#define VNetBridgeGSOSegment(skb) ERR_PTR(-ENOSYS)
#endif


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeSendLargePacket --
 *
 *      Split and send a large TCP/IPv4 sk_buff into multiple sk_buffs which
 *      fits on wire.  Called from VNetBridgeReceiveFromDev(), which is a
 *	protocol handler called from the bottom half, so steady as she
 *	goes...
 *
 *	skb passed in is deallocated by function.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The incoming packet is split into multiple packets and sent to the
 *      vnet.
 *
 *----------------------------------------------------------------------
 */

void
VNetBridgeSendLargePacket(struct sk_buff *skb,        // IN: packet to split
                          VNetBridge *bridge)         // IN: bridge
{
   struct sk_buff *segs;

   segs = VNetBridgeGSOSegment(skb);
   dev_kfree_skb(skb);
   if (IS_ERR(segs)) {
      LOG(1, (KERN_DEBUG "bridge-%s: cannot segment packet: error %ld\n",
              bridge->name, PTR_ERR(segs)));
      return;
   }

   while (segs) {
      struct sk_buff *newSkb;

      newSkb = segs;
      segs = newSkb->next;
      newSkb->next = NULL;
      /* Send it along */
      skb = newSkb;
      VNetSend(&bridge->port.jack, newSkb);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeReceiveFromDev --
 *
 *      Receive a packet from a bridged peer device
 *
 *      This is called from the bottom half.  Must be careful.
 *
 * Results:
 *      errno.
 *
 * Side effects:
 *      A packet may be sent to the vnet.
 *
 *----------------------------------------------------------------------
 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 14) && \
    !defined(VMW_TL10S64_WORKAROUND)
int
VNetBridgeReceiveFromDev(struct sk_buff *skb,         // IN: packet to receive
                         struct net_device *dev,      // IN: unused
                         struct packet_type *pt)      // IN: pt (pointer to bridge)
#else
int
VNetBridgeReceiveFromDev(struct sk_buff *skb,         // IN: packet to receive
                         struct net_device *dev,      // IN: unused
                         struct packet_type *pt,      // IN: pt (pointer to bridge)
                         struct net_device *real_dev) // IN: real device, unused
#endif
{
   VNetBridge *bridge = list_entry(pt, VNetBridge, pt);
   int i;
   unsigned long flags;

   if (bridge->dev == NULL) {
      LOG(3, (KERN_DEBUG "bridge-%s: received %d closed\n",
	      bridge->name, (int) skb->len));
      dev_kfree_skb(skb);
      return -EIO;	// value is ignored anyway
   }

   /*
    * Check is this is a packet that we sent up to the host, and if
    * so then don't bother to receive the packet.
    */

   spin_lock_irqsave(&bridge->historyLock, flags);
   for (i = 0; i < VNET_BRIDGE_HISTORY; i++) {
      struct sk_buff *s = bridge->history[i];
      if (s != NULL &&
	  (s == skb || SKB_IS_CLONE_OF(skb, s))) {
	 bridge->history[i] = NULL;
	 spin_unlock_irqrestore(&bridge->historyLock, flags);
	 dev_kfree_skb(s);
	 LOG(3, (KERN_DEBUG "bridge-%s: receive %d self %d\n",
		 bridge->name, (int) skb->len, i));
	 dev_kfree_skb(skb);
	 return 0;
      }
   }
   spin_unlock_irqrestore(&bridge->historyLock, flags);

#  if LOGLEVEL >= 4
   {
      struct timeval now;
      do_gettimeofday(&now);
      LOG(3, (KERN_DEBUG "bridge-%s: time %d\n",
	      bridge->name,
	      (int)((now.tv_sec * 1000000 + now.tv_usec)
                    - (vnetTime.tv_sec * 1000000 + vnetTime.tv_usec))));
   }
#  endif

   /*
    * SMAC might linearize the skb, but linearizing a shared skb is a no-no,
    * so check for sharing before calling out to SMAC.
    */
   skb = skb_share_check(skb, GFP_ATOMIC);
   if (!skb) {
      return 0;
   }

   if (bridge->smac) {
      /*
       * Wireless driver processes the packet and processes the ethernet header
       * and the length is reduced by the amount. We need the raw ethernet
       * packet length hence add the ethernet header length for incoming
       * packets.
       *
       * Note that SMAC interfaces assume skb linearity.
       */
      if (compat_skb_is_nonlinear(skb) && compat_skb_linearize(skb)) {
         LOG(4, (KERN_NOTICE "bridge-%s: couldn't linearize, packet dropped\n",
                 bridge->name));
         return 0;
      }
      if (VNetCallSMACFunc(bridge->smac, &skb, compat_skb_mac_header(skb),
                           SMAC_CheckPacketFromHost, skb->len + ETH_HLEN) !=
          PacketStatusForwardPacket) {
         LOG(4, (KERN_NOTICE "bridge-%s: packet dropped\n", bridge->name));
	 return 0;
      }
   }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 4)
   /*
    * Unbelievable... Caller sets h.raw = nh.raw before invoking us...
    */
   VNetBridgeComputeHeaderPos(skb);
#endif

   skb_push(skb, skb->data - compat_skb_mac_header(skb));
   LOG(3, (KERN_DEBUG "bridge-%s: receive %d\n",
	   bridge->name, (int) skb->len));

   /*
    * If this is a large packet, chop chop chop (if supported)...
    */
   if (VNetBridgeIsGSO(skb)) {
      VNetBridgeSendLargePacket(skb, bridge);
   } else {
      VNetSend(&bridge->port.jack, skb);
   }

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetBridgeProcRead/VNetBridgeProcShow --
 *
 *      Callback for read operation on this bridge entry in vnets proc fs.
 *
 * Results:
 *      Length of read operation.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
VNetBridgeProcShow(struct seq_file *seqf, // IN/OUT: buffer to write into
#else
VNetBridgeProcRead(char    *page,   // IN/OUT: buffer to write into
                   char   **start,  // OUT: 0 if file < 4k, else offset into page
                   off_t    off,    // IN: (unused) offset of read into the file
                   int      count,  // IN: (unused) maximum number of bytes to read
                   int     *eof,    // OUT: TRUE if there is nothing more to read
#endif
                   void    *data)   // IN: client data - pointer to bridge
{
   VNetBridge *bridge = (VNetBridge*)data;
   int len = 0;

   if (!bridge) {
      return len;
   }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
   VNetPrintPort(&bridge->port, seqf);

   seq_printf(seqf, "dev %s ", bridge->name);

   seq_printf(seqf, "\n");

   return 0;
#else
   len += VNetPrintPort(&bridge->port, page+len);

   len += sprintf(page+len, "dev %s ", bridge->name);

   len += sprintf(page+len, "\n");

   *start = 0;
   *eof   = 1;
   return len;
#endif
}
