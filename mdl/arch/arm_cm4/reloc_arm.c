#include "arch_if.h"
#include "registry.h" /* module_t's text_lo/data_lo -- set by the loader before this runs */

/*
 * GOT-base relocation for -msingle-pic-base modules.
 *
 * Each GOT slot lives at data_lo + tbl[i].got_offset (the loader places
 * the module's whole GOT at the very start of the data arena region --
 * see loader.c's memory layout comment). The packer has already
 * pre-normalized the value STORED at that slot to be a pure offset from
 * whichever base tbl[i].kind names (see mdl_reloc_kind_t's comment in
 * mdl_format.h for why a slot can need either the text or the data
 * base -- verified empirically against real arm-none-eabi-gcc output,
 * not assumed). So relocating one slot really is just "add one base
 * address to what's already there" -- no symbol lookup, no relocation
 * type dispatch beyond the single kind bit.
 */
void arch_apply_relocs(module_t *m, const mdl_reloc_t *tbl, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t base = (tbl[i].kind == MDL_RELOC_TEXT_BASE)
                             ? (uint32_t)m->text_lo
                             : (uint32_t)m->data_lo;
        uint32_t *slot = (uint32_t *)(m->data_lo + tbl[i].got_offset);
        *slot += base;
    }
}
