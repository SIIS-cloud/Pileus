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

#include <linux/lsm_hooks.h>
#include <linux/cred.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/difc_module.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/sched.h>

#define MAX_LABEL_SIZE 300

extern struct kmem_cache *tag_struct;

void clean_label(struct list_head *label);

int is_label_subset(struct list_head *p, 
			struct list_head *o, 
			struct list_head *q);

void change_label(struct list_head *old_label,
		struct list_head *new_label);

int can_label_change(struct list_head *old_label,
		struct list_head *new_label,
		struct list_head *olabel);

int security_to_labels(struct list_head *slabel, 
			struct list_head *ilabel, 
			char **labels, int *len);

int security_set_labels(struct list_head *slabel,
			struct list_head *ilabel,
			struct task_difc *tsp,
			const char *value, int size);
