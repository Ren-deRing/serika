#include <serika/compiler.h>
#include <serika/symbol.h>

notrace const struct ksymbol *ksymbol_find(uintptr_t addr) {
    const struct ksymbol *best = NULL;

    for (size_t i = 0; i < ksymbol_count; i++) {
        const struct ksymbol *sym = &ksymbols[i];

        if (sym->addr > addr)
            break;

        best = sym;
    }

    return best;
}

notrace const char *ksymbol_name(const struct ksymbol *sym) {
    if (!sym)
        return NULL;

    return ksymbol_names + sym->name_offset;
}
