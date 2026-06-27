// SPDX-License-Identifier: GPL-2.0
/*
 * pnm_module.c - PNMCompactor MMIO driver for gem5 full-system simulation.
 *
 * Exposes /dev/pnm so that userspace can mmap the PNMCompactor SimObject's
 * MMIO region (physical 0xD0000000, 256 bytes) directly, without needing
 * /dev/mem. The physical-to-virtual mapping is done lazily inside pnm_mmap()
 * via remap_pfn_range — no ioremap is performed at module init.
 *
 * Usage from guest userspace:
 *   insmod /root/pnm_module.ko
 *   chmod 666 /dev/pnm          # or run as root
 *   (rocksdb_pnm then accesses PNM registers via /dev/pnm)
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>

#define PNM_PHYS_BASE  0xD0000000UL
#define DEVICE_NAME    "pnm"

static dev_t          pnm_devno;
static struct cdev    pnm_cdev;
static struct class  *pnm_class;

/*
 * mmap: map the PNM physical MMIO region into the caller's address space.
 * Offset must be 0; size is whatever userspace requests (up to PAGE_SIZE).
 * The MMIO region is 256 bytes but the kernel rounds all mappings to page
 * granularity, so userspace will always receive a full 4 KiB page.
 */
static int pnm_mmap(struct file *file, struct vm_area_struct *vma)
{
    unsigned long size = vma->vm_end - vma->vm_start;

    if (vma->vm_pgoff != 0)
        return -EINVAL;
    /* mmap rounds length up to PAGE_SIZE; allow up to one page */
    if (size > PAGE_SIZE)
        return -EINVAL;

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

    return remap_pfn_range(vma, vma->vm_start,
                   PNM_PHYS_BASE >> PAGE_SHIFT,
                   size, vma->vm_page_prot);
}

static const struct file_operations pnm_fops = {
    .owner = THIS_MODULE,
    .mmap  = pnm_mmap,
};

/* Register the char device and create /dev/pnm. */
static int __init pnm_init(void)
{
    int ret;
    struct device *dev;

    ret = alloc_chrdev_region(&pnm_devno, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("pnm: alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    cdev_init(&pnm_cdev, &pnm_fops);
    pnm_cdev.owner = THIS_MODULE;
    ret = cdev_add(&pnm_cdev, pnm_devno, 1);
    if (ret < 0) {
        pr_err("pnm: cdev_add failed: %d\n", ret);
        goto err_chrdev;
    }

    pnm_class = class_create(DEVICE_NAME);
    if (IS_ERR(pnm_class)) {
        ret = PTR_ERR(pnm_class);
        pr_err("pnm: class_create failed: %d\n", ret);
        goto err_cdev;
    }

    dev = device_create(pnm_class, NULL, pnm_devno, NULL, DEVICE_NAME);
    if (IS_ERR(dev)) {
        ret = PTR_ERR(dev);
        pr_err("pnm: device_create failed: %d\n", ret);
        goto err_class;
    }

    pr_info("pnm: /dev/pnm ready (MMIO 0x%lx)\n", PNM_PHYS_BASE);
    return 0;

err_class:
    class_destroy(pnm_class);
err_cdev:
    cdev_del(&pnm_cdev);
err_chrdev:
    unregister_chrdev_region(pnm_devno, 1);
    return ret;
}

/* Remove /dev/pnm and unregister the char device. */
static void __exit pnm_exit(void)
{
    device_destroy(pnm_class, pnm_devno);
    class_destroy(pnm_class);
    cdev_del(&pnm_cdev);
    unregister_chrdev_region(pnm_devno, 1);
    pr_info("pnm: unloaded\n");
}

module_init(pnm_init);
module_exit(pnm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yonsei Research");
MODULE_DESCRIPTION("PNMCompactor MMIO driver for gem5");
