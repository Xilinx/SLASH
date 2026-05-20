#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>

static int __init conftest_init(void)
{
    struct vm_area_struct *vma = NULL;

    vm_flags_set(vma, (vm_flags_t)0);
    return 0;
}

static void __exit conftest_exit(void)
{
}

MODULE_LICENSE("GPL");
module_init(conftest_init);
module_exit(conftest_exit);
