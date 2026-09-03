#include "arch_if.h"

/*
 * GOT-base relocation for -msingle-pic-base modules. Not implemented yet
 * -- lands in M1 alongside the loader and packer. Declared now so the
 * arch_if.h contract is fully satisfied and M0 links cleanly against it.
 */
void arch_apply_relocs(module_t *m, const mdl_reloc_t *tbl, size_t n)
{
    (void)m;
    (void)tbl;
    (void)n;
    /* TODO(M1): for i in [0, n), add the module's data-region base
     * address to the 32-bit GOT slot at data_base + tbl[i].got_offset.
     * See mdl_header_t.got_off/got_count (mdl_format.h) for how the
     * packer lays out the GOT this reads. */
}
