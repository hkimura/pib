/*
 * pib_main.c - Initialize & finalize routines
 *
 * Copyright (c) 2013-2015 Minoru NAKAMURA <nminoru@nminoru.jp>
 *
 * This code is licenced under the GPL version 2 or BSD license.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/rtnetlink.h>
#include <linux/inet.h>
#include <rdma/ib_user_verbs.h>

#include "pib.h"
#include "pib_trace.h"


MODULE_AUTHOR("Minoru NAKAMURA");
MODULE_DESCRIPTION(PIB_DRIVER_DESCRIPTION);
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(PIB_DRIVER_VERSION);


struct kmem_cache *pib_ah_cachep;
struct kmem_cache *pib_mr_cachep;
struct kmem_cache *pib_qp_cachep;
struct kmem_cache *pib_cq_cachep;
struct kmem_cache *pib_srq_cachep;
struct kmem_cache *pib_send_wqe_cachep;
struct kmem_cache *pib_recv_wqe_cachep;
struct kmem_cache *pib_ack_cachep;
struct kmem_cache *pib_cqe_cachep;
struct kmem_cache *pib_mcast_link_cachep;


bool pib_multi_host_mode;
struct sockaddr *pib_netd_sockaddr;
int pib_netd_socklen;
u64 pib_hca_guid_base;
struct pib_dev *pib_devs[PIB_MAX_HCA];
struct pib_easy_sw  pib_easy_sw;
struct sockaddr **pib_lid_table;


int pib_debug_level;
module_param_named(debug_level, pib_debug_level, int, 0644);
MODULE_PARM_DESC(debug_level, "Enable debug tracing if > 0");

unsigned int pib_num_hca = 2;
module_param_named(num_hca, pib_num_hca, uint, S_IRUGO);
MODULE_PARM_DESC(num_hca, "Number of pib HCAs");

unsigned int pib_phys_port_cnt = 2;
module_param_named(phys_port_cnt, pib_phys_port_cnt, uint, S_IRUGO);
MODULE_PARM_DESC(phys_port_cnt, "Number of physical ports");

unsigned int pib_behavior;
module_param_named(behavior, pib_behavior, uint, 0644);
MODULE_PARM_DESC(behavior, "Bitmap of the `behavior' capabilities");

unsigned int pib_manner_warn;
module_param_named(manner_warn, pib_manner_warn, uint, 0644);
MODULE_PARM_DESC(manner_warn, "Bitmap of the `manner' capabilities to report as warnings");

unsigned int pib_manner_err;
module_param_named(manner_err, pib_manner_warn, uint, 0644);
MODULE_PARM_DESC(manner_err, "Bitmap of the warning `manner' capabilities to report as errors");

static char *server_addr;
module_param_named(addr, server_addr, charp, S_IRUGO);
MODULE_PARM_DESC(addr, "pibnetd's IP address");

static u16 server_port = PIB_NETD_DEFAULT_PORT;
module_param_named(port, server_port, ushort, S_IRUGO);
MODULE_PARM_DESC(port, "pibnetd's port number");

static struct class *dummy_parent_class; /* /sys/class/pib */
static struct device *dummy_parent_device;
static u64 dummy_parent_device_dma_mask = DMA_BIT_MASK(32);

static int query_device(struct ib_device *ibdev,
			struct ib_device_attr *props,
			struct ib_udata *udata)
{
	struct pib_dev *dev;
	unsigned long flags;

	if (!ibdev)
		return -EINVAL;

	dev = to_pdev(ibdev);

	pib_trace_api(dev, IB_USER_VERBS_CMD_QUERY_DEVICE, 0);
	
	spin_lock_irqsave(&dev->lock, flags);
	*props = dev->ib_dev_attr;
	spin_unlock_irqrestore(&dev->lock, flags);
	
	return 0;
}

#ifdef PIB_CQ_FLAGS_TIMESTAMP_COMPLETION_SUPPORT
static int pib_query_device(struct ib_device *ibdev,
			    struct ib_device_attr *props,
			    struct ib_udata *udata)
{
	return query_device(ibdev, props, udata);
}
#else 
static int pib_query_device(struct ib_device *ibdev,
			    struct ib_device_attr *props)
{
	return query_device(ibdev, props, NULL);
}
#endif

static int query_port(struct pib_dev *dev, u8 port_num,
		      struct ib_port_attr *props)
{
	unsigned long flags;

	if (port_num < 1 || dev->ib_dev.phys_port_cnt < port_num)
		return -EINVAL;

	spin_lock_irqsave(&dev->lock, flags);
	*props = dev->ports[port_num - 1].ib_port_attr;
	spin_unlock_irqrestore(&dev->lock, flags);
	
	return 0;	
}

static int pib_query_port(struct ib_device *ibdev, u8 port_num,
			  struct ib_port_attr *props)
{
	struct pib_dev *dev;

	if (!ibdev)
		return -EINVAL;

	dev = to_pdev(ibdev);

	pib_trace_api(dev, IB_USER_VERBS_CMD_QUERY_PORT, port_num);

	return query_port(dev, port_num, props);
}

#ifdef PIB_GET_PORT_IMMUTABLE_SUPPORT
static int pib_get_port_immutable(struct ib_device *ibdev, u8 port_num,
				  struct ib_port_immutable *immutable)
{
	struct pib_dev *dev;
	struct ib_port_attr attr;
	int err;

	if (!ibdev)
		return -EINVAL;

	dev = to_pdev(ibdev);

	err = query_port(dev, port_num, &attr);
	if (err)
		return err;

	immutable->pkey_tbl_len	= attr.pkey_tbl_len;
	immutable->gid_tbl_len= attr.gid_tbl_len;
	immutable->core_cap_flags = RDMA_CORE_PORT_IBA_IB;
	immutable->max_mad_size = IB_MGMT_MAD_SIZE;

	return 0;
}
#endif

static void setup_obj_num_bitmap(struct pib_dev *dev);
static int init_port(struct pib_dev *dev, u8 port_num);


static enum rdma_link_layer
pib_get_link_layer(struct ib_device *device, u8 port_num)
{
	return IB_LINK_LAYER_INFINIBAND;
}


static int pib_query_gid(struct ib_device *ibdev, u8 port_num, int index,
			 union ib_gid *gid)
{
	struct pib_dev *dev;
	unsigned long flags;

	if (!ibdev)
		return -EINVAL;

	dev = to_pdev(ibdev);

	if (port_num < 1 || ibdev->phys_port_cnt < port_num)
		return -EINVAL;

	if (index < 0 || PIB_GID_PER_PORT < index)
		return -EINVAL;
	
	if (!gid)
		return -ENOMEM;

	spin_lock_irqsave(&dev->lock, flags);
	*gid = dev->ports[port_num - 1].gid[index];
	spin_unlock_irqrestore(&dev->lock, flags);

	return 0;
}


void pib_fill_grh(struct pib_dev *dev, u8 port_num, struct ib_grh *dest, const struct ib_global_route *src)
{
	BUG_ON((port_num        < 1) || (dev->ib_dev.phys_port_cnt < port_num));
	BUG_ON((src->sgid_index < 0) || (PIB_GID_PER_PORT < src->sgid_index));

	dest->version_tclass_flow =
		cpu_to_be32((6 << 28)|(src->traffic_class << 20)|(src->flow_label & 0xFFFFF));
	dest->paylen	= cpu_to_be16(0); /* @todo */
	dest->next_hdr  = 0;
	dest->hop_limit = src->hop_limit;
	dest->sgid	= dev->ports[port_num - 1].gid[src->sgid_index];
	dest->dgid	= src->dgid;
}


static int pib_query_pkey(struct ib_device *ibdev, u8 port_num, u16 index, u16 *pkey)
{
	struct pib_dev *dev;
	unsigned long flags;

	if (!ibdev)
		return -EINVAL;

	dev = to_pdev(ibdev);

	spin_lock_irqsave(&dev->lock, flags);
	if (index < PIB_PKEY_TABLE_LEN)
		*pkey = be16_to_cpu(dev->ports[port_num - 1].pkey_table[index]);
	else
		*pkey = 0;
	spin_unlock_irqrestore(&dev->lock, flags);

	return 0;
}


static int pib_modify_device(struct ib_device *ibdev, int mask,
			     struct ib_device_modify *props)
{
	struct pib_dev *dev;
	unsigned long flags;

	pib_debug("pib: pib_modify_device: mask=%x\n", mask);

	if (!ibdev)
		return -EINVAL;

	dev = to_pdev(ibdev);

	pib_trace_api(dev, PIB_USER_VERBS_CMD_MODIFY_DEVICE, 0);

	if (mask & ~(IB_DEVICE_MODIFY_SYS_IMAGE_GUID|IB_DEVICE_MODIFY_NODE_DESC))
		return -EOPNOTSUPP;

	spin_lock_irqsave(&dev->lock, flags);
	if (mask & IB_DEVICE_MODIFY_NODE_DESC)
		/* @todo ポート毎の処理 (c.f. qib_node_desc_chg) */
		memcpy(dev->ib_dev.node_desc, props->node_desc, sizeof(props->node_desc));

	if (mask & IB_DEVICE_MODIFY_SYS_IMAGE_GUID)
		/* @todo ポート毎の処理 (c.f. qib_sys_guid_chg) */
		dev->ib_dev_attr.sys_image_guid = props->sys_image_guid;
	spin_unlock_irqrestore(&dev->lock, flags);

	return 0;
}


static int pib_modify_port(struct ib_device *ibdev, u8 port_num, int mask,
			      struct ib_port_modify *props)
{
	struct pib_dev *dev;
	unsigned long flags;

	pib_debug("pib: pib_modify_port: port=%u, mask=%x,%x,%x\n",
		  port_num, mask, props->set_port_cap_mask, props->clr_port_cap_mask);

	if (!ibdev)
		return -EINVAL;

	dev = to_pdev(ibdev);

	pib_trace_api(dev, PIB_USER_VERBS_CMD_MODIFY_PORT, port_num);

	if (mask & ~(IB_PORT_SHUTDOWN|IB_PORT_INIT_TYPE|IB_PORT_RESET_QKEY_CNTR))
		return -EOPNOTSUPP;

	spin_lock_irqsave(&dev->lock, flags);
	if (mask & IB_PORT_INIT_TYPE)
		pr_err("pib: pib_modify_port: init type\n");

	if (mask & IB_PORT_SHUTDOWN)
		pr_err("pib: pib_modify_port: port shutdown\n");

	if (mask & IB_PORT_RESET_QKEY_CNTR)
		pr_err("pib: pib_modify_port: port reset qkey control\n");

	dev->ports[port_num - 1].ib_port_attr.port_cap_flags |= props->set_port_cap_mask;
	dev->ports[port_num - 1].ib_port_attr.port_cap_flags &= ~props->clr_port_cap_mask;

	/* @todo port_cap_flags 変化を伝達 */
	spin_unlock_irqrestore(&dev->lock, flags);

	return 0;
}

static int pib_mmap(struct ib_ucontext *context, struct vm_area_struct *vma)
{
	pib_debug("pib: pib_mmap\n");

	return -EINVAL;
}


static ssize_t show_local_ca_ack_delay(struct device *device, struct device_attribute *attr,
				       char *buf)
{
	struct pib_dev *dev =
		container_of(device, struct pib_dev, ib_dev.dev);

	return sprintf(buf, "%u\n", dev->ib_dev_attr.local_ca_ack_delay);
}


static ssize_t store_local_ca_ack_delay(struct device *device, struct device_attribute *attr,
					const char *buf, size_t count)
{
	unsigned int local_ca_ack_delay;
	ssize_t result;	
	struct pib_dev *dev =
		container_of(device, struct pib_dev, ib_dev.dev);

	result = sscanf(buf, "%u", &local_ca_ack_delay);
	if (result != 1)
		return -EINVAL;

	if ((local_ca_ack_delay < 1) || (31 < local_ca_ack_delay))
		return -EINVAL;

	if (local_ca_ack_delay < pib_get_local_ca_ack_delay())
		local_ca_ack_delay = pib_get_local_ca_ack_delay();

	dev->ib_dev_attr.local_ca_ack_delay = (u8)local_ca_ack_delay;

	return count;
}


static ssize_t show_local_ack_timeout(struct device *device, struct device_attribute *attr,
				      char *buf)
{
	struct pib_dev *dev =
		container_of(device, struct pib_dev, ib_dev.dev);

	return sprintf(buf, "%u\n", dev->perf.local_ack_timeout);
}


#ifdef PIB_HACK_IMM_DATA_LKEY
static ssize_t show_imm_data_lkey(struct device *device, struct device_attribute *attr,
			     char *buf)
{
	struct pib_dev *dev =
		container_of(device, struct pib_dev, ib_dev.dev);

	return sprintf(buf, "0x%08x\n", dev->imm_data_lkey);
}
#endif


static DEVICE_ATTR(local_ca_ack_delay,	S_IRUGO|S_IWUSR, show_local_ca_ack_delay, store_local_ca_ack_delay);
static DEVICE_ATTR(local_ack_timeout,	S_IRUGO,         show_local_ack_timeout,  NULL);

#ifdef PIB_HACK_IMM_DATA_LKEY
static DEVICE_ATTR(imm_data_lkey, S_IRUGO, show_imm_data_lkey, NULL);
#endif


static struct device_attribute *pib_class_attributes[] = {
	&dev_attr_local_ca_ack_delay,
	&dev_attr_local_ack_timeout,
#ifdef PIB_HACK_IMM_DATA_LKEY
	&dev_attr_imm_data_lkey,
#endif
};


static struct pib_dev *pib_dev_add(struct device *dma_device, int dev_id)
{
	int i, j;
	struct pib_dev *dev;
	struct ib_device_attr ib_dev_attr = {
		.fw_ver              = PIB_DRIVER_FW_VERSION,
		.sys_image_guid      = cpu_to_be64(pib_hca_guid_base | 0x0200ULL),
		.max_mr_size         = 0xffffffffffffffffULL,
		.page_size_cap       = 0xfffffe00UL, /* @todo */
		.vendor_id           = 1U,
		.vendor_part_id      = 1U,
		.hw_ver              = 0U,
		.max_qp              = PIB_MAX_QP,
		.max_qp_wr           = 16351,
		.device_cap_flags    = PIB_DEVICE_CAP_FLAGS,

		.max_sge             = PIB_MAX_SGE,
		.max_sge_rd          =       0,
		.max_cq              = PIB_MAX_CQ - 1, /* CQ0 is invalid */
		.max_cqe             = 4194303,
		.max_mr              = PIB_MAX_MR - 1, /* MR0 is invalid */
		.max_pd              = PIB_MAX_PD - 1, /* PD0 is invalid */
		.max_qp_rd_atom      = PIB_MAX_RD_ATOM,
		.max_ee_rd_atom      =       0,
		.max_res_rd_atom     = 2096128,
		.max_qp_init_rd_atom =     128,
		.max_ee_init_rd_atom =       0,
		.atomic_cap          = IB_ATOMIC_GLOB,
		.masked_atomic_cap   = IB_ATOMIC_GLOB,
		.max_ee              =       0,
		.max_rdd             =       0,
		.max_mw              =       0,
		.max_raw_ipv6_qp     =       0,
		.max_raw_ethy_qp     =       0,
		.max_mcast_grp       =    8192,
		.max_mcast_qp_attach = PIB_MCAST_QP_ATTACH,
		.max_total_mcast_qp_attach = 2031616,
		.max_ah              = PIB_MAX_AH - 1, /* AH0 is invalid */
		.max_fmr             =       0, 
		.max_map_per_fmr     =       0,
		.max_srq             = PIB_MAX_SRQ - 1, /* SRQ0 is invalid */
		.max_srq_wr          =   16383,
		.max_srq_sge         = PIB_MAX_SGE -1, /* For Mellanox HCA simulation, max_srq_sge is set to max_sge - 1. */
		.max_fast_reg_page_list_len = 0,
		.max_pkeys           =     125,
		.local_ca_ack_delay  = pib_get_local_ca_ack_delay(),
	};

	dev = (struct pib_dev *)ib_alloc_device(sizeof *dev);
	if (!dev) {
		pr_err("pib: Device struct alloc failed\n");
		return NULL;
	}

	dev->dev_id			= dev_id;

	strlcpy(dev->ib_dev.name, "pib_%d", IB_DEVICE_NAME_MAX);

	dev->ib_dev.owner		= THIS_MODULE;
	dev->ib_dev.node_type		= RDMA_NODE_IB_CA;
	dev->ib_dev.node_guid		= cpu_to_be64(pib_hca_guid_base | ((3 + dev_id) << 8) | 0);
	dev->ib_dev.local_dma_lkey	= PIB_LOCAL_DMA_LKEY;
	dev->ib_dev.phys_port_cnt	= pib_phys_port_cnt;
	dev->ib_dev.num_comp_vectors	= num_possible_cpus();
	dev->ib_dev.uverbs_abi_ver	= PIB_UVERBS_ABI_VERSION;

	memcpy(dev->ib_dev.node_desc,
	       PIB_DRIVER_DESCRIPTION,
	       sizeof(PIB_DRIVER_DESCRIPTION));

	dev->ib_dev.uverbs_cmd_mask	=
		(1ULL << IB_USER_VERBS_CMD_GET_CONTEXT)		|
		(1ULL << IB_USER_VERBS_CMD_QUERY_DEVICE)	|
		(1ULL << IB_USER_VERBS_CMD_QUERY_PORT)		|
		(1ULL << IB_USER_VERBS_CMD_ALLOC_PD)		|
		(1ULL << IB_USER_VERBS_CMD_DEALLOC_PD)		|
		(1ULL << IB_USER_VERBS_CMD_CREATE_AH)           |
		(1ULL << IB_USER_VERBS_CMD_MODIFY_AH)           |
		(1ULL << IB_USER_VERBS_CMD_QUERY_AH)            |
		(1ULL << IB_USER_VERBS_CMD_DESTROY_AH)          |		
		(1ULL << IB_USER_VERBS_CMD_REG_MR)		|
		/* (1ULL << IB_USER_VERBS_CMD_REG_SMR)	           | */
		/* (1ULL << IB_USER_VERBS_CMD_REREG_MR)	           | */
		/* (1ULL << IB_USER_VERBS_CMD_QUERY_MR)	           | */
		(1ULL << IB_USER_VERBS_CMD_DEREG_MR)		|
		(1ULL << IB_USER_VERBS_CMD_CREATE_COMP_CHANNEL)	|
		(1ULL << IB_USER_VERBS_CMD_CREATE_CQ)		|
		(1ULL << IB_USER_VERBS_CMD_RESIZE_CQ)		|
		(1ULL << IB_USER_VERBS_CMD_DESTROY_CQ)		|
		(1ULL << IB_USER_VERBS_CMD_POLL_CQ)             |
		(1ULL << IB_USER_VERBS_CMD_REQ_NOTIFY_CQ)       |
		(1ULL << IB_USER_VERBS_CMD_CREATE_QP)		|
		(1ULL << IB_USER_VERBS_CMD_QUERY_QP)		|
		(1ULL << IB_USER_VERBS_CMD_MODIFY_QP)		|
		(1ULL << IB_USER_VERBS_CMD_DESTROY_QP)		|
		(1ULL << IB_USER_VERBS_CMD_POST_SEND)		|
		(1ULL << IB_USER_VERBS_CMD_POST_RECV)		|
		(1ULL << IB_USER_VERBS_CMD_ATTACH_MCAST)        |
		(1ULL << IB_USER_VERBS_CMD_DETACH_MCAST)        |
		(1ULL << IB_USER_VERBS_CMD_CREATE_SRQ)		|
		(1ULL << IB_USER_VERBS_CMD_MODIFY_SRQ)		|
		(1ULL << IB_USER_VERBS_CMD_QUERY_SRQ)		|
		(1ULL << IB_USER_VERBS_CMD_DESTROY_SRQ)		|
		(1ULL << IB_USER_VERBS_CMD_POST_SRQ_RECV);

	dev->ib_dev.query_device	= pib_query_device;
	dev->ib_dev.query_port		= pib_query_port;
	dev->ib_dev.get_link_layer	= pib_get_link_layer;
	dev->ib_dev.query_gid		= pib_query_gid;
	dev->ib_dev.query_pkey		= pib_query_pkey;
	dev->ib_dev.modify_device	= pib_modify_device;
	dev->ib_dev.modify_port		= pib_modify_port;
	dev->ib_dev.alloc_ucontext	= pib_alloc_ucontext;
	dev->ib_dev.dealloc_ucontext	= pib_dealloc_ucontext;
	dev->ib_dev.mmap		= pib_mmap;
	dev->ib_dev.alloc_pd		= pib_alloc_pd;
	dev->ib_dev.dealloc_pd		= pib_dealloc_pd;
	dev->ib_dev.create_ah		= pib_create_ah;
	dev->ib_dev.modify_ah		= pib_modify_ah;
	dev->ib_dev.query_ah		= pib_query_ah;
	dev->ib_dev.destroy_ah		= pib_destroy_ah;
	dev->ib_dev.create_srq		= pib_create_srq;
	dev->ib_dev.modify_srq		= pib_modify_srq;
	dev->ib_dev.query_srq		= pib_query_srq;
	dev->ib_dev.destroy_srq		= pib_destroy_srq;
	dev->ib_dev.post_srq_recv	= pib_post_srq_recv;
	dev->ib_dev.create_qp		= pib_create_qp;
	dev->ib_dev.modify_qp		= pib_modify_qp;
	dev->ib_dev.query_qp		= pib_query_qp;
	dev->ib_dev.destroy_qp		= pib_destroy_qp;
	dev->ib_dev.post_send		= pib_post_send;
	dev->ib_dev.post_recv		= pib_post_recv;
	dev->ib_dev.create_cq		= pib_create_cq;
	dev->ib_dev.modify_cq		= pib_modify_cq;
	dev->ib_dev.resize_cq		= pib_resize_cq;
	dev->ib_dev.destroy_cq		= pib_destroy_cq;
	dev->ib_dev.poll_cq		= pib_poll_cq;
	dev->ib_dev.req_notify_cq	= pib_req_notify_cq;
	dev->ib_dev.get_dma_mr		= pib_get_dma_mr;
	dev->ib_dev.reg_user_mr		= pib_reg_user_mr;
	dev->ib_dev.dereg_mr		= pib_dereg_mr;
	/* dev->ib_dev.destroy_mr */
	/* dev->ib_dev.create_mr */
	dev->ib_dev.alloc_fast_reg_mr 	= pib_alloc_fast_reg_mr;
	dev->ib_dev.alloc_fast_reg_page_list = pib_alloc_fast_reg_page_list;
	dev->ib_dev.free_fast_reg_page_list  = pib_free_fast_reg_page_list;
	dev->ib_dev.attach_mcast	= pib_attach_mcast;
	dev->ib_dev.detach_mcast	= pib_detach_mcast;
	dev->ib_dev.process_mad		= pib_process_mad;
#ifdef PIB_GET_PORT_IMMUTABLE_SUPPORT 
	dev->ib_dev.get_port_immutable	= pib_get_port_immutable;
#endif

	dev->ib_dev.dma_ops		= &pib_dma_mapping_ops;

	spin_lock_init(&dev->lock);

	INIT_LIST_HEAD(&dev->ucontext_head);
	INIT_LIST_HEAD(&dev->pd_head);
	INIT_LIST_HEAD(&dev->mr_head);
	INIT_LIST_HEAD(&dev->srq_head);
	INIT_LIST_HEAD(&dev->ah_head);
	INIT_LIST_HEAD(&dev->cq_head);
	INIT_LIST_HEAD(&dev->qp_head);

	dev->last_qp_num		= pib_random() & PIB_QPN_MASK;
	dev->qp_table			= RB_ROOT;

	spin_lock_init(&dev->qp_sched.lock);
	dev->qp_sched.wakeup_time	= jiffies;
	dev->qp_sched.rb_root		= RB_ROOT;

	spin_lock_init(&dev->wq_sched.lock);
	INIT_LIST_HEAD(&dev->wq_sched.head);
	INIT_LIST_HEAD(&dev->wq_sched.timer_head);
	PIB_INIT_WORK(&dev->debugfs.inject_err_work, dev, NULL, pib_inject_err_handler);
	spin_lock_init(&dev->debugfs.trace_lock);

	dev->ib_dev_attr		= ib_dev_attr;

	dev->mcast_table		= vzalloc(sizeof(struct list_head) * (PIB_MAX_LID - PIB_MCAST_LID_BASE));
	if (!dev->mcast_table)
		goto err_mcast_table;

	for (i=0 ; i<PIB_MAX_LID - PIB_MCAST_LID_BASE ; i++)
		INIT_LIST_HEAD(&dev->mcast_table[i]);

	dev->ports	= vzalloc(sizeof(struct sockaddr*) * dev->ib_dev.phys_port_cnt);
	if (!dev->ports)
		goto err_ports;

	dev->obj_num_bitmap	= vzalloc(PIB_MAX_OBJS / BITS_PER_BYTE);
	if (!dev->obj_num_bitmap)
		goto err_obj_num_bitmap;

	setup_obj_num_bitmap(dev);

	for (i=0 ; i < dev->ib_dev.phys_port_cnt ; i++)
		if (init_port(dev, i + 1))
			goto err_init_port;

#ifdef PIB_HACK_IMM_DATA_LKEY
	dev->imm_data_lkey	= PIB_IMM_DATA_LKEY;
#endif

	dev->ib_dev.dma_device = dma_device;

	if (ib_register_device(&dev->ib_dev, NULL))
		goto err_register_ibdev;

	if (pib_create_kthread(dev))
		goto err_create_kthread;

	for (i = 0; i < ARRAY_SIZE(pib_class_attributes); i++)
		if (device_create_file(&dev->ib_dev.dev, pib_class_attributes[i]))
			goto err_create_file;

	pr_info("pib: add HCA (dev_id=%d, ports=%u)\n",
		dev->dev_id, dev->ib_dev.phys_port_cnt);

	return dev;

err_create_file:
	for (j = i - 1; j >= 0 ; j--)
		device_remove_file(&dev->ib_dev.dev, pib_class_attributes[j]);

	pib_release_kthread(dev);

err_create_kthread:
	ib_unregister_device(&dev->ib_dev);	

err_register_ibdev:

err_init_port:

	vfree(dev->obj_num_bitmap);
err_obj_num_bitmap:

	vfree(dev->ports);
err_ports:

	vfree(dev->mcast_table);
err_mcast_table:

	ib_dealloc_device(&dev->ib_dev);

	return NULL;
}


static void setup_obj_num_bitmap(struct pib_dev *dev)
{
	unsigned long *bitmap = dev->obj_num_bitmap;

	bitmap_set(bitmap, PIB_BITMAP_CONTEXT_START, 1);
	bitmap_set(bitmap, PIB_BITMAP_PD_START,      1);
	bitmap_set(bitmap, PIB_BITMAP_SRQ_START,     1);
	bitmap_set(bitmap, PIB_BITMAP_CQ_START,      1);
	bitmap_set(bitmap, PIB_BITMAP_MR_START,      1);
	bitmap_set(bitmap, PIB_BITMAP_AH_START,      1);

	bitmap_set(bitmap, PIB_BITMAP_QP_START + PIB_QP0,          1);
	bitmap_set(bitmap, PIB_BITMAP_QP_START + PIB_QP1,          1);
	bitmap_set(bitmap, PIB_BITMAP_QP_START + PIB_LINK_QP,      1);
	bitmap_set(bitmap, PIB_BITMAP_QP_START + IB_MULTICAST_QPN, 1);

}


static int init_port(struct pib_dev *dev, u8 port_num)
{
	int j;
	struct pib_port *port;
	struct ib_port_attr ib_port_attr = {
		.state           = IB_PORT_INIT,
		.max_mtu         = IB_MTU_4096,
		.active_mtu      = IB_MTU_256,
		.gid_tbl_len     = PIB_GID_PER_PORT,
		.port_cap_flags  = PIB_PORT_CAP_FLAGS,
		.max_msg_sz      = PIB_MAX_PAYLOAD_LEN,
		.bad_pkey_cntr   = 0U,
		.qkey_viol_cntr  = 0U,
		.pkey_tbl_len    = PIB_PKEY_TABLE_LEN,
		.lid             = 0U,
		.sm_lid          = 0U,
		.lmc             = 0U,
		.max_vl_num      = 4U,
		.sm_sl           = 0U,
		.subnet_timeout  = 0U,
		.init_type_reply = 0U,
		.active_width    = IB_WIDTH_12X,
		.active_speed    = IB_SPEED_QDR,
		.phys_state      = PIB_PHYS_PORT_LINK_UP,
	};

	port = &dev->ports[port_num - 1];

	port->port_num	   = port_num;
	port->ib_port_attr = ib_port_attr;

	/*
	 * @see IBA Spec. Vol.1 4.1.1
	 */
	port->gid[0].global.subnet_prefix =
		/* default GID prefix */
		cpu_to_be64(0xFE80000000000000ULL);
	port->gid[0].global.interface_id  =
		cpu_to_be64(pib_hca_guid_base | ((3 + dev->dev_id) << 8) | (port_num));

	port->link_width_enabled = PIB_LINK_WIDTH_SUPPORTED;
	port->link_speed_enabled = PIB_LINK_SPEED_SUPPORTED;

	for (j=0 ; j < PIB_PKEY_TABLE_LEN ; j++)
		port->pkey_table[j] = IB_DEFAULT_PKEY_FULL;

	if (pib_multi_host_mode) {
		port->is_connected = false;
		port->ib_port_attr.phys_state = PIB_PHYS_PORT_POLLING;
		port->ib_port_attr.state      = IB_PORT_DOWN;
	} else {
		port->is_connected = true;
		port->ib_port_attr.phys_state = PIB_PHYS_PORT_LINK_UP;
		port->ib_port_attr.state      = IB_PORT_INIT;
	}

	PIB_INIT_WORK(&port->link.work, dev, port, pib_netd_comm_handler);

	return 0;
}


u32 pib_alloc_obj_num(struct pib_dev *dev, u32 start, u32 size, u32 *last_num_p)
{
	u32 n;
	unsigned long *bitmap = dev->obj_num_bitmap + start / BITS_PER_LONG;
	
	BUG_ON(!spin_is_locked(&dev->lock));

	n = *last_num_p + 1;

	n = find_next_zero_bit(bitmap, size, n);
	if (n != size)
		goto found;

	n = find_next_zero_bit(bitmap, size, 1);
	if (n != size)
		goto found;

	return (u32)-1;

found:
	bitmap_set(bitmap, n, 1);

	*last_num_p = n;

	return n;
}


void pib_dealloc_obj_num(struct pib_dev *dev, u32 start, u32 index)
{
	unsigned long *bitmap = dev->obj_num_bitmap + start / BITS_PER_LONG;

	BUG_ON(!spin_is_locked(&dev->lock));

	bitmap_clear(bitmap, index, 1);
}


static void pib_dev_remove(struct pib_dev *dev)
{
#ifdef PIB_HACK_IPOIB_LEAK_AH
	unsigned long flags;
	struct pib_ah *ah, *next_ah;
#endif

	pr_info("pib: remove HCA (dev_id=%d)\n", dev->dev_id);

	ib_unregister_device(&dev->ib_dev);

	pib_stop_delayed_queue(dev);

	pib_release_kthread(dev);

	vfree(dev->ports);
	vfree(dev->obj_num_bitmap);
	vfree(dev->mcast_table);

#ifdef PIB_HACK_IPOIB_LEAK_AH
	/*
	 * HACK
	 * ib_ipoib.ko が IB ドライバの unregister 時に AH をリークさせている。
	 * 応急処置として AH は全て解放する。
	 */
	spin_lock_irqsave(&dev->lock, flags);
	list_for_each_entry_safe(ah, next_ah, &dev->ah_head, list) {
		list_del(&ah->list);
		dev->nr_ah--;
		kmem_cache_free(pib_ah_cachep, ah);
	}
	spin_unlock_irqrestore(&dev->lock, flags);
#endif

	ib_dealloc_device(&dev->ib_dev);
}


static int pib_kmem_cache_create(void)
{
	pib_ah_cachep = kmem_cache_create("pib_ah",
					  sizeof(struct pib_ah), 0,
					  0, NULL);

	if (!pib_ah_cachep)
		return -1;

	pib_mr_cachep = kmem_cache_create("pib_mr",
					  sizeof(struct pib_mr), 0,
					  0, NULL);

	if (!pib_mr_cachep)
		return -1;
	
	pib_qp_cachep = kmem_cache_create("pib_qp",
					  sizeof(struct pib_qp), 0,
					  0, NULL);
	
	if (!pib_qp_cachep)
		return -1;

	pib_cq_cachep = kmem_cache_create("pib_cq",
					  sizeof(struct pib_cq), 0,
					  0, NULL);
	
	if (!pib_cq_cachep)
		return -1;

	pib_srq_cachep = kmem_cache_create("pib_srq",
					   sizeof(struct pib_srq), 0,
					   0, NULL);

	if (!pib_srq_cachep)
		return -1;

	pib_send_wqe_cachep = kmem_cache_create("pib_send_wqe",
						sizeof(struct pib_send_wqe), 0,
						0, NULL);

	if (!pib_send_wqe_cachep)
		return -1;

	pib_recv_wqe_cachep = kmem_cache_create("pib_recv_wqe",
						sizeof(struct pib_recv_wqe) ,0,
						0, NULL);

	if (!pib_recv_wqe_cachep)
		return -1;

	pib_ack_cachep = kmem_cache_create("pib_ack",
					   sizeof(struct pib_ack) ,0,
					   0, NULL);
	
	if (!pib_ack_cachep)
		return -1;

	pib_cqe_cachep = kmem_cache_create("pib_cqe",
					   sizeof(struct pib_cqe), 0,
					   0, NULL);

	if (!pib_cqe_cachep)
		return -1;

	pib_mcast_link_cachep = kmem_cache_create("pib_mcast_link",
					   sizeof(struct pib_mcast_link), 0,
					   0, NULL);

	if (!pib_mcast_link_cachep)
		return -1;

	return 0;
}


static void pib_kmem_cache_destroy(void)
{
	if (pib_ah_cachep)
		kmem_cache_destroy(pib_ah_cachep);

	if (pib_mr_cachep)
		kmem_cache_destroy(pib_mr_cachep);

	if (pib_qp_cachep)
		kmem_cache_destroy(pib_qp_cachep);

	if (pib_cq_cachep)
		kmem_cache_destroy(pib_cq_cachep);

	if (pib_srq_cachep)
		kmem_cache_destroy(pib_srq_cachep);

	if (pib_send_wqe_cachep)
		kmem_cache_destroy(pib_send_wqe_cachep);

	if (pib_recv_wqe_cachep)
		kmem_cache_destroy(pib_recv_wqe_cachep);

	if (pib_ack_cachep)
		kmem_cache_destroy(pib_ack_cachep);

	if (pib_cqe_cachep)
		kmem_cache_destroy(pib_cqe_cachep);

	if (pib_mcast_link_cachep)
		kmem_cache_destroy(pib_mcast_link_cachep);

	pib_ah_cachep = NULL;
	pib_mr_cachep = NULL;
	pib_qp_cachep = NULL;
	pib_cq_cachep = NULL;
	pib_srq_cachep = NULL;
	pib_send_wqe_cachep = NULL;
	pib_recv_wqe_cachep = NULL;
	pib_ack_cachep = NULL;
	pib_cqe_cachep = NULL;
	pib_mcast_link_cachep = NULL;
}


/*
 *  pib's 64-bits GUID is derived from the 48-bits MAC address of the first
 *  effective Ethernet NIC on this host.
 */
static void get_hca_guid_base(void)
{
	int i;
	struct net_device *dev;

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		if (dev->flags & IFF_LOOPBACK)
			continue;

		if (!dev->dev_addr)
			continue;

		for (i=0 ; i<ETH_ALEN ; i++) {
			pib_hca_guid_base |= (u8)dev->dev_addr[i];
			pib_hca_guid_base <<= 8;
		}
		
		pib_hca_guid_base <<= (sizeof(pib_hca_guid_base) - ETH_ALEN - 1) * 8;
		break;
	}
	rtnl_unlock();

	if (pib_hca_guid_base == 0)
		pib_hca_guid_base = 0xCafeBabe0000ULL;
}


static int parse_multi_host_mode(void)
{
	size_t str_len, socklen;
	struct sockaddr* sockaddr;
	struct sockaddr_in *in4;
	struct sockaddr_in6 *in6;

	if (!server_addr)
		return 0;

	if ((server_port == 0) || (65536 <= server_port)) {
		pr_err("pib: the specified value of port is out of range\n");
		return -EINVAL;
	}

	sockaddr = kzalloc(sizeof(struct sockaddr_in6), GFP_KERNEL);
	if (!sockaddr) {
		pr_err("pib: out of memory\n");
		return -ENOMEM;
	}

	in4 = (struct sockaddr_in *)sockaddr;
	in6 = (struct sockaddr_in6 *)sockaddr;

	str_len = strlen(server_addr);

	if (in4_pton(server_addr, str_len, (u8 *)&in4->sin_addr.s_addr, -1, NULL)) {
		in4->sin_family  = AF_INET;
		in4->sin_port    = htons(server_port);
		socklen          = sizeof(*in4);
	} else if (in6_pton(server_addr, str_len, (u8 *)&in6->sin6_addr.s6_addr, -1, NULL)) {
		in6->sin6_family = AF_INET6;
		in6->sin6_port   = htons(server_port);
		socklen          = sizeof(*in6);
	} else {
		kfree(sockaddr);
		return 0;
	}

	/* Enable multi-host-mode */
	pib_multi_host_mode = true;

	pib_netd_sockaddr = (struct sockaddr *)sockaddr;
	pib_netd_socklen  = socklen;
	pr_info("pibnetd's IP address: %s\n", server_addr);

	return 0;
}


static int __init pib_init(void)
{
	int i, j, err = 0;
  unsigned long aaa;

	pr_info("pib: " PIB_DRIVER_DESCRIPTION " v" PIB_DRIVER_VERSION "\n");

	err = parse_multi_host_mode();
	if (err < 0)
		return err;

	if ((pib_num_hca < 1) || (PIB_MAX_HCA < pib_num_hca)) {
		pr_err("pib: pib_num_hca(%u) out of range [1, %u]\n", pib_num_hca, PIB_MAX_HCA);
		return -EINVAL;
	}

	if ((pib_phys_port_cnt < 1) || (PIB_MAX_PORTS < pib_phys_port_cnt)) {
		pr_err("pib: phys_port_cnt(%u) out of range [1, %u]\n", pib_phys_port_cnt, PIB_MAX_PORTS);
		return -EINVAL;
	}

	if (!pib_multi_host_mode && (pib_num_hca * pib_phys_port_cnt < 2)) {
		pr_err("pib: In single-host-mode, the value of num_hca * phys_port_cn must be 2 or more.\n");
		return -EINVAL;
	}

	/* @todo */

	dummy_parent_class = class_create(THIS_MODULE, "pib");
	if (IS_ERR(dummy_parent_class)) {
		err = PTR_ERR(dummy_parent_class);
		goto err_class_create;
	}

	dummy_parent_device = device_create(dummy_parent_class, NULL, MKDEV(0, 0), NULL, "pib_root");
	if (IS_ERR(dummy_parent_device)) {
		err = PTR_ERR(dummy_parent_device);
		goto err_device_create;
	}

	dummy_parent_device->dma_mask = &dummy_parent_device_dma_mask;

	get_hca_guid_base();

	if (pib_multi_host_mode)
		pr_info("pib: multi-host-mode\n");
	else
		pr_info("pib: single-host-mode\n");

  pr_info("yooooo\n");
  aaa = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;
  pr_info("memlock val %ld\n", aaa);
  if (capable(CAP_IPC_LOCK)) {
    pr_info("capable(CAP_IPC_LOCK) YES\n");
  } else {
    pr_info("capable(CAP_IPC_LOCK) NO\n");
  }

	if (!pib_multi_host_mode) {
		pib_lid_table = vzalloc(sizeof(struct sockaddr*) * PIB_MAX_LID);
		if (!pib_lid_table)
			goto err_alloc_lid_table;
	}

	if (pib_kmem_cache_create()) {
		pib_kmem_cache_destroy();
		err = -ENOMEM;
		goto err_kmem_cache_destroy;
	}
	
	if (!pib_multi_host_mode) {
		if (pib_create_switch(&pib_easy_sw))
			goto err_create_switch;
	}

	for (i=0 ; i<pib_num_hca ; i++) {
		pib_devs[i] = pib_dev_add(dummy_parent_device, i);
		if (!pib_devs[i]) {
			err = -1;
			goto err_ib_add;
		}
	}

	if (pib_register_debugfs())
		goto err_create_debugfs;

	return 0;

err_create_debugfs:
err_ib_add:
	for (j=i - 1 ; 0 <= j ; j--)
		if (pib_devs[j]) {
			pib_dev_remove(pib_devs[j]);
			pib_devs[j] = NULL;
		}

	pib_release_switch(&pib_easy_sw);
err_create_switch:

	if (pib_netd_sockaddr) {
		kfree(pib_netd_sockaddr);
		pib_netd_sockaddr = NULL;
	}

	pib_kmem_cache_destroy();
err_kmem_cache_destroy:

	if (pib_lid_table)
		vfree(pib_lid_table);
	pib_lid_table = NULL;

err_alloc_lid_table:

	device_unregister(dummy_parent_device);
	dummy_parent_device = NULL;
err_device_create:

	class_destroy(dummy_parent_class);
	dummy_parent_class = NULL;
err_class_create:

	return err;
}


static void __exit pib_cleanup(void)
{
	int i;

	pr_info("pib: unload\n");

	pib_unregister_debugfs();

	for (i = pib_num_hca - 1 ; 0 <= i ; i--)
		if (pib_devs[i]) {
			pib_dev_remove(pib_devs[i]);
			pib_devs[i] = NULL;
		}

	if (!pib_multi_host_mode)
		pib_release_switch(&pib_easy_sw);

	if (pib_netd_sockaddr)
		kfree(pib_netd_sockaddr);

	pib_kmem_cache_destroy();

	if (pib_lid_table) {
		vfree(pib_lid_table);
		pib_lid_table = NULL;
	}

	if (dummy_parent_device) {
		device_unregister(dummy_parent_device);
		dummy_parent_device = NULL;
	}

	if (dummy_parent_class) {
		class_destroy(dummy_parent_class);
		dummy_parent_class = NULL;
	}
}


module_init(pib_init);
module_exit(pib_cleanup);
