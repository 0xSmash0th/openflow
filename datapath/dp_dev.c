#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/rcupdate.h>

#include "datapath.h"
#include "forward.h"

struct dp_dev {
	struct net_device_stats stats;
	struct datapath *dp;
};

static struct dp_dev *dp_dev_priv(struct net_device *netdev) 
{
	return netdev_priv(netdev);
}

static struct net_device_stats *dp_dev_get_stats(struct net_device *netdev)
{
	struct dp_dev *dp_dev = dp_dev_priv(netdev);
	return &dp_dev->stats;
}

int dp_dev_recv(struct net_device *netdev, struct sk_buff *skb) 
{
	int len = skb->len;
	struct dp_dev *dp_dev = dp_dev_priv(netdev);
	skb->pkt_type = PACKET_HOST;
	skb->protocol = eth_type_trans(skb, netdev);
	netif_rx(skb);
	netdev->last_rx = jiffies;
	dp_dev->stats.rx_packets++;
	dp_dev->stats.rx_bytes += len;
	return len;
}

static int dp_dev_mac_addr(struct net_device *dev, void *p)
{
	struct sockaddr *addr = p;

	if (netif_running(dev))
		return -EBUSY;
	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;
	memcpy(dev->dev_addr, addr->sa_data, dev->addr_len);
	return 0;
}

static int dp_dev_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct dp_dev *dp_dev = dp_dev_priv(netdev);
	struct datapath *dp;
	rcu_read_lock();
	dp = dp_dev->dp;
	if (likely(dp != NULL)) {
		dp_dev->stats.tx_packets++;
		dp_dev->stats.tx_bytes += skb->len;
		skb_reset_mac_header(skb);
		fwd_port_input(dp->chain, skb, OFPP_LOCAL);
	} else {
		dp_dev->stats.tx_dropped++;
		kfree_skb(skb);
	}
	rcu_read_unlock();
	return 0;
}

static int dp_dev_open(struct net_device *netdev)
{
	netif_start_queue(netdev);
	return 0;
}

static int dp_dev_stop(struct net_device *netdev)
{
	netif_stop_queue(netdev);
	return 0;
}

static void
do_setup(struct net_device *netdev)
{
	ether_setup(netdev);

	netdev->get_stats = dp_dev_get_stats;
	netdev->hard_start_xmit = dp_dev_xmit;
	netdev->open = dp_dev_open;
	netdev->stop = dp_dev_stop;
	netdev->tx_queue_len = 0;
	netdev->set_mac_address = dp_dev_mac_addr;

	netdev->flags = IFF_BROADCAST | IFF_MULTICAST;

	random_ether_addr(netdev->dev_addr);
}


int dp_dev_setup(struct datapath *dp)
{
	struct dp_dev *dp_dev;
	struct net_device *netdev;
	char of_name[8];
	int err;

	snprintf(of_name, sizeof of_name, "of%d", dp->dp_idx);
	netdev = alloc_netdev(sizeof(struct dp_dev), of_name, do_setup);
	if (!netdev)
		return -ENOMEM;

	err = register_netdev(netdev);
	if (err) {
		free_netdev(netdev);
		return err;
	}

	dp_dev = dp_dev_priv(netdev);
	dp_dev->dp = dp;
	dp->netdev = netdev;
	return 0;
}

void dp_dev_destroy(struct datapath *dp)
{
	struct dp_dev *dp_dev = dp_dev_priv(dp->netdev);
	dp_dev->dp = NULL;
	synchronize_net();
	unregister_netdev(dp->netdev);
}

int is_dp_dev(struct net_device *netdev) 
{
	return netdev->open == dp_dev_open;
}
