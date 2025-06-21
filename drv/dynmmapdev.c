#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>

MODULE_LICENSE("GPL");

#define DYNMMAPDEV_IOC_MAGIC 'd'
#define DYNMMAPDEV_IOC_UNMAP_PAGE _IOW(DYNMMAPDEV_IOC_MAGIC, 1, unsigned long)
struct ioctl_data {
    unsigned long addr;
    unsigned long offset;
};

#define DEVICE_NAME "dynmmapdev"
#define DEVICE_SIZE (16 * 4096) // 16ページ分仮想空間を作る（実際に物理は使わない）

static dev_t dev_number;               // ここに major, minor が入る
static struct cdev dynmmapdev_cdev;    // キャラクタデバイスオブジェクト
static struct class *dynmmapdev_class; // sysfs用
static DEFINE_XARRAY(pagemap);         // ユーザーアドレスとカーネルアドレスの対応

static long dynmmapdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct ioctl_data data;
    struct mm_struct *mm = current->mm;
    struct vm_area_struct *vma;
    unsigned long start, end;

    if (cmd != DYNMMAPDEV_IOC_UNMAP_PAGE)
        return -EINVAL;

    if (copy_from_user(&data, (void __user *)arg, sizeof(struct ioctl_data)))
        return -EFAULT;

    down_write(&mm->mmap_lock);

    vma = find_vma(mm, data.addr);
    if (!vma || data.addr < vma->vm_start) {
        up_write(&mm->mmap_lock);
        return -EINVAL;
    }

    unsigned long pfn = page_to_pfn(virt_to_page(data.addr));
    // phys_addr_t phys_addr = PFN_PHYS(pfn);

    // 削除対象の仮想ページ範囲（1ページ）
    start = data.addr & PAGE_MASK;
    end = start + PAGE_SIZE;

    pr_info("dynmmapdev: zap_vma_ptes 0x%lx - 0x%lx\n", start, end);

    // PTEの削除
    zap_vma_ptes(vma, start, PAGE_SIZE);
    zap_vma_ptes(vma, start + data.offset, PAGE_SIZE);
    pgprot_t prot = vm_get_page_prot(VM_READ | VM_SHARED);
    int ret = vmf_insert_pfn_prot(vma, data.addr + data.offset, pfn, prot);

    up_write(&mm->mmap_lock);
    return 0;
}

/* mmap対象のvma操作群 */
static vm_fault_t dynmmapdev_fault(struct vm_fault *vmf)
{
    struct page *page;
    void *page_addr;
    unsigned long offset = vmf->address - vmf->vma->vm_start;
    unsigned long page_index = offset >> PAGE_SHIFT;
    phys_addr_t phys_addr;

    if (offset >= DEVICE_SIZE)
        return VM_FAULT_SIGBUS; // 範囲外アクセスならSIGBUS

    page = xa_load(&pagemap, page_index);
    if (page) { // 以前の fault 処理でアドレスが割り当て済み
        pr_info("dynmmapdev: refault at offset=0x%lx -> reusing page %p\n",
                offset, page);
        get_page(page);  // refaultなので refcount++
        vmf->page = page;
        return 0;
    }

    pr_info("dynmmapdev: first fault at user_addr=0x%lx (offset=0x%lx)\n",
            vmf->address, offset);

    // 新しい1ページ分確保
    page = alloc_page(GFP_KERNEL);
    if (!page)
        return VM_FAULT_OOM;

    page_addr = page_address(page);
    phys_addr = virt_to_phys(page_addr);
    pr_info("dynmmapdev: allocated kernel_addr=0x%p phys_addr=0x%llx\n",
            page_addr, (unsigned long long)phys_addr);

    // 適当に内容を埋める（例：オフセットに応じた値を書き込む）
    memset(page_addr, page_index & 0xff, PAGE_SIZE);

    // ページ参照カウントを上げる
    get_page(page);

    // fault発生位置にマッピング
    xa_store(&pagemap, page_index, page, GFP_KERNEL);
    vmf->page = page;
    return 0;
}

static const struct vm_operations_struct dynmmap_vm_ops = {
    .fault = dynmmapdev_fault,
};

static int dynmmapdev_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int dynmmapdev_release(struct inode *inode, struct file *file)
{
    return 0;
}

static int dynmmapdev_mmap(struct file *file, struct vm_area_struct *vma)
{
    unsigned long size = vma->vm_end - vma->vm_start;

    if (size > DEVICE_SIZE)
        return -EINVAL;

    // start アドレスと、ユーザー空間で取得できるアドレスは同じ値になるはず
    pr_info("dynmmapdev: mmap requested: start=0x%lx end=0x%lx size=0x%lx\n",
        vma->vm_start, vma->vm_end, size);

    vma->vm_ops = &dynmmap_vm_ops;
    vm_flags_clear(vma, VM_IO);
    vm_flags_set(vma, VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);

    return 0;
}

static struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = dynmmapdev_open,
    .release = dynmmapdev_release,
    .mmap    = dynmmapdev_mmap,
    .unlocked_ioctl = dynmmapdev_ioctl,
};
 
static int __init dynmmapdev_init(void)
{
    int ret;

    // デバイス番号を動的に割り当ててもらう
    ret = alloc_chrdev_region(&dev_number, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("alloc_chrdev_region failed\n");
        return ret;
    }

    // キャラクタデバイスの初期化
    cdev_init(&dynmmapdev_cdev, &fops);
    dynmmapdev_cdev.owner = THIS_MODULE;

    // カーネルにデバイスを登録
    ret = cdev_add(&dynmmapdev_cdev, dev_number, 1);
    if (ret) {
        pr_err("cdev_add failed\n");
        unregister_chrdev_region(dev_number, 1);
        return ret;
    }

    // /dev/用のクラス作成
    dynmmapdev_class = class_create(DEVICE_NAME);
    if (IS_ERR(dynmmapdev_class)) {
        cdev_del(&dynmmapdev_cdev);
        unregister_chrdev_region(dev_number, 1);
        return PTR_ERR(dynmmapdev_class);
    }

    // /dev/エントリを作成
    if (device_create(dynmmapdev_class, NULL, dev_number, NULL, DEVICE_NAME) == NULL) {
        class_destroy(dynmmapdev_class);
        cdev_del(&dynmmapdev_cdev);
        unregister_chrdev_region(dev_number, 1);
        return -ENOMEM;
    }

    pr_info("dynmmapdev loaded: major=%d minor=%d\n", MAJOR(dev_number), MINOR(dev_number));
    return 0;
}

static void dynmmapdev_cleanup_pages(void)
{
    struct page *page;
    unsigned long index;

    xa_for_each(&pagemap, index, page) {
        pr_info("dynmmapdev: free page at index=%lu\n", index);
        put_page(page);
    }

    xa_destroy(&pagemap);
}

static void __exit dynmmapdev_exit(void)
{
    dynmmapdev_cleanup_pages();
    device_destroy(dynmmapdev_class, dev_number);
    class_destroy(dynmmapdev_class);
    cdev_del(&dynmmapdev_cdev);
    unregister_chrdev_region(dev_number, 1);

    pr_info("dynmmapdev unloaded\n");
}

module_init(dynmmapdev_init);
module_exit(dynmmapdev_exit);
