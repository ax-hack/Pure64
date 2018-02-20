/* =============================================================================
 * Pure64 -- a 64-bit OS/software loader written in Assembly for x86-64 systems
 * Copyright (C) 2008-2017 Return Infinity -- see LICENSE.TXT
 * =============================================================================
 */

#include "map.h"

#include "ahci.h"
#include "alloc.h"
#include "e820.h"
#include "string.h"

#ifndef NULL
#define NULL ((void *) 0x00)
#endif

#define BOUNDARY 0x1000

static void *ahci_malloc(void *map_ptr, unsigned int size) {
	return pure64_map_malloc((struct pure64_map *) map_ptr, size);
}

static void *ahci_realloc(void *map_ptr, void *addr, unsigned int size) {
	return pure64_map_realloc((struct pure64_map *) map_ptr, addr, size);
}

static void ahci_free(void *map_ptr, void *addr) {
	return pure64_map_free((struct pure64_map *) map_ptr, addr);
}

static int append_alloc(struct pure64_map *map,
                        struct pure64_alloc *alloc) {

	struct pure64_alloc *alloc_table;
	uint64_t alloc_table_size;

	alloc_table = map->alloc_table;

	alloc_table_size = map->alloc_count + 1;
	alloc_table_size *= sizeof(struct pure64_alloc);

	alloc_table = pure64_map_realloc(map, alloc_table, alloc_table_size);
	if (alloc_table == NULL)
		return -1;

	map->alloc_table = alloc_table;
	map->alloc_count++;

	map->alloc_table[map->alloc_count - 1] = *alloc;

	return 0;
}

static void sort_alloc_table(struct pure64_map *map) {

	uint64_t i;
	struct pure64_alloc tmp;
	struct pure64_alloc *a;
	struct pure64_alloc *b;
	uint64_t addr_a;
	uint64_t addr_b;
	int sorted_flag;

bubbleSortLoop:

	sorted_flag = 0;

	for (i = 0; (i < map->alloc_count) && ((i + 1) < map->alloc_count); i++) {
		a = &map->alloc_table[i];
		b = &map->alloc_table[i + 1];
		addr_a = (uint64_t) a->addr;
		addr_b = (uint64_t) b->addr;
		if (addr_a > addr_b) {
			tmp = *a;
			*a = *b;
			*b = tmp;
			sorted_flag = 1;
		}
	}

	if (sorted_flag)
		goto bubbleSortLoop;
}

static void *find_suitable_addr(struct pure64_map *map, uint64_t size) {

	struct pure64_e820 *e820;
	struct pure64_alloc *alloc;
	/* candidate address */
	uint64_t addr;
	/* e820 entry address
	 * and size */
	uint64_t addr2;
	uint64_t size2;
	/* allocation entry address
	 * and size */
	uint64_t addr3;
	uint64_t size3;
	uint64_t i;

	e820 = map->e820;

	/* Initial address is zero */

	addr = 0;

	/* Search the E820 map for a free
	 * entry */

	while (!pure64_e820_end(e820)) {

		/* Make sure that the E820 entry
		 * points to usable memory. */
		if (!pure64_e820_usable(e820)) {
			e820 = pure64_e820_next(e820);
			continue;
		}

		addr2 = (uint64_t) e820->addr;
		size2 = (uint64_t) e820->size;

		/* Check if E820 section can
		 * fit the requested size. */
		if (size2 < size) {
			e820 = pure64_e820_next(e820);
			continue;
		}

		addr = addr2;

		/* Iterate the allocation table
		 * and make sure that new address
		 * will not overlap with existing
		 * memory allocations. */

		for (i = 0; i < map->alloc_count; i++) {
			alloc = &map->alloc_table[i];
			/* Check if candidate address
			 * overlaps this entry in the
			 * allocation table. */
			addr3 = (uint64_t) alloc->addr;
			size3 = alloc->reserved;
			if (((addr + size) > addr3) && (addr <= addr3)) {
				/* Move candidate address to the
				 * end of the existing allocation. */
				addr = addr3 + size3;
				/* Check to make sure that the candidate
				 * address still fits the requirement size. */
				if ((addr + size) >= (addr2 + size2)) {
					/* The candidate address can no longer
					 * fit the size requirement. */
					break;
				}
			}
		}

		/* Check if the loop exited early.
		 * If the loop exited early, that
		 * means that the E820 entry does
		 * not contain a suitable space for
		 * the memory block. */

		if (i < map->alloc_count) {
			/* Continue to the next
			 * E820 entry, and search
			 * the allocation table
			 * again. */
			e820 = pure64_e820_next(e820);
			continue;
		}

		/* If the loop has reached this point,
		 * that means that the candidate address
		 * will not overlap with an existing
		 * allocation. We may return it to the
		 * caller now. */

		return (void *) addr;
	}

	/* If this point has been reached,
	 * that means that none of the E820
	 * entries contain enough memory for
	 * this allocation. */

	return NULL;
}

static struct pure64_alloc *find_alloc_entry(struct pure64_map *map, void *addr) {

	uint64_t i;

	for (i = 0; i < map->alloc_count; i++) {
		if (map->alloc_table[i].addr == addr)
			return &map->alloc_table[i];
	}

	return NULL;
}

void pure64_map_init(struct pure64_map *map) {

	uint64_t addr;
	uint64_t size;
	struct pure64_e820 *e820;

	addr = 0;

	/* Initialize E820 address */

	e820 = (struct pure64_e820 *) 0x6000;

	map->e820 = e820;

	/* Find an address to start the
	 * allocation table at. It will
	 * move locations when it needs to.
	 */

	while (!pure64_e820_end(e820)) {

		/* Check that the memory is usable. */
		if (!pure64_e820_usable(e820)) {
			e820 = pure64_e820_next(e820);
			continue;
		}

		/* Check that the memory is not already
		 * used by Pure64. */
		addr = (uint64_t) e820->addr;
		size = e820->size;
		if ((addr + size) < 0x60000) {
			e820 = pure64_e820_next(e820);
			continue;
		} else if (addr < 0x60000) {
			size = 0x60000 - addr;
			addr = 0x60000;
		}

		/* Check that the memory can hold at least
		 * two entries. One for the initial Pure64
		 * memory map, and the other for the allocation
		 * table itself. */
		if (size < (sizeof(struct pure64_alloc) * 2)) {
			e820 = pure64_e820_next(e820);
			continue;
		}

		/* Found a suitable location for the
		 * initial allocation table. */
		break;
	}

	/* Check if search was successful. */
	if (pure64_e820_end(e820)) {
		/* Search was not successful.
		 * Future calls for memory allocations
		 * will fail. */
		map->alloc_table = NULL;
		map->alloc_count = 0;
	} else {
		/* Search was a success.
		 * Initialize the memory allocation
		 * table at the new address. */
		map->alloc_table = (struct pure64_alloc *) addr;
		map->alloc_count = 2;
		/* Pure64 initial memory map */
		map->alloc_table[0].addr = (void *) 0x00;
		map->alloc_table[0].size = 0x60000;
		map->alloc_table[0].reserved = 0x60000;
		/* allocation table (self reference) */
		map->alloc_table[1].addr = map->alloc_table;
		map->alloc_table[1].size = sizeof(struct pure64_alloc) * 2;
		map->alloc_table[1].reserved = sizeof(struct pure64_alloc) * 2;
	}

	map->ahci_driver = pure64_map_malloc(map, sizeof(struct ahci_driver));
	if (map->ahci_driver != NULL) {
		ahci_init(map->ahci_driver);
		map->ahci_driver->mm_data = map;
		map->ahci_driver->mm_malloc = ahci_malloc;
		map->ahci_driver->mm_realloc = ahci_realloc;
		map->ahci_driver->mm_free = ahci_free;
		ahci_load(map->ahci_driver);
	}
}

void *pure64_map_malloc(struct pure64_map *map, uint64_t size) {

	struct pure64_alloc alloc;

	if (map->alloc_table == NULL)
		return NULL;

	alloc.size = size;

	if ((size % BOUNDARY) != 0)
		size += BOUNDARY - (size % BOUNDARY);

	alloc.reserved = size;

	alloc.addr = find_suitable_addr(map, size);
	if (alloc.addr == NULL)
		return NULL;

	if (append_alloc(map, &alloc) != 0)
		return NULL;

	return alloc.addr;
}

void *pure64_map_realloc(struct pure64_map *map,
                         void *addr,
                         uint64_t size) {

	/* Existing allocation table
	 * entry. */
	struct pure64_alloc *alloc;
	/* The next address for the
	 * memory block. */
	void *addr2;
	/* The number of bytes reserved
	 * for a new allocation (if one
	 * is actually made). */
	uint64_t reserved;

	/* Check if caller passed NULL,
	 * which means they want a completely
	 * new memory section. */

	if (addr == NULL) {
		return pure64_map_malloc(map, size);
	}

	/* Find the entry in the allocation
	 * table, so we know how much data
	 * to copy over. */

	alloc = find_alloc_entry(map, addr);
	if (alloc == NULL)
		return NULL;

	/* Check to make sure that there is
	 * more memory already reserved for
	 * this entry. */

	if (alloc->reserved >= size) {
		/* The allocation entry
		 * has enough memory reserved
		 * already. */
		alloc->size = size;
		return addr;
	}

	/* Give the size of the allocation
	 * room to grow. Allocate it on a
	 * boundary. */

	reserved = size;

	if ((reserved % BOUNDARY) != 0) {
		reserved += BOUNDARY - (reserved % BOUNDARY);
	}

	/* Find an address to put the new data
	 * into. */

	addr2 = find_suitable_addr(map, size);
	if (addr2 == NULL)
		return NULL;

	/* Assign the new address to the allocation
	 * table entry and resort the entries. */

	alloc->addr = addr2;
	alloc->size = size;
	alloc->reserved = reserved;

	/* Copy memory from old location to the
	 * new location. */

	pure64_memcpy(addr2, addr, alloc->size);

	sort_alloc_table(map);

	return (void *) addr2;
}

void pure64_map_free(struct pure64_map *map,
                     void *addr) {

	struct pure64_alloc *alloc;

	/* Check if the address is a null
	 * pointer. This means that this
	 * function should do nothing. */
	if (addr == NULL)
		return;

	/* This function finds the allocation
	 * entry and puts it at the end of the
	 * table. Once it's at the end of the
	 * table, it decrements the entry count
	 * so that the entry is no longer visible. */

	/* Find the allocation entry. */
	alloc = find_alloc_entry(map, addr);
	if (alloc == NULL)
		return;

	/* This will cause the sort function to put
	 * the entry at the end of the table. */
	alloc->addr = (void *) 0xffffffffffffffff;
	sort_alloc_table(map);

	map->alloc_count--;
}
