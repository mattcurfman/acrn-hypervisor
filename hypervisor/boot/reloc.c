/*
 * Copyright (C) 2018 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <hypervisor.h>
#include <reloc.h>
#ifdef CONFIG_EFI_STUB
#include <acrn_efi.h>
#endif

struct Elf64_Dyn {
	uint64_t d_tag;
	uint64_t d_ptr;
};

#define DT_NULL		0	/* end of .dynamic section */
#define DT_RELA		7	/* relocation table */
#define DT_RELASZ	8	/* size of reloc table */
#define DT_RELAENT	9	/* size of one entry */

struct Elf64_Rel {
	uint64_t r_offset;
	uint64_t r_info;
	uint64_t reserved;
};

#define ELF64_R_TYPE(i)		((i) & 0xffffffff)
#define R_X86_64_RELATIVE	8

uint64_t trampoline_start16_paddr;

/* get the delta between CONFIG_RAM_START and the actual load address */
uint64_t get_hv_image_delta(void)
{
	uint64_t addr;

	asm volatile (" call 0f\n"
		"0: pop %%rax\n"
		"	sub $0b, %%rax\n"
		"	mov %%rax, %0\n"
		: "=m" (addr)
		:
		: "%rax");

	return addr;
}

/* get the actual Hypervisor load address */
uint64_t get_hv_image_base(void)
{
	return (get_hv_image_delta() + CONFIG_RAM_START);
}

/*
 * Because trampoline code is relocated in different way, if HV code
 * accesses trampoline using relative addressing, it needs to take
 * out the HV relocation delta
 *
 * This function is valid if:
 *  - The hpa of HV code is always higher than trampoline code
 *  - The HV code is always relocated to higher address, compared
 *    with CONFIG_RAM_START
 */
static uint64_t trampoline_relo_addr(void *addr)
{
	return (uint64_t)addr - get_hv_image_delta();
}

#ifdef CONFIG_RELOC
void _relocate(void)
{
	struct Elf64_Dyn *dyn;
	struct Elf64_Rel *start = NULL, *end = NULL;
	uint64_t delta, size = 0;
	uint64_t trampoline_end;
	uint64_t primary_32_start, primary_32_end;
	uint64_t *addr;

	/* get the delta that needs to be patched */
	delta = get_hv_image_delta();
	if (delta == 0U)
		return;

	/* Look for the descriptoin of relocation sections */
	for (dyn = (struct Elf64_Dyn *)_DYNAMIC; dyn->d_tag != DT_NULL; dyn++) {
		switch (dyn->d_tag) {
		case DT_RELA:
			start = (struct Elf64_Rel *)(dyn->d_ptr + delta);
			break;
		case DT_RELASZ:
			end = (struct Elf64_Rel *)start + dyn->d_ptr;
			break;
		case DT_RELAENT:
			size = dyn->d_ptr;
			break;
		}
	}

	/* Sanity check */
	if ((start == NULL) || (size == 0U))
		return;

	/*
	 * Need to subtract the relocation delta to get the correct
	 * absolute addresses
	 */
	trampoline_end = (uint64_t)_ld_trampoline_end - delta;
	primary_32_start = (uint64_t)cpu_primary_start_32 - delta;
	primary_32_end = (uint64_t)cpu_primary_start_64 - delta;

	while (start < end) {
		if ((ELF64_R_TYPE(start->r_info)) == R_X86_64_RELATIVE) {
			addr = (uint64_t *)(delta + start->r_offset);

			/*
			 * we won't fixup any trampoline.S and cpu_primary.S here
			 * for a number of reasons:
			 *
			 * - trampoline code itself takes another relocation,
			 *   so any entries for trampoline symbols can't be fixed up
			 *   through .rela sections
			 * - In cpu_primary.S, the 32 bits code doesn't need relocation
			 * - Linker option "-z noreloc-overflow" could force R_X86_32
			 *   to R_X86_64 in the relocation sections, which could make
			 *   the fixed up code dirty. Even if relocation for 32 bits
			 *   is needed in the future, it's recommended to do it
			 *   explicitly in the assembly code to avoid confusion.
			 */
			if ((start->r_offset > trampoline_end) &&
					((start->r_offset < primary_32_start) ||
					(start->r_offset > primary_32_end))) {
				*addr += delta;
			}
		}
		start = (struct Elf64_Rel *)((char *)start + size);
	}
}
#else
void _relocate(void)
{
}
#endif

uint64_t read_trampoline_sym(void *sym)
{
	uint64_t *hva;

	hva = HPA2HVA(trampoline_start16_paddr) + trampoline_relo_addr(sym);
	return *hva;
}

void write_trampoline_sym(void *sym, uint64_t val)
{
	uint64_t *hva;

	hva = HPA2HVA(trampoline_start16_paddr) + trampoline_relo_addr(sym);
	*hva = val;
}

static void update_trampoline_code_refs(uint64_t dest_pa)
{
	void *ptr;
	uint64_t val;
	int i;

	/*
	 * calculate the fixup CS:IP according to fixup target address
	 * dynamically.
	 *
	 * trampoline code starts in real mode,
	 * so the target addres is HPA
	 */
	val = dest_pa + trampoline_relo_addr(trampoline_fixup_target);

	ptr = HPA2HVA(dest_pa + trampoline_relo_addr(trampoline_fixup_cs));
	*(uint16_t *)(ptr) = (uint16_t)((val >> 4) & 0xFFFFU);

	ptr = HPA2HVA(dest_pa + trampoline_relo_addr(trampoline_fixup_ip));
	*(uint16_t *)(ptr) = (uint16_t)(val & 0xfU);

	/* Update temporary page tables */
	ptr = HPA2HVA(dest_pa + trampoline_relo_addr(CPU_Boot_Page_Tables_ptr));
	*(uint32_t *)(ptr) += (uint32_t)dest_pa;

	ptr = HPA2HVA(dest_pa + trampoline_relo_addr(CPU_Boot_Page_Tables_Start));
	*(uint64_t *)(ptr) += dest_pa;

	ptr = HPA2HVA(dest_pa + trampoline_relo_addr(trampoline_pdpt_addr));
	for (i = 0; i < 4; i++)
		*(uint64_t *)(ptr + sizeof(uint64_t) * i) += dest_pa;

	/* update the gdt base pointer with relocated offset */
	ptr = HPA2HVA(dest_pa + trampoline_relo_addr(trampoline_gdt_ptr));
	*(uint64_t *)(ptr + 2) += dest_pa;

	/* update trampoline jump pointer with relocated offset */
	ptr = HPA2HVA(dest_pa + trampoline_relo_addr(trampoline_start64_fixup));
	*(uint32_t *)ptr += dest_pa;

	/* update trampoline's main entry pointer */
	ptr = HPA2HVA(dest_pa + trampoline_relo_addr(main_entry));
	*(uint64_t *)ptr += get_hv_image_delta();

	/* update trampoline's spinlock pointer */
	ptr = HPA2HVA(dest_pa + trampoline_relo_addr(trampoline_spinlock_ptr));
	*(uint64_t *)ptr += get_hv_image_delta();
}

uint64_t prepare_trampoline(void)
{
	uint64_t size, dest_pa;

	size = (uint64_t)_ld_trampoline_end - (uint64_t)_ld_trampoline_start;
#ifndef CONFIG_EFI_STUB
	dest_pa = e820_alloc_low_memory(CONFIG_LOW_RAM_SIZE);
#else
	dest_pa = (uint64_t)get_ap_trampoline_buf();
#endif

	pr_dbg("trampoline code: %llx size %x", dest_pa, size);

	/* Copy segment for AP initialization code below 1MB */
	memcpy_s(HPA2HVA(dest_pa), (size_t)size, _ld_trampoline_load, (size_t)size);
	update_trampoline_code_refs(dest_pa);
	trampoline_start16_paddr = dest_pa;

	return dest_pa;
}
