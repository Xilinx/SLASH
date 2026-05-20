/*
 * Token-form MODULE_IMPORT_NS probe.
 *
 * Pre-6.13:  MODULE_IMPORT_NS(ns) = MODULE_INFO(import_ns, __stringify(ns))
 *            -> token form is the documented usage.
 * 6.13+:     MODULE_IMPORT_NS(ns) = MODULE_INFO(import_ns, ns)
 *            -> token form fails to compile (DMA_BUF undefined).
 *
 * So this probe succeeds iff the token form is the right one to use.
 * Compile success on a pre-6.13 kernel that still accepts the string
 * form silently produces the wrong namespace string at runtime, which
 * is why we probe the token form (precise) instead of the string form
 * (ambiguous on older kernels).
 */
#include <linux/init.h>
#include <linux/module.h>

static int __init conftest_init(void)
{
    return 0;
}

static void __exit conftest_exit(void)
{
}

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(DMA_BUF);
module_init(conftest_init);
module_exit(conftest_exit);
