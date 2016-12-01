/*
 * DT NUMA Parsing support, based on the powerpc implementation.
 *
 * Copyright (C) 2015 Cavium Inc.
 * Author: Ganapatrao Kulkarni <gkulkarni@cavium.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>
#include <linux/ctype.h>
#include <linux/module.h>
#include <linux/nodemask.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <asm/smp_plat.h>

#define MAX_DISTANCE_REF_POINTS 8
static int min_common_depth;
static int distance_ref_points_depth;
static const __be32 *distance_ref_points;
static int distance_lookup_table[MAX_NUMNODES][MAX_DISTANCE_REF_POINTS];
static int default_nid;
static int of_node_to_nid_single(struct device_node *device);
static struct device_node *of_cpu_to_node(int cpu);

static void initialize_distance_lookup_table(int nid,
		const __be32 *associativity)
{
	int i;

	for (i = 0; i < distance_ref_points_depth; i++) {
		const __be32 *entry;

		entry = &associativity[be32_to_cpu(distance_ref_points[i])];
		distance_lookup_table[nid][i] = of_read_number(entry, 1);
	}
}

/* must hold reference to node during call */
static const __be32 *of_get_associativity(struct device_node *dev)
{
	return of_get_property(dev, "arm,associativity", NULL);
}

/* Returns nid in the range [0..MAX_NUMNODES-1], or -1 if no useful numa
 * info is found.
 */
static int associativity_to_nid(const __be32 *associativity)
{
	int nid = NUMA_NO_NODE;

	if (min_common_depth == -1)
		goto out;

	if (of_read_number(associativity, 1) >= min_common_depth)
		nid = of_read_number(&associativity[min_common_depth], 1);

	/* set 0xffff as invalid node */
	if (nid == 0xffff || nid >= MAX_NUMNODES)
		nid = NUMA_NO_NODE;

	if (nid != NUMA_NO_NODE)
		initialize_distance_lookup_table(nid, associativity);
out:
	return nid;
}

/* Returns the nid associated with the given device tree node,
 * or -1 if not found.
 */
static int of_node_to_nid_single(struct device_node *device)
{
	int nid = default_nid;
	const __be32 *tmp;

	tmp = of_get_associativity(device);
	if (tmp)
		nid = associativity_to_nid(tmp);
	return nid;
}

/* Walk the device tree upwards, looking for an associativity id */
int of_node_to_nid(struct device_node *device)
{
	struct device_node *tmp;
	int nid = NUMA_NO_NODE;

	of_node_get(device);
	while (device) {
		nid = of_node_to_nid_single(device);
		if (nid != NUMA_NO_NODE)
			break;

		tmp = device;
		device = of_get_parent(tmp);
		of_node_put(tmp);
	}
	of_node_put(device);

	return nid;
}

static int __init find_min_common_depth(unsigned long node)
{
	int depth;
	const __be32 *numa_prop;
	int nr_address_cells;

	/*
	 * This property is a set of 32-bit integers, each representing
	 * an index into the arm,associativity nodes.
	 *
	 * With form 1 affinity the first integer is the most significant
	 * NUMA boundary and the following are progressively less significant
	 * boundaries. There can be more than one level of NUMA.
	 */

	distance_ref_points = of_get_flat_dt_prop(node,
			"arm,associativity-reference-points",
			&distance_ref_points_depth);
	numa_prop = distance_ref_points;

	if (numa_prop) {
		nr_address_cells = dt_mem_next_cell(
				OF_ROOT_NODE_ADDR_CELLS_DEFAULT, &numa_prop);
		nr_address_cells = dt_mem_next_cell(
				OF_ROOT_NODE_ADDR_CELLS_DEFAULT, &numa_prop);
	}
	if (!distance_ref_points) {
		pr_debug("NUMA: arm,associativity-reference-points not found.\n");
		goto err;
	}

	distance_ref_points_depth /= sizeof(__be32);

	if (!distance_ref_points_depth) {
		pr_err("NUMA: missing arm,associativity-reference-points\n");
		goto err;
	}
	depth = of_read_number(distance_ref_points, 1);

	/*
	 * Warn and cap if the hardware supports more than
	 * MAX_DISTANCE_REF_POINTS domains.
	 */
	if (distance_ref_points_depth > MAX_DISTANCE_REF_POINTS) {
		pr_debug("NUMA: distance array capped at %d entries\n",
				MAX_DISTANCE_REF_POINTS);
		distance_ref_points_depth = MAX_DISTANCE_REF_POINTS;
	}

	return depth;
err:
	return -1;
}

void __init dt_numa_set_node_info(u32 cpu, u64 hwid,  void *dn_ptr)
{
	struct device_node *dn = (struct device_node *) dn_ptr;
	int nid = default_nid;

	if (dn)
		nid = of_node_to_nid_single(dn);

	node_cpu_hwid[cpu].node_id = nid;
	node_cpu_hwid[cpu].cpu_hwid = hwid;
	node_set(nid, numa_nodes_parsed);
}

int dt_get_cpu_node_id(int cpu)
{
	struct device_node *dn = NULL;
	int nid = default_nid;

	dn =  of_cpu_to_node(cpu);
	if (dn)
		nid = of_node_to_nid_single(dn);
	return nid;
}

static struct device_node *of_cpu_to_node(int cpu)
{
	struct device_node *dn = NULL;

	while ((dn = of_find_node_by_type(dn, "cpu"))) {
		const u32 *cell;
		u64 hwid;

		/*
		 * A cpu node with missing "reg" property is
		 * considered invalid to build a cpu_logical_map
		 * entry.
		 */
		cell = of_get_property(dn, "reg", NULL);
		if (!cell) {
			pr_err("%s: missing reg property\n", dn->full_name);
			return NULL;
		}
		hwid = of_read_number(cell, of_n_addr_cells(dn));

		if (cpu_logical_map(cpu) == hwid)
			return dn;
	}
	return NULL;
}

static int __init parse_memory_node(unsigned long node)
{
	const __be32 *reg, *endp, *associativity;
	int length;
	int nid = default_nid;

	associativity = of_get_flat_dt_prop(node, "arm,associativity", &length);

	if (associativity)
		nid = associativity_to_nid(associativity);

	reg = of_get_flat_dt_prop(node, "reg", &length);
	endp = reg + (length / sizeof(__be32));

	while ((endp - reg) >= (dt_root_addr_cells + dt_root_size_cells)) {
		u64 base, size;
		struct memblock_region *mblk;

		base = dt_mem_next_cell(dt_root_addr_cells, &reg);
		size = dt_mem_next_cell(dt_root_size_cells, &reg);
		pr_debug("NUMA-DT:  base = %llx , node = %u\n",
				base, nid);
		for_each_memblock(memory, mblk) {
			if (mblk->base == base) {
				node_set(nid, numa_nodes_parsed);
				numa_add_memblk(nid, mblk->base, mblk->size);
				break;
			}
		}
	}

	return 0;
}

/**
 * early_init_dt_scan_numa_map - parse memory node and map nid to memory range.
 */
int __init early_init_dt_scan_numa_map(unsigned long node, const char *uname,
				     int depth, void *data)
{
	const char *type = of_get_flat_dt_prop(node, "device_type", NULL);

	if (depth == 0) {
		min_common_depth = find_min_common_depth(node);
		if (min_common_depth < 0)
			return min_common_depth;
		pr_debug("NUMA associativity depth for CPU/Memory: %d\n",
				min_common_depth);
		return 0;
	}

	if (type) {
		if (strcmp(type, "memory") == 0)
			parse_memory_node(node);
	}
	return 0;
}

int dt_get_node_distance(int a, int b)
{
	int i;
	int distance = LOCAL_DISTANCE;

	for (i = 0; i < distance_ref_points_depth; i++) {
		if (distance_lookup_table[a][i] == distance_lookup_table[b][i])
			break;

		/* Double the distance for each NUMA level */
		distance *= 2;
	}
	return distance;
}

/* DT node mapping is done already early_init_dt_scan_memory */
int __init arm64_dt_numa_init(void)
{
	int i;
	u32 nodea, nodeb, distance, node_count = 0;

	of_scan_flat_dt(early_init_dt_scan_numa_map, NULL);

	for_each_node_mask(i, numa_nodes_parsed)
		node_count = i;
	node_count++;

	for (nodea =  0; nodea < node_count; nodea++) {
		for (nodeb = 0; nodeb < node_count; nodeb++) {
			distance = dt_get_node_distance(nodea, nodeb);
			numa_set_distance(nodea, nodeb, distance);
		}
	}
	return 0;
}
