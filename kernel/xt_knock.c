/*
	Project: Trigger
	Description: NetFilter Kernel Module to Support BPF Based Single Packet Authentication
	Auther: Bradley Landherr
*/

#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/errno.h>   // https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/errno-base.h for relevent error codes
#include <linux/byteorder/generic.h>
#include <linux/rculist.h>
#include <linux/timer.h>

// Netfilter headers
#include <linux/netfilter.h>
#include <linux/netfilter/x_tables.h>
#include <linux/netfilter/nf_conntrack_common.h>
#include "xt_knock.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bradley Landherr https://github.com/landhb");
MODULE_DESCRIPTION("NetFilter Kernel Module to Support BPF Based Port Knocking");
MODULE_VERSION("0.1");
MODULE_ALIAS("trigger");
MODULE_ALIAS("ip_conntrack_knock");


DEFINE_SPINLOCK(listmutex);

#define MODULE_NAME "knock"
#define MAX_PORTS 10
#define DEFAULT_PORT 1234

// Companion thread
struct task_struct * raw_thread;

// Globally accessed structs
char * src;
conntrack_state * knock_state;
struct timer_list * reaper;

static unsigned	int pkt_hook(void * priv, struct sk_buff * skb, const struct nf_hook_state * state) {

	unsigned int ret = NF_ACCEPT;
	struct iphdr * ip_header = (struct iphdr *)skb_network_header(skb);
	struct tcphdr * tcp_header = (struct tcphdr *)skb_transport_header(skb);


	// We only want to look at NEW connections
	if(skb->nfctinfo == IP_CT_ESTABLISHED && skb->nfctinfo == IP_CT_ESTABLISHED_REPLY) {
		return NF_ACCEPT;
	}

	// Check if packet is destined for our port watchlist
	if(tcp_header->dest == htons(DEFAULT_PORT)) {

			inet_ntoa(src, ip_header->saddr);

			if(!src) {
					return NF_DROP;
			}

			if(state_lookup(knock_state, 4, ip_header->saddr, NULL,  tcp_header->dest)) {
				printk(KERN_INFO	"[!] Hook accepted      source:%s\n", src);
				return NF_ACCEPT;
			}

			return NF_DROP;
	}

	return	ret;	
}



static struct nf_hook_ops pkt_hook_ops __read_mostly	= {
	.pf 		= NFPROTO_IPV4,
	.priority	= 1,
	.hooknum	= NF_INET_LOCAL_IN,
	.hook		= &pkt_hook,
};


// Callback function for the reaper: removes expired connections
void reap_expired_connections(unsigned long timeout) {

	conntrack_state	 * state, *tmp;

	spin_lock(&listmutex);

	list_for_each_entry_safe(state, tmp, &(knock_state->list), list) {

		if(jiffies - state->time_added >= msecs_to_jiffies(timeout)) {
			printk(KERN_INFO "[!] Knock expired\n");
			list_del_rcu(&(state->list));
			spin_unlock(&listmutex);
			//synchronize_rcu();
			kfree(state);
			spin_lock(&listmutex);
			continue;
		}
	}

	spin_unlock(&listmutex);

	// Set the timeout value
	mod_timer(reaper, jiffies + msecs_to_jiffies(timeout));

	return;
} 


// Init function to register target
static int __init nf_conntrack_knock_init(void) {

	int ret;
	raw_thread = NULL;
	 reaper = NULL;

	// Initialize our memory
	src = kmalloc(16 * sizeof(char), GFP_KERNEL);
	knock_state = init_state(); 
	//state_sync_init();

	// Start kernel thread raw socket to listen for triggers
	raw_thread = kthread_create(&listen, NULL, MODULE_NAME);

	// Increments usage counter - preserve structure even on exit
	get_task_struct(raw_thread);

	if(IS_ERR(raw_thread)) {
		printk(KERN_INFO "[-] Unable to start child thread\n");
		return PTR_ERR(raw_thread);
	}


	// Now it is safe to start kthread - exiting from it doesn't destroy its struct.
	wake_up_process(raw_thread);


	printk(KERN_INFO "[+] Started child thread\n");



	ret = nf_register_hook(&pkt_hook_ops);

	if(ret) {
		printk(KERN_INFO "[-] Failed to register hook\n");
		return ret;
	} 


	reaper = init_reaper(30000);

	if(!reaper) {
		printk(KERN_INFO "[-] Failed to initialize connection reaper\n");
		return -1;
	}
		

	printk(KERN_INFO "[+] Loaded Knock Netfilter module into kernel\n");
	return 0;
	
}


// Exit function to unregister target
static void __exit nf_conntrack_knock_exit(void) {

	int err = 0;

	if(raw_thread) {
		//lock_kernel();
		err = kthread_stop(raw_thread);
		put_task_struct(raw_thread);
		raw_thread = NULL;
		printk(KERN_INFO "[*] Stopped counterpart thread\n");
		//unlock_kernel();
	} else {
		printk(KERN_INFO "[!] no kernel thread to kill\n");
	}

	if(reaper) {
		cleanup_reaper(reaper);
	}

	nf_unregister_hook(&pkt_hook_ops);
	printk(KERN_INFO "[*] Unloaded Knock Netfilter module from kernel\n");
	return;
}


// Register the initialization and exit functions
module_init(nf_conntrack_knock_init);
module_exit(nf_conntrack_knock_exit);