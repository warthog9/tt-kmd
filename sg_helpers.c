#include "sg_helpers.h"

#include <linux/kernel.h>

// -1 because the chain entry requires its own struct scatterlist, and
// for simplicity we reserve the last entry of every page for the chain.
// (But note that the end mark is on the last valid scatterlist entry.)
// On x86-64 this works out to 145.
#define SCL_PER_PAGE (PAGE_SIZE / sizeof(struct scatterlist) - 1)

// This is very similar to sg_alloc_table_from_pages, but we need to go big so
// we use single-page allocations and scatterlist chaining for unlimited scaling.
bool alloc_chained_sgt_for_pages(struct sg_table *table, struct page **pages, unsigned int n_pages)
{
	struct page **pages_end = pages + n_pages;

	struct scatterlist *current_scl = NULL; // last entry of previous page of scatterlists

	memset(table, 0, sizeof(*table));

	if (n_pages == 0)
		return true;

	while (pages < pages_end) {
		// Zeroed because sg_set_page preserves the page_link chain/end bits.
		struct page *new_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		struct scatterlist *page_first_scl;

		if (!new_page)
			goto out_free;

		// Attach the new page to the chain.
		page_first_scl = page_address(new_page);

		if (current_scl) {
			sg_chain(current_scl, 1, page_first_scl);
		} else {
			table->sgl = page_first_scl;
		}

		current_scl = page_first_scl;

		// Measure out contiguous page ranges and write each into a scatterlist entry in the
		// current scatterlist page.
		while (pages < pages_end && current_scl - page_first_scl < SCL_PER_PAGE) {
			struct page **contig_start = pages++;

			for (; pages < pages_end; pages++)
				if (page_to_pfn(pages[-1]) + 1 != page_to_pfn(pages[0]))
					break;

			sg_set_page(current_scl++, *contig_start, (pages - contig_start) * PAGE_SIZE, 0);
		}

		table->orig_nents = table->nents += current_scl - page_first_scl;

		// Note that current_scl points to the extra entry reserved for chaining.
		// Chaining entries are not included in nents. sg_next() just skips over them.
	}

	sg_mark_end(current_scl);
	return true;

out_free:
	free_chained_sgt(table);
	return false;
}

// Free a chained scatterlist created by make_chained_scl_for_pages.
// Doesn't check each scatterlist entry if it's chain/end, rather asssumes
// that there are always SCL_PER_PAGE except for the last page.
void free_chained_sgt(struct sg_table *table)
{
	struct scatterlist *next_page = table->sgl;
	unsigned int num_entries = table->nents;

	while (next_page) {
		struct scatterlist *current_page = next_page;

		if (num_entries > SCL_PER_PAGE) {
			// Not SCL_PER_PAGE-1 because we deducted one in the definition of SCL_PER_PAGE.
			// (The last entry of each page is reserved for chaining.)

			// assert(sg_is_chain(current_page[SCL_PER_PAGE]));
			next_page = sg_chain_ptr(&current_page[SCL_PER_PAGE]);
			num_entries -= SCL_PER_PAGE;
		} else {
			next_page = NULL;
		}

		__free_page(virt_to_page(current_page));
	}
}
