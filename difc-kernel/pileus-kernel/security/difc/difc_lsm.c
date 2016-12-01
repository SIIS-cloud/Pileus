/*
# The Pileus additions are ...
#
#  Copyright (c) 2016 The Pennsylvania State University
#  Systems and Internet Infrastructure Security Laboratory
#
# they were developed by:
#
#  Yuqiong Sun          <yus138@cse.psu.edu>
#  Giuseppe Petracca    <gxp18@cse.psu.edu>
#  Trent Jaeger         <tjaeger@cse.psu.edu>
#
# Unless otherwise noted, all code additions are ...
#
#  * Licensed under the Apache License, Version 2.0 (the "License");
#  * you may not use this file except in compliance with the License.
#  * You may obtain a copy of the License at
#  *
#  * http://www.apache.org/licenses/LICENSE-2.0
#  *
#  * Unless required by applicable law or agreed to in writing, software
#  * distributed under the License is distributed on an "AS IS" BASIS,
#  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  * See the License for the specific language governing permissions and
#  * limitations under the License.
*/

#include <linux/xattr.h>
#include <linux/pagemap.h>
#include <linux/mount.h>
#include <linux/stat.h>
#include <linux/kd.h>
#include <asm/ioctls.h>
#include <linux/dccp.h>
#include <linux/mutex.h>
#include <linux/pipe_fs_i.h>
#include <linux/audit.h>
#include <linux/magic.h>
#include <linux/dcache.h>
#include <linux/personality.h>
#include <linux/msg.h>
#include <linux/shm.h>
#include "difc.h"

struct kmem_cache *tag_struct;

static struct task_difc *new_task_difc(gfp_t gfp) {
	struct task_difc *tsp;
	tsp = kzalloc(sizeof(struct task_difc), gfp);
	
	if (!tsp)
		return NULL;
	tsp->confined = false;
	INIT_LIST_HEAD(&tsp->slabel);
	INIT_LIST_HEAD(&tsp->ilabel);
	INIT_LIST_HEAD(&tsp->olabel);
	
	return tsp;
} 

static void difc_free_label(struct list_head *label) {
	struct tag *t, *t_next;
	list_for_each_entry_safe(t, t_next, label, next) {
		list_del_rcu(&t->next);
		kmem_cache_free(tag_struct, t);
	}
}

static int difc_copy_label(struct list_head *old, struct list_head *new) {
	struct tag *t;
	
	list_for_each_entry(t, old, next) {
		struct tag *new_tag;
		new_tag = kmem_cache_alloc(tag_struct, GFP_NOFS);
		if (new_tag == NULL)
			goto out;
		new_tag->content = t->content;
		list_add_tail(&new_tag->next, new);
	}
	return 0;

out:
	return -ENOMEM;
}

static struct inode_difc *inode_security_novalidate(struct inode *inode) {
	return inode->i_security;
}

static struct inode_difc *new_inode_difc(void) {
	struct inode_difc *isp;
	struct task_difc *tsp;
	int rc = -ENOMEM;
	
	isp = kzalloc(sizeof(struct inode_difc), GFP_NOFS);
	
	if(!isp)
		return NULL;

	INIT_LIST_HEAD(&isp->slabel);
	INIT_LIST_HEAD(&isp->ilabel);

	tsp = current_security();

	/*
	* Label of inode is the label of the task creating the inode
	*/

	rc = difc_copy_label(&tsp->slabel, &isp->slabel);
	if (rc < 0)
		goto out;

	rc = difc_copy_label(&tsp->ilabel, &isp->ilabel);
	if (rc < 0)
		goto out;

	return isp;

out:
	kfree(isp);
	return NULL;
}

static int difc_cred_alloc_blank(struct cred *cred, gfp_t gfp) {
	struct task_difc *tsp;
	tsp = new_task_difc(gfp);
	if (!tsp)
		return -ENOMEM;
	
	cred->security = tsp;
	return 0;
}

static int difc_cred_prepare(struct cred *new, const struct cred *old, 
				gfp_t gfp) {
	struct task_difc *old_tsp = old->security;
	struct task_difc *new_tsp;
	int rc;

	new_tsp = new_task_difc(gfp);
	if (!new_tsp)
		return -ENOMEM;
	
	new_tsp->confined = old_tsp->confined;	
	rc = difc_copy_label(&old_tsp->slabel, &new_tsp->slabel);
	if (rc != 0)
		return rc;

	rc = difc_copy_label(&old_tsp->ilabel, &new_tsp->ilabel);
	if (rc != 0)
		return rc;

	// Don't copy ownerships
	rc = difc_copy_label(&old_tsp->olabel, &new_tsp->olabel);
	if (rc != 0)
		return rc;

	new->security = new_tsp;
	return 0;
}

static void difc_cred_free(struct cred *cred) {
	struct task_difc *tsp = cred->security;

	if (tsp == NULL)
		return;
	cred->security = NULL;
	
	difc_free_label(&tsp->ilabel);
	list_del(&tsp->ilabel);

	difc_free_label(&tsp->slabel);
	list_del(&tsp->slabel);

	difc_free_label(&tsp->olabel);
	list_del(&tsp->olabel);

	kfree(tsp);
}

static int difc_bprm_check_security(struct linux_binprm *bprm) {
	return 0;
} 

static int difc_inode_alloc_security(struct inode *inode) {
	struct inode_difc *isp;

	isp = new_inode_difc();
	if (!isp)
		return -ENOMEM;

	inode->i_security = isp;	
	return 0;
}

static void difc_inode_free_security(struct inode *inode) {
	struct inode_difc *isp = inode->i_security;

	if (isp == NULL)
		return;
	inode->i_security = NULL;

	difc_free_label(&isp->ilabel);
	list_del(&isp->ilabel);

	difc_free_label(&isp->slabel);
	list_del(&isp->slabel);

	kfree(isp);
}

static int difc_inode_init_security(struct inode *inode, struct inode *dir,
				const struct qstr *qstr, const char **name,
				void **value, size_t *len) {
	struct inode_difc *isp = inode->i_security;
	int rc, llen;
	char *labels;
	struct task_difc *tsp = current_security();

	/* 
	if (!isp) {
		printk(KERN_ALERT "SYQ: inode->i_security is null (%s)\n", __func__);
		return 0;
	}
	*/

	if (tsp->confined) {
		printk(KERN_ALERT "SYQ: new inode is created %ld\n", inode->i_ino);
	}

	if (name)
		*name = XATTR_DIFC_SUFFIX;
	
	if (value && len) {
		rc = security_to_labels(&isp->slabel, &isp->ilabel, &labels, &llen);
		if (rc < 0)
			return rc;
		*value = kstrdup(labels, GFP_NOFS);
		kfree(labels);
		if (!*value) {
			printk(KERN_ALERT "memory error in %s, %d\n", __func__, __LINE__);
			return -ENOMEM;
		}	
		*len = llen;
	}

	return 0;
}

static int difc_inode_getsecurity(const struct inode *inode,
				const char *name, void **buffer,
				bool alloc) {
	struct inode_difc *isp = inode->i_security;
	int len;
	int rc = 0;

	if (!isp) {
		printk(KERN_DEBUG "SYQ: inode->i_security is null (%s)\n", __func__);
		return rc; 
	}

	if (strcmp(name, XATTR_DIFC_SUFFIX) == 0) {
		rc = security_to_labels(&isp->slabel, &isp->ilabel, (char **)buffer, &len);
		if (rc < 0)
			return rc;
		else
			return len;
	}

	return rc;
}

// called by difc_inode_setxattr()
static int difc_inode_setsecurity(struct inode *inode, const char *name,
				const void *value, size_t size, int flags) {

	struct inode_difc *isp = inode->i_security;
	struct task_difc *tsp = current_security();
	int rc = 0;	

	if (size >= MAX_LABEL_SIZE || value ==NULL || size == 0)
		return -EINVAL;

	if (!isp) {
		printk(KERN_DEBUG "SYQ: inode->i_security is null (%s)\n", __func__);
		return rc; 
	}

	rc = security_set_labels(&isp->slabel, &isp->ilabel, tsp, value, size);
	if (rc < 0)
		return rc;

	return 0;
}

static int difc_inode_listsecurity(struct inode *inode, char *buffer, 
					size_t buffer_size) {
	int len = sizeof(XATTR_NAME_DIFC);
	if (buffer != NULL && len <= buffer_size)
		memcpy(buffer, XATTR_NAME_DIFC, len);
	return len;
}

/*
* FIX IT: Currently, anybody could set xattr
*/
static int difc_inode_getxattr(struct dentry *dentry, const char *name) {
	return 0;
}

static int difc_inode_setxattr(struct dentry *dentry, const char *name,
				const char *value, size_t size, int flags) {
	return 0;
}

static void difc_inode_post_setxattr(struct dentry *dentry, const char *name,
				const char *value, size_t size, int flags) {
	
	struct inode *inode = dentry->d_inode;

	difc_inode_setsecurity(inode, name, value, size, flags);
	
	return;
}

static int difc_inode_unlink(struct inode *dir, struct dentry *dentry) {
	int rc = 0;
	struct task_difc *tsp = current_security();
	struct super_block *sbp = dentry->d_inode->i_sb;
	struct inode_difc *isp = dentry->d_inode->i_security;
	struct tag *t;

	if (!tsp->confined)
		return rc;

	if ((dir->i_mode & S_ISVTX) == 0)
		return rc;

	switch (sbp->s_magic) {
		case PIPEFS_MAGIC:
		case SOCKFS_MAGIC:
		case CGROUP_SUPER_MAGIC:
		case DEVPTS_SUPER_MAGIC:
		case PROC_SUPER_MAGIC:
		case TMPFS_MAGIC:
		case SYSFS_MAGIC:
		case RAMFS_MAGIC:
		case DEBUGFS_MAGIC:
			return rc;
		default:
			/* For now, only check on the rest cases */
			break;
	}

	/* Only allow when current can integrity write the dentry inode */
	list_for_each_entry_rcu(t, &isp->ilabel, next)
		if (t->content == 0)
			return rc;

	rc = is_label_subset(&isp->ilabel, &tsp->olabel, &tsp->ilabel);
	if (rc < 0) {
		printk(KERN_ALERT "SYQ: cannot delete file (%s)\n", dentry->d_name.name);
		rc = -EPERM;
		goto out;
	}
	
out:
	/* For debugging, always return 0 */
	rc = 0;
	return rc;
}

static int difc_inode_rmdir(struct inode *dir, struct dentry *dentry) {
	/* 
	* Currently, we assume files under the directory would have the same label
	* if a dir is a/b/c and labels are a(1), b(1;2), c(1;2;3)
	* Impossible, since otherwise the file cannot be read due to parent directories have less integrity
	*/
	return difc_inode_unlink(dir, dentry);
}

static int difc_inode_permission(struct inode *inode, int mask) {
	int rc = 0;
	struct task_difc *tsp = current_security();
	struct inode_difc *isp = inode->i_security;
	struct super_block *sbp = inode->i_sb;
	struct tag *t;
	int top, down;

	mask &= (MAY_READ|MAY_WRITE|MAY_EXEC|MAY_APPEND);

	/* For some reason, label of / is not persistent.  Thus if /, return */
	if (mask == 0 || !isp || inode->i_ino == 2) 
		return rc;

	if (!tsp->confined)
		return rc;

	switch (sbp->s_magic) {
		case PIPEFS_MAGIC:
		case SOCKFS_MAGIC:
		case CGROUP_SUPER_MAGIC:
		case DEVPTS_SUPER_MAGIC:
		case PROC_SUPER_MAGIC:
		case TMPFS_MAGIC:
		case SYSFS_MAGIC:
		case RAMFS_MAGIC:
		case DEBUGFS_MAGIC:
			return rc;
		default:
			/* For now, only check on the rest cases */
			break;
	}

	if (mask & (MAY_READ | MAY_EXEC)) {
		/*
		* Check for special tag: 65535 and 0
		* If integrity label contains 65535 and secrecy label contains 0, the inode is globally readable
		*/
		top = -1;
		down = -1;
		list_for_each_entry_rcu(t, &isp->ilabel, next)
			if (t->content == 65535)
				top = 0;
		list_for_each_entry_rcu(t, &isp->slabel, next)
			if (t->content == 0)
				down = 0;
		
		if (top ==0 && down == 0)
			goto out;

		if (top != 0) {
			/*
			*  Integrity: Ip <= Iq + Op
			*/
			rc = is_label_subset(&tsp->ilabel, &tsp->olabel, &isp->ilabel);
			if (rc < 0) {
				printk(KERN_ALERT "SYQ: integrity cannot read (0x%08x: %ld)\n", sbp->s_magic, inode->i_ino);
				rc = -EACCES;
				goto out;
			}
		}
	
		if (down != 0) {
			/*
			*  Secrecy: Sq <= Sp + Op
			*/
			rc = is_label_subset(&isp->slabel, &tsp->olabel, &tsp->slabel);
			if (rc < 0 && down != 0) {
				printk(KERN_ALERT "SYQ: secrecy cannot read (0x%08x: %ld)\n", sbp->s_magic, inode->i_ino);
				rc = -EACCES;
				goto out;
			}
		}
	} 
	
	if(mask & (MAY_WRITE | MAY_APPEND)) {
		/*
		* Check for special tag: 65535 and 0
		* If integrity label contains 0 and secrecy label contains 65535, the inode is globally writable
		*/
		top = -1;
		down = -1;
		list_for_each_entry_rcu(t, &isp->ilabel, next)
			if (t->content == 0)
				top = 0;
		list_for_each_entry_rcu(t, &isp->slabel, next)
			if (t->content == 65535)
				down = 0;
		
		if (top ==0 && down == 0)
			goto out;

		if (top != 0) {
			/*
			*  Integrity: Iq <= Ip + Op
			*/
			rc = is_label_subset(&isp->ilabel, &tsp->olabel, &tsp->ilabel);
			if (rc < 0) {
				printk(KERN_ALERT "SYQ: integrity cannot write (0x%08x: %ld)\n", sbp->s_magic, inode->i_ino);
				rc = -EACCES;
				goto out;
			}
		}

		if (down != 0) {
			/*
			*  Secrecy: Sp <= Sq + Op
			*/
			rc = is_label_subset(&tsp->slabel, &tsp->olabel, &isp->slabel);
			if (rc < 0) {
				printk(KERN_ALERT "SYQ: secrecy cannot write (0x%08x: %ld)\n", sbp->s_magic, inode->i_ino);
				rc = -EACCES;
				goto out;
			}
		}
	}

out:
	/* Always allow for debugging */
	rc = 0;
	return rc;
}

static void difc_d_instantiate(struct dentry *opt_dentry, struct inode *inode) {
	struct inode_difc *isp;
	struct super_block *sbp;
	struct dentry *dp;
	char *buffer;
	int rc, len;

	if (!inode)
		return;

	isp = inode->i_security;
	sbp = inode->i_sb;

	// root
	if (opt_dentry->d_parent == opt_dentry)
		return;

	switch (sbp->s_magic) {
		case PIPEFS_MAGIC:
		case SOCKFS_MAGIC:
		case CGROUP_SUPER_MAGIC:
		case DEVPTS_SUPER_MAGIC:
		case PROC_SUPER_MAGIC:
		case TMPFS_MAGIC:
			break;
		default:
			if (S_ISSOCK(inode->i_mode)) 
				return;
			if (inode->i_op->getxattr == NULL)
				return;
				
			dp = dget(opt_dentry);
			buffer = kzalloc(MAX_LABEL_SIZE, GFP_KERNEL);
			if (!buffer) {
				printk(KERN_ALERT "SYQ: oops@%s\n", __func__);
				return;
			}
			len = inode->i_op->getxattr(dp, XATTR_NAME_DIFC, buffer, MAX_LABEL_SIZE);
			if (len > 0) {
				rc = security_set_labels(&isp->slabel, &isp->ilabel, NULL, buffer, len);
				if (rc < 0) {
					
					printk(KERN_ALERT "SYQ: security_set_labels (%s) @ %s\n", buffer, __func__);
				}
			}
			dput(dp);
			kfree(buffer);
			break;
	}
	return;
}

static int difc_sk_alloc_security(struct sock *sk, int family, gfp_t priority) {
	struct socket_difc *ssp;

	ssp = kzalloc(sizeof(struct socket_difc), priority);
	if (!ssp)
		return -ENOMEM;

	// Set in difc_socket_post_create()?
	ssp->isp = NULL;
	ssp->peer_isp = NULL;
	sk->sk_security =  ssp;

	return 0;
}

static void difc_sk_free_security(struct sock *sk) {
	struct socket_difc *ssp = sk->sk_security;
	sk->sk_security = NULL;

	if (!ssp)
		kfree(ssp);
}

static void difc_sk_clone_security(const struct sock *sk, struct sock *newsk) {
	struct socket_difc *ssp = sk->sk_security;
	struct socket_difc *newssp = newsk->sk_security;

	newssp->isp = ssp->isp;
	newssp->peer_isp = ssp->peer_isp;
}

static int difc_socket_create(int family, int type, int protocol, int kern) {
	/*
	* Seems like no need to set up
	*/
	return 0;
}


static int difc_socket_getpeersec_stream(struct socket *sock, char __user *optval,
					int __user *optlen, unsigned len) {
	struct socket_difc *ssp = sock->sk->sk_security;
	struct inode_difc *peer_isp;
	char *buffer;
	int label_len;
	int rc = 0;

	if (!ssp) {
		printk(KERN_ALERT "SYQ: socket security is null\n");
		return -ENOPROTOOPT;
	}
	
	peer_isp = ssp->peer_isp;
	if (!peer_isp) {
		printk(KERN_ALERT "SYQ: socket peer isp is null\n");
		return -ENOPROTOOPT;
	}

	rc = security_to_labels(&peer_isp->slabel, &peer_isp->ilabel, &buffer, &label_len);
	if (rc < 0)
		return rc;

	if (label_len > len) {
		rc = -ERANGE;
		goto out_len;
	}

	if (copy_to_user(optval, buffer, label_len))
		rc = -EFAULT;

out_len:
	if (put_user(label_len, optlen))
		rc = -EFAULT;
	kfree(buffer);
	return rc;
}

static int difc_socket_unix_stream_connect(struct sock *sock, struct sock *other,
						struct sock *newsk){
	struct socket_difc *ssp_sock = sock->sk_security;
	struct socket_difc *ssp_other = other->sk_security;
	struct socket_difc *ssp_newsk = newsk->sk_security;

	ssp_newsk->peer_isp = ssp_sock->isp;
	ssp_sock->peer_isp = ssp_newsk->isp;
	
	return 0;
}

static int difc_socket_post_create(struct socket *sock, int family,
				int type, int protocol, int kern) {
	struct inode_difc *isp = inode_security_novalidate(SOCK_INODE(sock));
	struct socket_difc *ssp;

	if (sock->sk) {
		ssp = sock->sk->sk_security;
		ssp->isp = isp;
	}	

	return 0;
}

static struct security_hook_list difc_hooks[] = {
	LSM_HOOK_INIT(cred_alloc_blank, difc_cred_alloc_blank),
	LSM_HOOK_INIT(cred_free, difc_cred_free),
	LSM_HOOK_INIT(cred_prepare, difc_cred_prepare),

	LSM_HOOK_INIT(inode_alloc_security, difc_inode_alloc_security),
	LSM_HOOK_INIT(inode_free_security, difc_inode_free_security),
	LSM_HOOK_INIT(inode_init_security, difc_inode_init_security),
	LSM_HOOK_INIT(inode_getxattr, difc_inode_getxattr),
	/*
	LSM_HOOK_INIT(inode_removexattr, difc_inode_removexattr),
	*/
	LSM_HOOK_INIT(inode_setxattr, difc_inode_setxattr),
	LSM_HOOK_INIT(inode_post_setxattr, difc_inode_post_setxattr),
	LSM_HOOK_INIT(inode_getsecurity, difc_inode_getsecurity),
	LSM_HOOK_INIT(inode_setsecurity, difc_inode_setsecurity),
	LSM_HOOK_INIT(inode_listsecurity, difc_inode_listsecurity),
	LSM_HOOK_INIT(inode_permission, difc_inode_permission),
	LSM_HOOK_INIT(inode_unlink, difc_inode_unlink),
	LSM_HOOK_INIT(inode_rmdir, difc_inode_rmdir),
	
	LSM_HOOK_INIT(d_instantiate, difc_d_instantiate),


	LSM_HOOK_INIT(sk_alloc_security, difc_sk_alloc_security),
	LSM_HOOK_INIT(sk_free_security, difc_sk_free_security),
	LSM_HOOK_INIT(sk_clone_security, difc_sk_clone_security),
	LSM_HOOK_INIT(socket_getpeersec_stream, difc_socket_getpeersec_stream),
	LSM_HOOK_INIT(socket_post_create, difc_socket_post_create),
	LSM_HOOK_INIT(socket_create, difc_socket_create),
	LSM_HOOK_INIT(unix_stream_connect, difc_socket_unix_stream_connect),

	LSM_HOOK_INIT(bprm_check_security, difc_bprm_check_security),
};

void __init difc_add_hooks(void) {
	security_add_hooks(difc_hooks, ARRAY_SIZE(difc_hooks));
}

static __init int difc_init(void) {

	struct task_difc *tsp;
	struct cred *cred;

	tag_struct = KMEM_CACHE(tag, SLAB_PANIC);

	tsp = new_task_difc(GFP_KERNEL);
	if (!tsp)
		return -ENOMEM;

	printk(KERN_DEBUG "SYQ: DIFC kernel module loaded.\n");
	
	cred = (struct cred *) current->cred;
	cred->security = tsp;

	if (!security_module_enable("difc"))
		return 0;
	difc_add_hooks();

	return 0;
}

security_initcall(difc_init);
