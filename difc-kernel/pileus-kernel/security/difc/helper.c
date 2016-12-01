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

#include "difc.h"

void clean_label(struct list_head *label) {
	struct tag *t, *next_t;

	/* Free label */
	list_for_each_entry_safe(t, next_t, label, next) {
		list_del_rcu(&t->next);
		kmem_cache_free(tag_struct, t);
	}
}

/*
	check if p belongs to q + o
*/
int is_label_subset(struct list_head *p, struct list_head *o, struct list_head *q) {
	int rc = 0;
	bool present;
	struct tag *tp, *tq, *to;


	list_for_each_entry_rcu(tp, p, next) {
		present = false;
		list_for_each_entry_rcu(tq, q, next) {
			if (tq->content == tp->content){
				present = true;
				break;
			}
		}
		if (present)
			continue;
		list_for_each_entry_rcu(to, o, next) {
			if (to->content == tp->content) {
				present = true;
				break;
			}
		}
		if (present) {
			continue;
		}
		else {
			printk("SYQ: tag %d error\n", tp->content);
			return -EACCES;
		}
	}
	return rc;
}

int can_label_change(struct list_head *old_label, struct list_head *new_label,
		struct list_head *olabel) {
	struct tag *new_t, *old_t, *ownership_t;
	bool present, allow;
	int result = -EPERM;

	if (olabel == NULL)
		return 0;

	list_for_each_entry_rcu(new_t, new_label, next) {
		present = false;
		list_for_each_entry_rcu(old_t, old_label, next) {
			if (new_t->content == old_t->content) {
				present = true;
				break;
			}
		}
		if (!present) {
			allow = false;
			list_for_each_entry_rcu(ownership_t, olabel, next) {
				if (ownership_t->content == new_t->content) {
					allow = true;
					break;
				}
			}
			if (!allow)
				goto out;
		}
	}


	list_for_each_entry_rcu(old_t, old_label, next) {
		present = false;
		list_for_each_entry_rcu(new_t, new_label, next) {
			if (old_t->content == new_t->content) {
				present = true;
				break;
			}
		}
		if (!present) {
			allow = false;
			list_for_each_entry_rcu(ownership_t, olabel, next) {
				if (ownership_t->content == old_t->content) {
					allow = true;
					break;
				}
			}
			if (!allow)
				goto out;
		}
	}
	
	result = 0;
	return result;
out:
	printk(KERN_ALERT "SYQ: cannot change into label\n");
	// TEMP HACK FOR DEBUG
	// always allow file label set
	result = 0;
	return result;
}

void change_label(struct list_head *old_label, struct list_head *new_label) {
	struct tag *t, *next_t;

	/* Free old label */
	clean_label(old_label);
	
	/* Set new label */
	list_for_each_entry_safe(t, next_t, new_label, next) {
		list_del_rcu(&t->next);
		list_add_tail_rcu(&t->next, old_label);
	}
		
}


int security_to_labels(struct list_head *slabel, 
			struct list_head *ilabel, 
			char **labels, int *len) {

	struct tag *t;
	int llen = 0; 
	int ret;

	*labels = kzalloc(MAX_LABEL_SIZE, GFP_NOFS);
	if (!*labels)
		return -ENOMEM;

	/*
	* TODO: may buffer overflow here, fix it!
	*/

	list_for_each_entry_rcu(t, slabel, next) {
		ret = sprintf(*labels + llen, "%ld;", t->content);
		llen += ret;
	}

	(*labels)[llen++] = '|';

	list_for_each_entry_rcu(t, ilabel, next) {
		ret = sprintf(*labels + llen, "%ld;", t->content);
		llen += ret;
	}

	*len = llen;

	return 0;
}

int security_set_labels(struct list_head *slabel,
			struct list_head *ilabel,
			struct task_difc *tsp,
			const char *value, int size) {
	struct tag *new_tag;
	char *pos, *next_tag, *data, *delimiter;
	int ret = -ENOMEM;
	int tag_content;
	struct list_head new_ilabel, new_slabel;

	data = kmalloc(size + 1, GFP_KERNEL);
	if (!data)
		return ret;

	INIT_LIST_HEAD(&new_ilabel);	
	INIT_LIST_HEAD(&new_slabel);

	memcpy(data, value, size);
	*(data + size) = '\0';

	pos = data;
	delimiter = strchr(data, '|');
	if (!delimiter) {
		ret = -EINVAL;
		goto out;
	}

	for(; pos < delimiter; pos = next_tag) {
		next_tag = strchr(pos, ';');
		if (next_tag) {
			*next_tag = '\0';
			next_tag++;
		}
		pos = skip_spaces(pos);
		tag_content = simple_strtoul(pos, &pos, 10);
		//printk(KERN_DEBUG "SYQ: inode tag: %d\n", tag_content);
		new_tag = kmem_cache_alloc(tag_struct, GFP_NOFS);
		if (!new_tag) {
			ret = -ENOMEM;
			goto out;
		}
		new_tag->content = tag_content;
		list_add_tail_rcu(&new_tag->next, &new_slabel);
	}
	
	if (tsp != NULL && tsp->confined)
		ret = can_label_change(slabel, &new_slabel, &tsp->olabel);
	else if (tsp != NULL && !tsp->confined)
		ret = 0;
	else
		ret = can_label_change(slabel, &new_slabel, NULL);
	if (ret != 0) {
		printk(KERN_ALERT "SYQ: %s secrecy label (%s) denied\n", __func__, data);
		clean_label(&new_slabel);
		goto out;
	}

	pos++;

	for(; pos < data + size; pos = next_tag) {
		next_tag = strchr(pos, ';');
		if (next_tag) {
			*next_tag = '\0';
			next_tag++;
		} else {
			ret = -EINVAL;
			goto out;
		}
		pos = skip_spaces(pos);
		tag_content = simple_strtoul(pos, &pos, 10);
		//printk(KERN_DEBUG "SYQ: inode tag: %d\n", tag_content);
		new_tag = kmem_cache_alloc(tag_struct, GFP_NOFS);
		if (!new_tag) {
			ret = -ENOMEM;
			goto out;
		}
		new_tag->content = tag_content;
		list_add_tail_rcu(&new_tag->next, &new_ilabel);
	}

	if (tsp != NULL && tsp->confined)
		ret = can_label_change(ilabel, &new_ilabel, &tsp->olabel);
	else if (tsp != NULL && !tsp->confined)
		ret = 0;
	else
		ret = can_label_change(ilabel, &new_ilabel, NULL);
		
	if (ret != 0) {
		clean_label(&new_slabel);
		clean_label(&new_ilabel);
		printk(KERN_ALERT "SYQ: %s integrity label (%s) denied\n", __func__, data);
		goto out;
	}

	change_label(slabel, &new_slabel);
	change_label(ilabel, &new_ilabel);
	ret = 0;

out:
	kfree(data);
	list_del(&new_ilabel);
	list_del(&new_slabel);
	return ret;
}
