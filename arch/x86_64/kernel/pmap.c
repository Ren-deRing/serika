#include <serika/buddy.h>
#include <serika/page.h>
#include <serika/pmap.h>
#include <serika/slab.h>
#include <serika/string.h>

#include <asm/arch.h>

typedef uint64_t pml4e_t;
typedef uint64_t pdpte_t;
typedef uint64_t pde_t;
typedef uint64_t pte_t;

#define PML4_INDEX(va) (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)   (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)   (((va) >> 12) & 0x1FF)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PDE_ADDR_MASK 0x000FFFFFFFE00000ULL

#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_PWT      (1ULL << 3)
#define PTE_PCD      (1ULL << 4)
#define PTE_PAT      (1ULL << 7)
#define PTE_GLOBAL   (1ULL << 8)
#define PTE_NX       (1ULL << 63)

/* for PDPTE / PDE */
#define PDPTE_PS     (1ULL << 7)
#define PDE_PS       (1ULL << 7)
#define PDE_PAT      (1ULL << 12)

static bool is_table_empty(uint64_t *table) {
    for (int i = 0; i < 512; i++) {
        if (table[i] & PTE_PRESENT) return false;
    }

    return true;
}

static struct pmap g_kernel_pmap;

struct pmap *pmap_kernel(void) {
    return &g_kernel_pmap;
}

struct pmap *pmap_create(void) {
    struct pmap *pmap = kmalloc(sizeof(struct pmap));
    if (!pmap) return NULL;

    uintptr_t pml4_phys = (uintptr_t)alloc_pages(0);
    if (!pml4_phys) {
        kfree(pmap);
        return NULL;
    }

    pml4e_t *pml4_virt = (pml4e_t *)P2V(pml4_phys);
    memset(pml4_virt, 0, PAGE_SIZE);
    spin_lock_init(&pmap->lock);

    uint64_t f = spin_lock_irqsave(&g_kernel_pmap.lock);
    memcpy(&pml4_virt[256], &((pml4e_t *)g_kernel_pmap.pm_root_virt)[256],
           256 * sizeof(pml4e_t));
    spin_unlock_irqrestore(&g_kernel_pmap.lock, f);

    pmap->pm_root_phys = pml4_phys;
    pmap->pm_root_virt = pml4_virt;

    return pmap;
}

static int pmap_map_page(struct pmap *pmap, uintptr_t va, uintptr_t pa, uint32_t flags) {
    pml4e_t *pml4 = (pml4e_t *)pmap->pm_root_virt;
    uint64_t table_flags = PTE_PRESENT | PTE_WRITABLE;
    uintptr_t new_pdpt_phys = 0;
    uintptr_t new_pd_phys = 0;
    bool new_pdpt = false;
    bool new_pd = false;

    if (flags & PROT_USER) table_flags |= PTE_USER;

    /* PML4 -> PDPT */
    size_t idx = PML4_INDEX(va);
    if (!(pml4[idx] & PTE_PRESENT)) {
        new_pdpt_phys = (uintptr_t)alloc_pages(0);
        if (!new_pdpt_phys) return -1;

        memset((void *)P2V(new_pdpt_phys), 0, PAGE_SIZE);
        pml4[idx] = new_pdpt_phys | table_flags;
        new_pdpt = true;
    } else {
        pml4[idx] |= PTE_WRITABLE;
        if (flags & PROT_USER) pml4[idx] |= PTE_USER;
    }

    pdpte_t *pdpt = (pdpte_t *)P2V(pml4[idx] & PTE_ADDR_MASK);

    /* PDPT -> PD */
    idx = PDPT_INDEX(va);
    if (!(pdpt[idx] & PTE_PRESENT)) {
        new_pd_phys = (uintptr_t)alloc_pages(0);
        if (!new_pd_phys) {
            if (new_pdpt) {
                pml4[PML4_INDEX(va)] = 0;
                free_pages((void *)new_pdpt_phys, 0);
            }
            return -1;
        }

        memset((void *)P2V(new_pd_phys), 0, PAGE_SIZE);
        pdpt[idx] = new_pd_phys | table_flags;
        new_pd = true;
    } else {
        if (pdpt[idx] & PDPTE_PS) return -1;

        pdpt[idx] |= PTE_WRITABLE;
        if (flags & PROT_USER) pdpt[idx] |= PTE_USER;
    }

    pde_t *pd = (pde_t *)P2V(pdpt[idx] & PTE_ADDR_MASK);

    uint64_t pte_flags = PTE_PRESENT;

    if (flags & PROT_WRITE) pte_flags |= PTE_WRITABLE;
    if (flags & PROT_USER) pte_flags |= PTE_USER;
    if (flags & PROT_GLOBAL) pte_flags |= PTE_GLOBAL;
    if (!(flags & PROT_EXEC)) pte_flags |= PTE_NX;

    /* HUGE page */
    if (flags & PROT_HUGE) {
        idx = PD_INDEX(va);

        if (pd[idx] & PTE_PRESENT) return -1;

        if (flags & PROT_WC) pte_flags |= PDE_PAT | PTE_PWT;

        pd[idx] = (pa & PDE_ADDR_MASK) | pte_flags | PDE_PS;
        return 0;
    }

    /* Normal page */
    idx = PD_INDEX(va);

    if (pd[idx] & PDE_PS) return -1;

    if (!(pd[idx] & PTE_PRESENT)) {
        uintptr_t pt_phys = (uintptr_t)alloc_pages(0);
        if (!pt_phys) {
            if (new_pd) {
                pdpt[PDPT_INDEX(va)] = 0;
                free_pages((void *)new_pd_phys, 0);
            }

            if (new_pdpt) {
                pml4[PML4_INDEX(va)] = 0;
                free_pages((void *)new_pdpt_phys, 0);
            }

            return -1;
        }

        memset((void *)P2V(pt_phys), 0, PAGE_SIZE);
        pd[idx] = pt_phys | table_flags;
    } else {
        pd[idx] |= PTE_WRITABLE;
        if (flags & PROT_USER) pd[idx] |= PTE_USER;
    }

    pte_t *pt = (pte_t *)P2V(pd[idx] & PTE_ADDR_MASK);

    idx = PT_INDEX(va);
    if (pt[idx] & PTE_PRESENT) return -1;

    if (flags & PROT_WC) pte_flags |= PTE_PAT | PTE_PWT;

    pt[idx] = (pa & PTE_ADDR_MASK) | pte_flags;

    return 0;
}

static size_t pmap_unmap_page(struct pmap *pmap, uintptr_t va, uintptr_t end, bool *changed) {
    pml4e_t *pml4 = (pml4e_t *)pmap->pm_root_virt;

    /* PML4 -> PDPT */
    size_t l4 = PML4_INDEX(va);
    if (!(pml4[l4] & PTE_PRESENT)) return PAGE_SIZE;

    pdpte_t *pdpt = (pdpte_t *)P2V(pml4[l4] & PTE_ADDR_MASK);

    /* PDPT -> PD */
    size_t l3 = PDPT_INDEX(va);
    if (!(pdpt[l3] & PTE_PRESENT) || (pdpt[l3] & PDPTE_PS)) return 0;

    pde_t *pd = (pde_t *)P2V(pdpt[l3] & PTE_ADDR_MASK);

    /* HUGE page */
    size_t l2 = PD_INDEX(va);
    if (!(pd[l2] & PTE_PRESENT)) return PAGE_SIZE;

    if (pd[l2] & PDE_PS) {
        if (va & (PAGE_SIZE_HUGE - 1)) return 0;
        if (end - va < PAGE_SIZE_HUGE) return 0;

        pd[l2] = 0;
        *changed = true;

        if (is_table_empty(pd)) {
            free_pages((void *)(pdpt[l3] & PTE_ADDR_MASK), 0);
            pdpt[l3] = 0;

            if (is_table_empty(pdpt)) {
                free_pages((void *)(pml4[l4] & PTE_ADDR_MASK), 0);
                pml4[l4] = 0;
            }
        }

        return PAGE_SIZE_HUGE;
    }

    /* Normal page */
    pte_t *pt = (pte_t *)P2V(pd[l2] & PTE_ADDR_MASK);

    size_t l1 = PT_INDEX(va);
    if (!(pt[l1] & PTE_PRESENT)) return PAGE_SIZE;

    pt[l1] = 0;
    *changed = true;

    if (is_table_empty(pt)) {
        free_pages((void *)(pd[l2] & PTE_ADDR_MASK), 0);
        pd[l2] = 0;

        if (is_table_empty(pd)) {
            free_pages((void *)(pdpt[l3] & PTE_ADDR_MASK), 0);
            pdpt[l3] = 0;

            if (is_table_empty(pdpt)) {
                free_pages((void *)(pml4[l4] & PTE_ADDR_MASK), 0);
                pml4[l4] = 0;
            }
        }
    }

    return PAGE_SIZE;
}

int pmap_map(struct pmap *pmap, uintptr_t virt, uintptr_t phys, size_t size, uint32_t flags) {
    if (!pmap || size == 0) return -1;

    size_t step = (flags & PROT_HUGE) ? PAGE_SIZE_HUGE : PAGE_SIZE;

    if ((virt & (step - 1)) || (phys & (step - 1)) || (size & (step - 1)))
        return -1;

    if (virt + size < virt || phys + size < phys) return -1;

    uintptr_t end = virt + size;

    if (pmap != pmap_kernel() &&
        (PML4_INDEX(virt) >= 256 || PML4_INDEX(end - 1) >= 256))
        return -1;

    uint64_t f = spin_lock_irqsave(&pmap->lock);

    for (uintptr_t va = virt; va < end; va += step) {
        if (pmap_map_page(pmap, va, phys + (va - virt), flags) != 0) {
            for (uintptr_t undo = virt; undo < va; undo += step) {
                bool changed = false;
                pmap_unmap_page(pmap, undo, undo + step, &changed);
            }

            if ((read_cr3() & PTE_ADDR_MASK) == pmap->pm_root_phys)
                flush_tlb_all();

            spin_unlock_irqrestore(&pmap->lock, f);
            return -1;
        }
    }

    if ((read_cr3() & PTE_ADDR_MASK) == pmap->pm_root_phys)
        flush_tlb_all();

    spin_unlock_irqrestore(&pmap->lock, f);
    return 0;
}

void pmap_unmap(struct pmap *pmap, uintptr_t virt, size_t size) {
    if (!pmap || size == 0) return;
    if ((virt & (PAGE_SIZE - 1)) || (size & (PAGE_SIZE - 1))) return;
    if (virt + size < virt) return;

    uintptr_t end = virt + size;

    if (pmap != pmap_kernel() &&
        (PML4_INDEX(virt) >= 256 || PML4_INDEX(end - 1) >= 256))
        return;

    uint64_t f = spin_lock_irqsave(&pmap->lock);

    bool changed = false;

    for (uintptr_t va = virt; va < end;) {
        size_t step = pmap_unmap_page(pmap, va, end, &changed);
        if (!step) break;

        va += step;
    }

    if (changed && (read_cr3() & PTE_ADDR_MASK) == pmap->pm_root_phys)
        flush_tlb_all();

    spin_unlock_irqrestore(&pmap->lock, f);
}

uintptr_t pmap_extract(struct pmap *pmap, uintptr_t va) {
    if (!pmap) return 0;

    uint64_t f = spin_lock_irqsave(&pmap->lock);

    pml4e_t *pml4 = (pml4e_t *)pmap->pm_root_virt;

    /* PML4 -> PDPT */
    size_t l4 = PML4_INDEX(va);
    if (!(pml4[l4] & PTE_PRESENT)) {
        spin_unlock_irqrestore(&pmap->lock, f);
        return 0;
    }

    pdpte_t *pdpt = (pdpte_t *)P2V(pml4[l4] & PTE_ADDR_MASK);

    /* PDPT -> PD */
    size_t l3 = PDPT_INDEX(va);
    if (!(pdpt[l3] & PTE_PRESENT) || (pdpt[l3] & PDPTE_PS)) {
        spin_unlock_irqrestore(&pmap->lock, f);
        return 0;
    }

    pde_t *pd = (pde_t *)P2V(pdpt[l3] & PTE_ADDR_MASK);

    /* HUGE page */
    size_t l2 = PD_INDEX(va);
    if (!(pd[l2] & PTE_PRESENT)) {
        spin_unlock_irqrestore(&pmap->lock, f);
        return 0;
    }

    if (pd[l2] & PDE_PS) {
        uintptr_t pa = (pd[l2] & PDE_ADDR_MASK) + (va & (PAGE_SIZE_HUGE - 1));
        spin_unlock_irqrestore(&pmap->lock, f);
        return pa;
    }

    /* Normal page */
    pte_t *pt = (pte_t *)P2V(pd[l2] & PTE_ADDR_MASK);

    size_t l1 = PT_INDEX(va);
    if (!(pt[l1] & PTE_PRESENT)) {
        spin_unlock_irqrestore(&pmap->lock, f);
        return 0;
    }

    uintptr_t pa = (pt[l1] & PTE_ADDR_MASK) + (va & (PAGE_SIZE - 1));

    spin_unlock_irqrestore(&pmap->lock, f);
    return pa;
}

void pmap_destroy(struct pmap *pmap) {
    if (!pmap || pmap == pmap_kernel()) return;
    if ((read_cr3() & PTE_ADDR_MASK) == pmap->pm_root_phys) return;

    uint64_t f = spin_lock_irqsave(&pmap->lock);

    pml4e_t *pml4 = (pml4e_t *)pmap->pm_root_virt;

    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PTE_PRESENT)) continue;

        uintptr_t pdpt_phys = pml4[i] & PTE_ADDR_MASK;
        pdpte_t *pdpt = (pdpte_t *)P2V(pdpt_phys);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT) || (pdpt[j] & PDPTE_PS)) continue;

            uintptr_t pd_phys = pdpt[j] & PTE_ADDR_MASK;
            pde_t *pd = (pde_t *)P2V(pd_phys);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT) || (pd[k] & PDE_PS)) continue;

                free_pages((void *)(pd[k] & PTE_ADDR_MASK), 0);
            }

            free_pages((void *)pd_phys, 0);
        }

        free_pages((void *)pdpt_phys, 0);
    }

    free_pages((void *)pmap->pm_root_phys, 0);

    spin_unlock_irqrestore(&pmap->lock, f);
    kfree(pmap);
}

void pmap_activate(struct pmap *pmap) {
    if (!pmap || !pmap->pm_root_phys) return;

    write_cr3(pmap->pm_root_phys);
}

int pmap_init(void) {
    uintptr_t pml4_phys = (uintptr_t)alloc_pages(0);
    if (!pml4_phys) return -1;

    pml4e_t *pml4_virt = (pml4e_t *)P2V(pml4_phys);
    memset(pml4_virt, 0, PAGE_SIZE);

    spin_lock_init(&g_kernel_pmap.lock);
    g_kernel_pmap.pm_root_phys = pml4_phys;
    g_kernel_pmap.pm_root_virt = pml4_virt;

    return 0;
}
