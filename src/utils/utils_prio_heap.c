/*
 * Simple insertion-only static-sized priority heap containing
 * pointers, based on CLR, chapter 7
 */

#include <linux/slab.h>
#include "utils_prio_heap.h"



int heap_init(struct ptr_heap *heap, size_t size, gfp_t gfp_mask)
{
	heap->ptrs = (uint64_t *)kmalloc(size, gfp_mask);
	if (!heap->ptrs)
		return -ENOMEM;
	heap->size = 0;
	heap->max = size / sizeof(uint64_t);
	// heap->gt = gt;
	return 0;
}

void heap_free(struct ptr_heap *heap)
{
	kfree(heap->ptrs);
}

void heap_insert(struct ptr_heap *heap, uint64_t p)
{
	uint64_t *ptrs = heap->ptrs;
	int pos;

	if (heap->size < heap->max) {
		/* Heap insertion */
		pos = heap->size++;
		while (pos > 0 && p < ptrs[(pos-1)/2]) {
			ptrs[pos] = ptrs[(pos-1)/2];
			pos = (pos-1)/2;
		}
		ptrs[pos] = p;
	}

	/* The heap is full, so something will have to be dropped */

	/* If the new pointer is smaller than the current minmum, drop it */
	if (p < ptrs[0])
		return;

	/* Replace the current min and heapify */
	// res = ptrs[0];
	ptrs[0] = p;
	pos = 0;

	while (1) {
		int left = 2 * pos + 1;
		int right = 2 * pos + 2;
		int largest = pos;
		if (left < heap->size && ptrs[left] < p)
			largest = left;
		if (right < heap->size && ptrs[right] < ptrs[largest])
			largest = right;
		if (largest == pos)
			break;
		/* Push p down the heap one level and bump one up */
		ptrs[pos] = ptrs[largest];
		ptrs[largest] = p;
		pos = largest;
	}
}