// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/version.h>
#include <linux/iommu.h>
#include <linux/file.h>
#include <linux/vmalloc.h>

#include "chardev_private.h"
#include "device.h"
#include "memory.h"
#include "ioctl.h"
#include "sg_helpers.h"

// In Linux 5.0, dma_alloc_coherent always zeroes memory and dma_zalloc_coherent
// was removed.
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
#define dma_alloc_coherent dma_zalloc_coherent
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
static int pin_user_pages_fast_longterm(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages)
{
	// vma array allocation removed in 52650c8b466bac399aec213c61d74bfe6f7af1a4.
	return pin_user_pages_fast(start, nr_pages, gup_flags | FOLL_LONGTERM, pages);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static int pin_user_pages_fast_longterm(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages)
{
	// Can't use pin_user_pages_fast(FOLL_LONGTERM) because it calls __gup_longterm_locked with vmas = NULL
	// which allocates a contiguous vmas array and that fails often.

	int ret;

	struct vm_area_struct **vmas = kvmalloc_array(nr_pages, sizeof(struct vm_area_struct *), GFP_KERNEL);
	if (vmas == NULL)
		return -ENOMEM;

	ret = pin_user_pages(start, nr_pages, gup_flags | FOLL_LONGTERM, pages, vmas);

	kvfree(vmas);
	return ret;
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
static int pin_user_pages_fast_longterm(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages)
{
	// Can't use get_user_pages_fast(FOLL_LONGTERM) because it calls __gup_longterm_locked with vmas = NULL
	// which allocates a contiguous vmas array and that fails often.

	int ret;

	struct vm_area_struct **vmas = kvmalloc_array(nr_pages, sizeof(struct vm_area_struct *), GFP_KERNEL);
	if (vmas == NULL)
		return -ENOMEM;

	ret = get_user_pages(start, nr_pages, gup_flags | FOLL_LONGTERM, pages, vmas);

	kvfree(vmas);
	return ret;
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 4)
static int pin_user_pages_fast_longterm(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages)
{
	int ret;

	// If we don't pass in vmas, get_user_pages_longterm will allocate it in contiguous memory and that fails often.
	struct vm_area_struct **vmas = kvmalloc_array(nr_pages, sizeof(struct vm_area_struct *), GFP_KERNEL);
	if (vmas == NULL)
		return -ENOMEM;

	down_read(&current->mm->mmap_sem);
	ret = get_user_pages_longterm(start, nr_pages, gup_flags, pages, vmas);
	up_read(&current->mm->mmap_sem);

	kvfree(vmas);
	return ret;
}
#else
static int pin_user_pages_fast_longterm(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages)
{
	// Kernels this old don't know about long-term pinning, so they don't allocate the vmas array.
	return get_user_pages_fast(start, nr_pages, gup_flags, pages);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
// unpin_user_pages_dirty_lock is provided by the kernel.
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
static void unpin_user_pages_dirty_lock(struct page **pages, unsigned long npages, bool make_dirty)
{
	put_user_pages_dirty_lock(pages, npages, make_dirty);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
static void unpin_user_pages_dirty_lock(struct page **pages, unsigned long npages, bool make_dirty)
{
	if (make_dirty)
		put_user_pages_dirty_lock(pages, npages);
	else
		put_user_pages(pages, npages);
}
#else
static void unpin_user_pages_dirty_lock(struct page **pages, unsigned long npages, bool make_dirty)
{
	struct page **end = pages + npages;
	for (; pages != end; pages++) {
		if (make_dirty)
			set_page_dirty_lock(*pages);
		put_page(*pages);
	}
}
#endif

#define MAX_DMA_BUF_SIZE (1u << MAX_DMA_BUF_SIZE_LOG2)

// These are the mmap offsets for various resources. In the user-kernel
// interface they are dynamic (TENSTORRENT_IOCTL_QUERY_MAPPINGS and
// TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF), but they are actually hard-coded.
#define MMAP_OFFSET_RESOURCE0_UC	(U64_C(0) << 32)
#define MMAP_OFFSET_RESOURCE0_WC	(U64_C(1) << 32)
#define MMAP_OFFSET_RESOURCE1_UC	(U64_C(2) << 32)
#define MMAP_OFFSET_RESOURCE1_WC	(U64_C(3) << 32)
#define MMAP_OFFSET_RESOURCE2_UC	(U64_C(4) << 32)
#define MMAP_OFFSET_RESOURCE2_WC	(U64_C(5) << 32)

// tenstorrent_allocate_dma_buf_in.buf_index is u8 so that sets a limit of
// U8_MAX DMA buffers per fd. 32-bit mmap offsets are divided by PAGE_SIZE,
// so PAGE_SIZE << 32 is the largest possible offset.
#define MMAP_OFFSET_DMA_BUF		((u64)(PAGE_SIZE-U8_MAX) << 32)

#define MMAP_SIZE_DMA_BUF (U64_C(1) << 32)

struct pinned_page_range {
	struct list_head list;

	unsigned long page_count;
	struct page **pages;	// vmalloc/vfree

	struct sg_table dma_mapping;	// alloc_chained_sgt_for_pages / free_chained_sgt
};

struct peer_resource_mapping {
	struct list_head list;

	dma_addr_t mapped_address;
	size_t size;
};

// This replaces tenstorrent_query_mappings from ioctl.h with a version
// that uses a flexible array member rather than a zero-length array.
// This keeps UBSAN from triggering when we write the output mappings.
struct tenstorrent_query_mappings_flex {
	struct tenstorrent_query_mappings_in in;
	struct tenstorrent_mapping out_mappings[];
};

long ioctl_query_mappings(struct chardev_private *priv,
			  struct tenstorrent_query_mappings __user *arg_)
{
	struct tenstorrent_query_mappings_flex __user *arg = (struct tenstorrent_query_mappings_flex __user *)arg_;

	struct tenstorrent_mapping mappings[6];
	struct tenstorrent_mapping *next_mapping;

	u32 valid_mappings_to_copy;
	u32 extra_mappings_to_clear;
	u32 valid_mappings;

	resource_size_t resource_len;

	struct tenstorrent_query_mappings_in in;
	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	memset(mappings, 0, sizeof(mappings));
	next_mapping = mappings;

	resource_len = pci_resource_len(priv->device->pdev, 0);
	if (resource_len > 0) {
		next_mapping->mapping_id = TENSTORRENT_MAPPING_RESOURCE0_UC;
		next_mapping->mapping_base = MMAP_OFFSET_RESOURCE0_UC;
		next_mapping->mapping_size = resource_len;
		next_mapping++;

		next_mapping->mapping_id = TENSTORRENT_MAPPING_RESOURCE0_WC;
		next_mapping->mapping_base = MMAP_OFFSET_RESOURCE0_WC;
		next_mapping->mapping_size = resource_len;
		next_mapping++;
	}

	resource_len = pci_resource_len(priv->device->pdev, 2);
	if (resource_len > 0) {
		next_mapping->mapping_id = TENSTORRENT_MAPPING_RESOURCE1_UC;
		next_mapping->mapping_base = MMAP_OFFSET_RESOURCE1_UC;
		next_mapping->mapping_size = resource_len;
		next_mapping++;

		next_mapping->mapping_id = TENSTORRENT_MAPPING_RESOURCE1_WC;
		next_mapping->mapping_base = MMAP_OFFSET_RESOURCE1_WC;
		next_mapping->mapping_size = resource_len;
		next_mapping++;
	}

	resource_len = pci_resource_len(priv->device->pdev, 4);
	if (resource_len > 0) {
		next_mapping->mapping_id = TENSTORRENT_MAPPING_RESOURCE2_UC;
		next_mapping->mapping_base = MMAP_OFFSET_RESOURCE2_UC;
		next_mapping->mapping_size = resource_len;
		next_mapping++;

		next_mapping->mapping_id = TENSTORRENT_MAPPING_RESOURCE2_WC;
		next_mapping->mapping_base = MMAP_OFFSET_RESOURCE2_WC;
		next_mapping->mapping_size = resource_len;
		next_mapping++;
	}

	valid_mappings = next_mapping - mappings;

	valid_mappings_to_copy = min(in.output_mapping_count, valid_mappings);
	extra_mappings_to_clear = (in.output_mapping_count > valid_mappings)
		? in.output_mapping_count - valid_mappings : 0;

	if (U32_MAX / sizeof(struct tenstorrent_mapping) < extra_mappings_to_clear)
		return -EFAULT;

	if (copy_to_user(&arg->out_mappings, &mappings,
			 valid_mappings_to_copy * sizeof(struct tenstorrent_mapping)))
		return -EFAULT;

	if (clear_user(&arg->out_mappings[valid_mappings_to_copy],
		       extra_mappings_to_clear * sizeof(struct tenstorrent_mapping)))
		return -EFAULT;

	return 0;
}

static struct dmabuf *lookup_dmabuf_by_index(struct chardev_private *priv, u8 buf_index) {
	struct dmabuf *dmabuf;

	hash_for_each_possible(priv->dmabufs, dmabuf, hash_chain, buf_index)
		if (dmabuf->index == buf_index)
			return dmabuf;

	return NULL;
}

static u64 dmabuf_mapping_start(u8 buf_index) {
	return MMAP_OFFSET_DMA_BUF + buf_index * MMAP_SIZE_DMA_BUF;
}

long ioctl_allocate_dma_buf(struct chardev_private *priv,
			    struct tenstorrent_allocate_dma_buf __user *arg)
{
	dma_addr_t dma_handle = 0;
	void *dma_buf_kernel_ptr;
	struct dmabuf *dmabuf;
	long ret = 0;

	struct tenstorrent_allocate_dma_buf_in in;
	struct tenstorrent_allocate_dma_buf_out out;
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	if (!priv->device->dma_capable)
		return -EINVAL;

	if (in.buf_index >= TENSTORRENT_MAX_DMA_BUFS)
		return -EINVAL;

	if (in.requested_size % PAGE_SIZE != 0
	    || in.requested_size == 0
	    || in.requested_size > MAX_DMA_BUF_SIZE)
		return -EINVAL;

	mutex_lock(&priv->mutex);

	if (lookup_dmabuf_by_index(priv, in.buf_index)) {
		ret = -EINVAL;
		goto out;
	}

	dmabuf = kzalloc(sizeof(*dmabuf), GFP_KERNEL);
	if (!dmabuf) {
		ret = -ENOMEM;
		goto out;
	}

	dma_buf_kernel_ptr = dma_alloc_coherent(&priv->device->pdev->dev,
						in.requested_size,
						&dma_handle, GFP_KERNEL);

	if (dma_buf_kernel_ptr == NULL) {
		kfree(dmabuf);
		ret = -ENOMEM;
		goto out;
	}

	dmabuf->index = in.buf_index;
	dmabuf->ptr = dma_buf_kernel_ptr;
	dmabuf->phys = dma_handle;
	dmabuf->size = in.requested_size;

	out.physical_address = (u64)dmabuf->phys;
	out.mapping_offset = dmabuf_mapping_start(in.buf_index);
	out.size = in.requested_size;

	if (copy_to_user(&arg->out, &out, sizeof(out)) != 0) {
		dma_free_coherent(&priv->device->pdev->dev, dmabuf->size,
				  dmabuf->ptr, dmabuf->phys);

		kfree(dmabuf);
		ret = -EFAULT;
		goto out;
	}

	hash_add(priv->dmabufs, &dmabuf->hash_chain, dmabuf->index);

out:
	mutex_unlock(&priv->mutex);
	return ret;
}

long ioctl_free_dma_buf(struct chardev_private *priv,
			struct tenstorrent_free_dma_buf __user *arg)
{
	// This is unsupported until I figure out how to block freeing as long
	// as a mapping exists. Otherwise the dma buffer is freed when the
	// struct file is destroyed, and that's safe because the mapping
	// refcounts the file.
	return -EINVAL;
}


static bool is_iommu_translated(struct device *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	return domain && domain->type != IOMMU_DOMAIN_IDENTITY;
}

static bool is_pin_pages_size_safe(u64 size)
{
	// With IOMMU enabled on 5.4, 2GB pinnings may succeed, but then soft lockup on process exit.
	// (tt_cdev_release -> unmap_sg -> __unmap_single -> iommu_unmap_page)
	// This doesn't happen in 5.15, but I don't know exactly when it was fixed.
#if LINUX_VERSION_CODE <= KERNEL_VERSION(5, 4, 0)
	return size <= 1 << 30;
#else
	return true;
#endif
}

long ioctl_pin_pages(struct chardev_private *priv,
		     struct tenstorrent_pin_pages __user *arg)
{
	unsigned long nr_pages;
	struct page **pages;
	int pages_pinned;
	struct pinned_page_range *pinning;
	struct sg_table dma_mapping = {0};
	long ret;
	u32 bytes_to_copy;

	struct tenstorrent_pin_pages_in in;
	struct tenstorrent_pin_pages_out out;
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	if (!PAGE_ALIGNED(in.virtual_address) || !PAGE_ALIGNED(in.size) || in.size == 0)
		return -EINVAL;

	if (!is_pin_pages_size_safe(in.size))
		return -EINVAL;

	if (in.flags != 0 && in.flags != TENSTORRENT_PIN_PAGES_CONTIGUOUS)
		return -EINVAL;

	pinning = kmalloc(sizeof(*pinning), GFP_KERNEL);
	if (!pinning)
		return -ENOMEM;

	nr_pages = PAGE_ALIGN(in.size) >> PAGE_SHIFT;
	pages = vzalloc(nr_pages * sizeof(struct page *));
	if (!pages) {
		pr_err("vzalloc failed for %lu page pointers\n", nr_pages);
		ret = -ENOMEM;
		goto err_free_pinning;
	}

	pages_pinned = pin_user_pages_fast_longterm(in.virtual_address, nr_pages, FOLL_WRITE, pages);
	if (pages_pinned < 0) {
		pr_warn("pin_user_pages_longterm failed: %d\n", pages_pinned);
		ret = pages_pinned;
		goto err_vfree_pages;
	}

	if (pages_pinned != nr_pages) {
		pr_err("could only pin %d of %lu pages\n", pages_pinned, nr_pages);
		ret = -EINVAL;
		goto err_unpin_pages;
	}

	if (is_iommu_translated(&priv->device->pdev->dev)) {
		struct scatterlist *sg;
		unsigned int i;
		dma_addr_t expected_next_address;
		unsigned long total_dma_len = 0;

		if (!alloc_chained_sgt_for_pages(&dma_mapping, pages, nr_pages)) {
			pr_warn("alloc_chained_sgt_for_pages failed for %lu pages, probably out of memory.\n", nr_pages);
			ret = -ENOMEM;
			goto err_unpin_pages;
		}

		mutex_lock(&priv->mutex);

		ret = dma_map_sgtable(&priv->device->pdev->dev, &dma_mapping, DMA_BIDIRECTIONAL, 0);

		if (ret != 0) {
			pr_err("dma_map_sg failed.\n");
			goto err_unlock_priv;
		}

		// This can only happen due to a misconfiguration or a bug.
		for_each_sgtable_dma_sg((&dma_mapping), sg, i) {
			if (i > 0 && sg_dma_address(sg) != expected_next_address) {
				pr_err("discontiguous mapping\n");
				ret = -EINVAL;
			}

			expected_next_address = sg_dma_address(sg) + sg_dma_len(sg);
			total_dma_len += sg_dma_len(sg);
		}

		if (total_dma_len != nr_pages * PAGE_SIZE) {
			pr_err("dma-mapped (%lX) != original length (%lX).\n", total_dma_len, nr_pages * PAGE_SIZE);
			ret = -EINVAL;
		}

		if (ret != 0) {
			debug_print_sgtable(&dma_mapping);
			goto err_dma_unmap;
		}

		out.physical_address = sg_dma_address(dma_mapping.sgl);
	} else {
		int i;

		for (i = 1; i < pages_pinned; i++) {
			if (page_to_pfn(pages[i]) != page_to_pfn(pages[i-1]) + 1) {
				pr_err("pages discontiguous at %d\n", i);
				ret = -EINVAL;
				goto err_unpin_pages;
			}
		}

		out.physical_address = page_to_phys(pages[0]);

		mutex_lock(&priv->mutex);
	}

	pinning->page_count = nr_pages;
	pinning->pages = pages;
	pinning->dma_mapping = dma_mapping;

	list_add(&pinning->list, &priv->pinnings);
	mutex_unlock(&priv->mutex);

	if (clear_user(&arg->out, in.output_size_bytes) != 0)
		return -EFAULT;

	bytes_to_copy = min(in.output_size_bytes, (u32)sizeof(out));

	if (copy_to_user(&arg->out, &out, bytes_to_copy) != 0)
		return -EFAULT;

	return 0;

err_dma_unmap:
	dma_unmap_sgtable(&priv->device->pdev->dev, &dma_mapping, DMA_BIDIRECTIONAL, 0);
err_unlock_priv:
	free_chained_sgt(&dma_mapping);
	mutex_unlock(&priv->mutex);
err_unpin_pages:
	unpin_user_pages_dirty_lock(pages, pages_pinned, false);
err_vfree_pages:
	vfree(pages);
err_free_pinning:
	kfree(pinning);
	return ret;
}

long ioctl_map_peer_bar(struct chardev_private *priv,
			struct tenstorrent_map_peer_bar __user *arg) {

	struct file *peer_file;
	struct chardev_private *peer_priv;
	struct peer_resource_mapping *peer_mapping;
	resource_size_t resource_len;
	phys_addr_t phys_addr;
	dma_addr_t mapping;
	int ret;

	struct tenstorrent_map_peer_bar_in in;
	struct tenstorrent_map_peer_bar_out out;

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	if (in.flags != 0)
		return -EINVAL;

	if (in.peer_bar_index >= PCI_NUM_RESOURCES)
		return -EINVAL;

	if (in.peer_bar_length == 0)
		return -EINVAL;

	peer_file = fget(in.peer_fd);
	if (!peer_file)
		return -EBADF;

	peer_priv = get_tenstorrent_priv(peer_file);
	if (!peer_priv) {
		ret = -EINVAL;
		goto err_fput;
	}

	if (peer_priv->device == priv->device) {
		ret = -EINVAL;
		goto err_fput;
	}

	if (peer_priv->device->dev_class != priv->device->dev_class) {
		ret = -EINVAL;
		goto err_fput;
	}

	peer_mapping = kmalloc(sizeof(*peer_mapping), GFP_KERNEL);
	if (!peer_mapping) {
		ret = -ENOMEM;
		goto err_fput;
	}

	// Avoid deadlocks on concurrent calls to IOCTL_MAP_PEER_BAR
	// by locking in a globally-consistent order.
	if (priv->device < peer_priv->device) {
		mutex_lock(&priv->mutex);
		mutex_lock(&peer_priv->mutex);
	} else {
		mutex_lock(&peer_priv->mutex);
		mutex_lock(&priv->mutex);
	}

	resource_len = pci_resource_len(peer_priv->device->pdev, in.peer_bar_index);
	if (in.peer_bar_offset >= resource_len || in.peer_bar_length > resource_len - in.peer_bar_offset) {
		ret = -EINVAL;
		goto err_unlock;
	}

	phys_addr = pci_resource_start(peer_priv->device->pdev, in.peer_bar_index) + in.peer_bar_offset;

	mapping = dma_map_resource(&priv->device->pdev->dev, phys_addr, in.peer_bar_length, DMA_BIDIRECTIONAL, 0);
	ret = dma_mapping_error(&priv->device->pdev->dev, mapping);
	if (ret != 0)
		goto err_unlock;

	peer_mapping->mapped_address = mapping;
	peer_mapping->size = in.peer_bar_length;

	list_add(&peer_mapping->list, &priv->peer_mappings);

	mutex_unlock(&priv->mutex);
	mutex_unlock(&peer_priv->mutex);

	fput(peer_file);

	out.dma_address = mapping;

	if (copy_to_user(&arg->out, &out, sizeof(out)) != 0)
		return -EFAULT;

	return 0;

err_unlock:
	mutex_unlock(&priv->mutex);
	mutex_unlock(&peer_priv->mutex);

	kfree(peer_mapping);

err_fput:
	fput(peer_file);

	return ret;
}

// Is the mapping target range contained entirely with start - start+len?
// start and len must be page-aligned.
static bool vma_target_range(struct vm_area_struct *vma, u64 start, resource_size_t len)
{
	unsigned long mapping_len_pg = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	unsigned long mapping_end_pg = vma->vm_pgoff + mapping_len_pg;

	if (vma->vm_pgoff >= start >> PAGE_SHIFT
	    && mapping_end_pg <= (start + len) >> PAGE_SHIFT) {
		vma->vm_pgoff -= start >> PAGE_SHIFT;
		return true;
	} else {
		return false;
	}
}

static struct dmabuf *vma_dmabuf_target(struct chardev_private *priv,
					struct vm_area_struct *vma) {
	unsigned long dmabuf_index;
	struct dmabuf *dmabuf;

	if (vma->vm_pgoff < MMAP_OFFSET_DMA_BUF >> PAGE_SHIFT)
		// Not in DMA buffer offset range (too low).
		return NULL;

	dmabuf_index = (vma->vm_pgoff - (MMAP_OFFSET_DMA_BUF >> PAGE_SHIFT)) / (MMAP_SIZE_DMA_BUF >> PAGE_SHIFT);
	if (dmabuf_index >= TENSTORRENT_MAX_DMA_BUFS)
		// Not in DMA buffer offset range (too high).
		return NULL;

	dmabuf = lookup_dmabuf_by_index(priv, dmabuf_index);
	if (!dmabuf)
		// No allocated DMA buffer for that index.
		return NULL;

	if (vma_target_range(vma, dmabuf_mapping_start(dmabuf_index), dmabuf->size))
		return dmabuf;
	else
		// Allocated DMA buffer does not cover requested size.
		return NULL;
}

static int map_pci_bar(struct pci_dev *pdev, struct vm_area_struct *vma, unsigned int bar)
{
	resource_size_t bar_start = pci_resource_start(pdev, bar);
	resource_size_t bar_len = pci_resource_len(pdev, bar);

	return vm_iomap_memory(vma, bar_start, bar_len);
}

int tenstorrent_mmap(struct chardev_private *priv, struct vm_area_struct *vma)
{
	struct pci_dev *pdev = priv->device->pdev;

	// We multiplex various mappable entities into a single character
	// device using the mapping offset to determine which entity you get.
	// Each mapping must be contained within a single entity.
	// - PCI BAR 0/2/4 uncacheable mapping
	// - PCI BAR 0/2/4 write-combining mapping
	// - DMA buffer mapping

	if (vma_target_range(vma, MMAP_OFFSET_RESOURCE0_UC, pci_resource_len(pdev, 0))) {
		vma->vm_page_prot = pgprot_device(vma->vm_page_prot);
		return map_pci_bar(pdev, vma, 0);

	} else if (vma_target_range(vma, MMAP_OFFSET_RESOURCE0_WC, pci_resource_len(pdev, 0))) {
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		return map_pci_bar(pdev, vma, 0);

	} else if (vma_target_range(vma, MMAP_OFFSET_RESOURCE1_UC, pci_resource_len(pdev, 2))) {
		vma->vm_page_prot = pgprot_device(vma->vm_page_prot);
		return map_pci_bar(pdev, vma, 2);

	} else if (vma_target_range(vma, MMAP_OFFSET_RESOURCE1_WC, pci_resource_len(pdev, 2))) {
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		return map_pci_bar(pdev, vma, 2);

	} else if (vma_target_range(vma, MMAP_OFFSET_RESOURCE2_UC, pci_resource_len(pdev, 4))) {
		vma->vm_page_prot = pgprot_device(vma->vm_page_prot);
		return map_pci_bar(pdev, vma, 4);

	} else if (vma_target_range(vma, MMAP_OFFSET_RESOURCE2_WC, pci_resource_len(pdev, 4))) {
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		return map_pci_bar(pdev, vma, 4);

	} else {
		struct dmabuf *dmabuf = vma_dmabuf_target(priv, vma);
		if (dmabuf != NULL)
			return dma_mmap_coherent(&pdev->dev, vma, dmabuf->ptr,
						 dmabuf->phys, dmabuf->size);
		else
			return -EINVAL;
	}
}

void tenstorrent_memory_cleanup(struct chardev_private *priv)
{
	struct tenstorrent_device *tt_dev = priv->device;
	struct pinned_page_range *pinning, *tmp_pinning;
	struct hlist_node *tmp_dmabuf;
	struct dmabuf *dmabuf;
	unsigned int i;
	struct peer_resource_mapping *peer_mapping, *tmp_peer_mapping;

	hash_for_each_safe(priv->dmabufs, i, tmp_dmabuf, dmabuf, hash_chain) {
		dma_free_coherent(&tt_dev->pdev->dev, dmabuf->size, dmabuf->ptr, dmabuf->phys);

		hash_del(&dmabuf->hash_chain);
		kfree(dmabuf);
	}

	list_for_each_entry_safe(pinning, tmp_pinning, &priv->pinnings, list) {
		dma_unmap_sgtable(&priv->device->pdev->dev, &pinning->dma_mapping, DMA_BIDIRECTIONAL, 0);
		free_chained_sgt(&pinning->dma_mapping);

		unpin_user_pages_dirty_lock(pinning->pages, pinning->page_count, true);
		vfree(pinning->pages);

		list_del(&pinning->list);
		kfree(pinning);
	}

	list_for_each_entry_safe(peer_mapping, tmp_peer_mapping, &priv->peer_mappings, list) {
		dma_unmap_resource(&priv->device->pdev->dev, peer_mapping->mapped_address, peer_mapping->size, DMA_BIDIRECTIONAL, 0);

		list_del(&peer_mapping->list);
		kfree(peer_mapping);
	}
}
