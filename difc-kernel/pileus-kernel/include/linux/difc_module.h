/*
* Author: Yuqiong Sun <yus138@cse.psu.edu>
*/


#ifdef CONFIG_SECURITY_DIFC
enum label_types {OWNERSHIP_ADD = 0, OWNERSHIP_DROP, SECRECY_LABEL, INTEGRITY_LABEL};

struct tag {
	struct list_head next;
	long int content;
};

struct task_difc {
	bool confined;
	struct list_head slabel;
	struct list_head ilabel;
	struct list_head olabel;
};

struct inode_difc {
	struct list_head slabel;
	struct list_head ilabel;
};

struct socket_difc {
	struct inode_difc *isp;
	struct inode_difc *peer_isp;
};

extern size_t difc_label_change(struct file *file, const char __user *buf, 
			size_t size, loff_t *ppos, struct task_difc *tsp, enum label_types ops);

extern size_t difc_confine_task(struct file *file, const char __user *buf, 
				size_t size, loff_t *ppos, struct task_difc *tsp);
#endif

