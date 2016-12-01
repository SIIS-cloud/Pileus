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


#include <linux/seq_file.h>
#include <linux/fcntl.h>
#include <linux/module.h>
#include <linux/parser.h>
#include <linux/uaccess.h>

#include "difc.h"

static int add_ownership(struct task_difc *tsp, int tag_content) {
	bool present = false;
	struct tag *t, *new_tag;
	int result = -EINVAL;

	list_for_each_entry_rcu(t, &tsp->olabel, next)
		if (t->content == tag_content) {
			present = true;
			break;
		}
	if (!present) {

		// TODO: check authenticity of ownership before adding it

		if (tag_struct == NULL)
			printk(KERN_DEBUG "SYQ: tag_struct is NULL\n");
		new_tag = kmem_cache_alloc(tag_struct, GFP_NOFS);
		if (!new_tag) {
			result = -ENOMEM;
			return result;
		}
		new_tag->content = tag_content;
		list_add_tail_rcu(&new_tag->next, &tsp->olabel);
	}
	return 0;
}

static int drop_ownership(struct task_difc *tsp, int tag_content) {
	struct tag *t;
	struct tag *next_tag;

	list_for_each_entry_safe(t, next_tag, &tsp->olabel, next) {
		if (t->content == tag_content) {
			list_del_rcu(&t->next);
			kmem_cache_free(tag_struct, t);
		}
	}
	return 0;	
}


size_t difc_confine_task(struct file *file, const char __user *buf, 
				size_t size, loff_t *ppos, struct task_difc *tsp) {
	int confine;
	char temp[2];

	if (size > sizeof(temp) || *ppos != 0)
		return -EINVAL;
	
	if (copy_from_user(temp, buf, size) != 0)
		return -EFAULT;

	temp[size]= '\0';

	if (sscanf(temp, "%d", &confine) != 1)
		return -EINVAL;

	if (confine == 1)
		tsp->confined = true;
	else if (confine == 0)
		tsp->confined = false;
	else
		return -EINVAL;

	return size;

}

size_t difc_label_change(struct file *file, const char __user *buf, 
				size_t size, loff_t *ppos, 
				struct task_difc *tsp, enum label_types ops) {
	char *data, *pos, *next_tag;
	int result;
	long int tag_content;
	struct list_head new_label;
	struct tag *new_tag;

	if (size >= PAGE_SIZE)
		size = PAGE_SIZE -1;

	result = -EINVAL;
	if (*ppos != 0)
		return result;

	result = -ENOMEM;
	data = kmalloc(size + 1, GFP_KERNEL);
	if (!data)
		return result;

	*(data + size) = '\0';
	
	INIT_LIST_HEAD(&new_label);
	result = -EFAULT;
	if (copy_from_user(data, buf, size))
		goto out;

	pos = data;	
	for(; pos; pos = next_tag) {
		next_tag = strchr(pos, ';');
		if (next_tag) {
			*next_tag = '\0';
			next_tag++;
			if (*next_tag == '\0')
				next_tag = NULL;
		}
		
		pos = skip_spaces(pos);
		tag_content = simple_strtoul(pos, &pos, 10);
		//printk(KERN_DEBUG "SYQ: ownership: %ld\n", tag_content);
		if (ops == OWNERSHIP_ADD) {
			result = add_ownership(tsp, tag_content);
			if (result < 0)
				goto out;	
		} 
		else if (ops == OWNERSHIP_DROP) {
			result = drop_ownership(tsp, tag_content);
			if (result < 0)
				goto out;	
		}
		else {
			new_tag = kmem_cache_alloc(tag_struct, GFP_NOFS);
			if (!new_tag) {
				result = -ENOMEM;
				goto out;
			}
			new_tag->content = tag_content;
			list_add_tail_rcu(&new_tag->next, &new_label);
		}

	}

	
	if (ops == SECRECY_LABEL || ops == INTEGRITY_LABEL) {
		if (ops == SECRECY_LABEL) { 
			result = can_label_change(&tsp->slabel, &new_label, &tsp->olabel);
			if (result != 0) {
				clean_label(&new_label);
				printk(KERN_ALERT "SYQ: %s secrecy label (%s) denied\n", __func__, data);
				goto out;
			} else {
				change_label(&tsp->slabel, &new_label);
			}
		} else {
			result = can_label_change(&tsp->ilabel, &new_label, &tsp->olabel);
			if (result != 0) {
				clean_label(&new_label);
				printk(KERN_ALERT "SYQ: %s integrity label (%s) denied\n", __func__, data);
				goto out;
			} else {
				change_label(&tsp->ilabel, &new_label);
			}
		}
	}
	result = size;
out:
	list_del(&new_label);
	kfree(data);
	return result;
}
