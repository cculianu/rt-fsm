/*
 *  ELF file handling for TCC
 * 
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

static int put_elf_str(Section *s, const char *sym)
{
    int offset, len;
    char *ptr;

    len = Strlen(sym) + 1;
    offset = s->data_offset;
    ptr = section_ptr_add(s, len);
    Memcpy(ptr, sym, len);
    return offset;
}

/* elf symbol hashing function */
static unsigned long elf_hash(const unsigned char *name)
{
    unsigned long h = 0, g;
    
    while (*name) {
        h = (h << 4) + *name++;
        g = h & 0xf0000000;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* rebuild hash table of section s */
/* NOTE: we do factorize the hash table code to go faster */
static void rebuild_hash(Section *s, unsigned int nb_buckets)
{
    Elf32_Sym *sym;
    int *ptr, *hash, nb_syms, sym_index, h;
    char *strtab;

    strtab = s->link->data;
    nb_syms = s->data_offset / sizeof(Elf32_Sym);

    s->hash->data_offset = 0;
    ptr = section_ptr_add(s->hash, (2 + nb_buckets + nb_syms) * sizeof(int));
    ptr[0] = nb_buckets;
    ptr[1] = nb_syms;
    ptr += 2;
    hash = ptr;
    Memset(hash, 0, (nb_buckets + 1) * sizeof(int));
    ptr += nb_buckets + 1;

    sym = (Elf32_Sym *)s->data + 1;
    for(sym_index = 1; sym_index < nb_syms; sym_index++) {
        if (ELF32_ST_BIND(sym->st_info) != STB_LOCAL) {
            h = elf_hash(strtab + sym->st_name) % nb_buckets;
            *ptr = hash[h];
            hash[h] = sym_index;
        } else {
            *ptr = 0;
        }
        ptr++;
        sym++;
    }
}

/* return the symbol number */
static int put_elf_sym(Section *s, 
                       unsigned long value, unsigned long size,
                       int info, int other, int shndx, const char *name)
{
    int name_offset, sym_index;
    int nbuckets, h;
    Elf32_Sym *sym;
    Section *hs;
    
    sym = section_ptr_add(s, sizeof(Elf32_Sym));
    if (name)
        name_offset = put_elf_str(s->link, name);
    else
        name_offset = 0;
    /* XXX: endianness */
    sym->st_name = name_offset;
    sym->st_value = value;
    sym->st_size = size;
    sym->st_info = info;
    sym->st_other = other;
    sym->st_shndx = shndx;
    sym_index = sym - (Elf32_Sym *)s->data;
    hs = s->hash;
    if (hs) {
        int *ptr, *base;
        ptr = section_ptr_add(hs, sizeof(int));
        base = (int *)hs->data;
        /* only add global or weak symbols */
        if (ELF32_ST_BIND(info) != STB_LOCAL) {
            /* add another hashing entry */
            nbuckets = base[0];
            h = elf_hash(name) % nbuckets;
            *ptr = base[2 + h];
            base[2 + h] = sym_index;
            base[1]++;
            /* we resize the hash table */
            hs->nb_hashed_syms++;
            if (hs->nb_hashed_syms > 2 * nbuckets) {
                rebuild_hash(s, 2 * nbuckets);
            }
        } else {
            *ptr = 0;
            base[1]++;
        }
    }
    return sym_index;
}

/* find global ELF symbol 'name' and return its index. Return 0 if not
   found. */
static int find_elf_sym(Section *s, const char *name)
{
    Elf32_Sym *sym;
    Section *hs;
    int nbuckets, sym_index, h;
    const char *name1;
    
    hs = s->hash;
    if (!hs)
        return 0;
    nbuckets = ((int *)hs->data)[0];
    h = elf_hash(name) % nbuckets;
    sym_index = ((int *)hs->data)[2 + h];
    while (sym_index != 0) {
        sym = &((Elf32_Sym *)s->data)[sym_index];
        name1 = s->link->data + sym->st_name;
        if (!Strcmp(name, name1))
            return sym_index;
        sym_index = ((int *)hs->data)[2 + nbuckets + sym_index];
    }
    return 0;
}

/* return elf symbol value or error */
int tcc_get_symbol(TCCState *s, unsigned long *pval, const char *name)
{
    int sym_index;
    Elf32_Sym *sym;
    
    sym_index = find_elf_sym(symtab_section, name);
    if (!sym_index)
        return -1;
    sym = &((Elf32_Sym *)symtab_section->data)[sym_index];
    *pval = sym->st_value;
    return 0;
}

void *tcc_get_symbol_err(TCCState *s, const char *name)
{
    unsigned long val;
    if (tcc_get_symbol(s, &val, name) < 0)
        error("%s not defined", name);
    return (void *)val;
}

/* add an elf symbol : check if it is already defined and patch
   it. Return symbol index. NOTE that sh_num can be SHN_UNDEF. */
static int add_elf_sym(Section *s, unsigned long value, unsigned long size,
                       int info, int other, int sh_num, const char *name)
{
    Elf32_Sym *esym;
    int sym_bind, sym_index, sym_type, esym_bind;

    sym_bind = ELF32_ST_BIND(info);
    sym_type = ELF32_ST_TYPE(info);
        
    if (sym_bind != STB_LOCAL) {
        /* we search global or weak symbols */
        sym_index = find_elf_sym(s, name);
        if (!sym_index)
            goto do_def;
        esym = &((Elf32_Sym *)s->data)[sym_index];
        if (esym->st_shndx != SHN_UNDEF) {
            esym_bind = ELF32_ST_BIND(esym->st_info);
            if (sh_num == SHN_UNDEF) {
                /* ignore adding of undefined symbol if the
                   corresponding symbol is already defined */
            } else if (sym_bind == STB_GLOBAL && esym_bind == STB_WEAK) {
                /* global overrides weak, so patch */
                goto do_patch;
            } else if (sym_bind == STB_WEAK && esym_bind == STB_GLOBAL) {
                /* weak is ignored if already global */
            } else {
#if 0
                printf("new_bind=%d new_shndx=%d last_bind=%d old_shndx=%d\n",
                       sym_bind, sh_num, esym_bind, esym->st_shndx);
#endif
                /* NOTE: we accept that two DLL define the same symbol */
                if (s != tcc_state->dynsymtab_section)
                    error_noabort("'%s' defined twice", name);
            }
        } else {
        do_patch:
            esym->st_info = ELF32_ST_INFO(sym_bind, sym_type);
            esym->st_shndx = sh_num;
            esym->st_value = value;
            esym->st_size = size;
            esym->st_other = other;
        }
    } else {
    do_def:
        sym_index = put_elf_sym(s, value, size, 
                                ELF32_ST_INFO(sym_bind, sym_type), other, 
                                sh_num, name);
    }
    return sym_index;
}

/* put relocation */
static void put_elf_reloc(Section *symtab, Section *s, unsigned long offset,
                          int type, int symbol)
{
    char buf[256];
    Section *sr;
    Elf32_Rel *rel;

    sr = s->reloc;
    if (!sr) {
        /* if no relocation section, create it */
        Snprintf(buf, sizeof(buf), ".rel%s", s->name);
        /* if the symtab is allocated, then we consider the relocation
           are also */
        sr = new_section(tcc_state, buf, SHT_REL, symtab->sh_flags);
        sr->sh_entsize = sizeof(Elf32_Rel);
        sr->link = symtab;
        sr->sh_info = s->sh_num;
        s->reloc = sr;
    }
    rel = section_ptr_add(sr, sizeof(Elf32_Rel));
    rel->r_offset = offset;
    rel->r_info = ELF32_R_INFO(symbol, type);
}

/* put stab debug information */

typedef struct {
    unsigned long n_strx;         /* index into string table of name */
    unsigned char n_type;         /* type of symbol */
    unsigned char n_other;        /* misc info (usually empty) */
    unsigned short n_desc;        /* description field */
    unsigned long n_value;        /* value of symbol */
} Stab_Sym;

static void put_stabs(const char *str, int type, int other, int desc, 
                      unsigned long value)
{
    Stab_Sym *sym;

    sym = section_ptr_add(stab_section, sizeof(Stab_Sym));
    if (str) {
        sym->n_strx = put_elf_str(stabstr_section, str);
    } else {
        sym->n_strx = 0;
    }
    sym->n_type = type;
    sym->n_other = other;
    sym->n_desc = desc;
    sym->n_value = value;
}

static void put_stabs_r(const char *str, int type, int other, int desc, 
                        unsigned long value, Section *sec, int sym_index)
{
    put_stabs(str, type, other, desc, value);
    put_elf_reloc(symtab_section, stab_section, 
                  stab_section->data_offset - sizeof(unsigned long),
                  R_DATA_32, sym_index);
}

static void put_stabn(int type, int other, int desc, int value)
{
    put_stabs(NULL, type, other, desc, value);
}

static void put_stabd(int type, int other, int desc)
{
    put_stabs(NULL, type, other, desc, 0);
}

/* relocate common symbols in the .bss section */
static void relocate_common_syms(void)
{
    Elf32_Sym *sym, *sym_end;
    unsigned long offset, align;
    
    sym_end = (Elf32_Sym *)(symtab_section->data + symtab_section->data_offset);
    for(sym = (Elf32_Sym *)symtab_section->data + 1; 
        sym < sym_end;
        sym++) {
        if (sym->st_shndx == SHN_COMMON) {
            /* align symbol */
            align = sym->st_value;
            offset = bss_section->data_offset;
            offset = (offset + align - 1) & -align;
            sym->st_value = offset;
            sym->st_shndx = bss_section->sh_num;
            offset += sym->st_size;
            bss_section->data_offset = offset;
        }
    }
}

/* relocate symbol table, resolve undefined symbols if do_resolve is
   true and output error if undefined symbol. */
static void relocate_syms(TCCState *s1, int do_resolve)
{
    Elf32_Sym *sym, *esym, *sym_end;
    int sym_bind, sh_num, sym_index;
    const char *name;
    unsigned long addr;

    sym_end = (Elf32_Sym *)(symtab_section->data + symtab_section->data_offset);
    for(sym = (Elf32_Sym *)symtab_section->data + 1; 
        sym < sym_end;
        sym++) {
        sh_num = sym->st_shndx;
        if (sh_num == SHN_UNDEF) {
            name = strtab_section->data + sym->st_name;
            if (do_resolve) {
                name = symtab_section->link->data + sym->st_name;
                addr = (unsigned long)resolve_sym(s1, name, ELF32_ST_TYPE(sym->st_info));
                if (addr) {
                    sym->st_value = addr;
                    goto found;
                }
            } else if (s1->dynsym) {
                /* if dynamic symbol exist, then use it */
                sym_index = find_elf_sym(s1->dynsym, name);
                if (sym_index) {
                    esym = &((Elf32_Sym *)s1->dynsym->data)[sym_index];
                    sym->st_value = esym->st_value;
                    goto found;
                }
            }
            /* XXX: _fp_hw seems to be part of the ABI, so we ignore
               it */
            if (!Strcmp(name, "_fp_hw"))
                goto found;
            /* only weak symbols are accepted to be undefined. Their
               value is zero */
            sym_bind = ELF32_ST_BIND(sym->st_info);
            if (sym_bind == STB_WEAK) {
                sym->st_value = 0;
            } else {
                error_noabort("undefined symbol '%s'", name);
            }
        } else if (sh_num < SHN_LORESERVE) {
            /* add section base */
            sym->st_value += s1->sections[sym->st_shndx]->sh_addr;
        }
    found: ;
    }
}

/* relocate a given section (CPU dependent) */
static void relocate_section(TCCState *s1, Section *s)
{
    Section *sr;
    Elf32_Rel *rel, *rel_end, *qrel;
    Elf32_Sym *sym;
    int type, sym_index;
    unsigned char *ptr;
    unsigned long val, addr;
#if defined(TCC_TARGET_I386)
    int esym_index;
#endif

    sr = s->reloc;
    rel_end = (Elf32_Rel *)(sr->data + sr->data_offset);
    qrel = (Elf32_Rel *)sr->data;
    for(rel = qrel;
        rel < rel_end;
        rel++) {
        ptr = s->data + rel->r_offset;

        sym_index = ELF32_R_SYM(rel->r_info);
        sym = &((Elf32_Sym *)symtab_section->data)[sym_index];
        val = sym->st_value;
        type = ELF32_R_TYPE(rel->r_info);
        addr = s->sh_addr + rel->r_offset;

        /* CPU specific */
        switch(type) {
#if defined(TCC_TARGET_I386)
        case R_386_32:
            if (s1->output_type == TCC_OUTPUT_DLL) {
                esym_index = s1->symtab_to_dynsym[sym_index];
                qrel->r_offset = rel->r_offset;
                if (esym_index) {
                    qrel->r_info = ELF32_R_INFO(esym_index, R_386_32);
                    qrel++;
                    break;
                } else {
                    qrel->r_info = ELF32_R_INFO(0, R_386_RELATIVE);
                    qrel++;
                }
            }
            *(int *)ptr += val;
            break;
        case R_386_PC32:
            if (s1->output_type == TCC_OUTPUT_DLL) {
                /* DLL relocation */
                esym_index = s1->symtab_to_dynsym[sym_index];
                if (esym_index) {
                    qrel->r_offset = rel->r_offset;
                    qrel->r_info = ELF32_R_INFO(esym_index, R_386_PC32);
                    qrel++;
                    break;
                }
            }
            *(int *)ptr += val - addr;
            break;
        case R_386_PLT32:
            *(int *)ptr += val - addr;
            break;
        case R_386_GLOB_DAT:
        case R_386_JMP_SLOT:
            *(int *)ptr = val;
            break;
        case R_386_GOTPC:
            *(int *)ptr += s1->got->sh_addr - addr;
            break;
        case R_386_GOTOFF:
            *(int *)ptr += val - s1->got->sh_addr;
            break;
        case R_386_GOT32:
            /* we load the got offset */
            *(int *)ptr += s1->got_offsets[sym_index];
            break;
#elif defined(TCC_TARGET_ARM)
	case R_ARM_PC24:
	case R_ARM_PLT32:
	    {
                int x;
                x = (*(int *)ptr)&0xffffff;
                (*(int *)ptr) &= 0xff000000;
                if (x & 0x800000)
                    x -= 0x1000000;
                x *= 4;
                x += val - addr;
                if((x & 3) != 0 || x >= 0x4000000 || x < -0x4000000)
                    error("can't relocate value at %x",addr);
                x >>= 2;
                x &= 0xffffff;
                (*(int *)ptr) |= x;
	    }
	    break;
	case R_ARM_ABS32:
	    *(int *)ptr += val;
	    break;
	case R_ARM_GOTPC:
	    *(int *)ptr += s1->got->sh_addr - addr;
	    break;
        case R_ARM_GOT32:
            /* we load the got offset */
            *(int *)ptr += s1->got_offsets[sym_index];
            break;
	case R_ARM_COPY:
            break;
	default:
	    fprintf(stderr,"FIXME: handle reloc type %x at %lx [%.8x] to %lx\n",
                    type,addr,(unsigned int )ptr,val);
            break;
#elif defined(TCC_TARGET_C67)
	case R_C60_32:
	    *(int *)ptr += val;
	    break;
        case R_C60LO16:
            {
                uint32_t orig;
                
                /* put the low 16 bits of the absolute address */
                // add to what is already there
                
                orig  =   ((*(int *)(ptr  )) >> 7) & 0xffff;
                orig |=  (((*(int *)(ptr+4)) >> 7) & 0xffff) << 16;
                
                //patch both at once - assumes always in pairs Low - High
                
                *(int *) ptr    = (*(int *) ptr    & (~(0xffff << 7)) ) |  (((val+orig)      & 0xffff) << 7);
                *(int *)(ptr+4) = (*(int *)(ptr+4) & (~(0xffff << 7)) ) | ((((val+orig)>>16) & 0xffff) << 7);
            }
            break;
        case R_C60HI16:
            break;
        default:
	    fprintf(stderr,"FIXME: handle reloc type %x at %lx [%.8x] to %lx\n",
                    type,addr,(unsigned int )ptr,val);
            break;
#else
#error unsupported processor
#endif
        }
    }
    /* if the relocation is allocated, we change its symbol table */
    if (sr->sh_flags & SHF_ALLOC)
        sr->link = s1->dynsym;
}

static void put_got_offset(TCCState *s1, int index, unsigned long val)
{
    int n;
    unsigned long *tab;

    if (index >= s1->nb_got_offsets) {
        /* find immediately bigger power of 2 and reallocate array */
        n = 1;
        while (index >= n)
            n *= 2;
        tab = tcc_realloc(s1->got_offsets, n * sizeof(unsigned long));
        if (!tab)
            error("memory full");
        s1->got_offsets = tab;
        Memset(s1->got_offsets + s1->nb_got_offsets, 0,
               (n - s1->nb_got_offsets) * sizeof(unsigned long));
        s1->nb_got_offsets = n;
    }
    s1->got_offsets[index] = val;
}

/* XXX: suppress that */
static void put32(unsigned char *p, uint32_t val)
{
    p[0] = val;
    p[1] = val >> 8;
    p[2] = val >> 16;
    p[3] = val >> 24;
}

static void build_got(TCCState *s1)
{
    unsigned char *ptr;

    /* if no got, then create it */
    s1->got = new_section(s1, ".got", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE);
    s1->got->sh_entsize = 4;
    add_elf_sym(symtab_section, 0, 4, ELF32_ST_INFO(STB_GLOBAL, STT_OBJECT), 
                0, s1->got->sh_num, "_GLOBAL_OFFSET_TABLE_");
    ptr = section_ptr_add(s1->got, 3 * sizeof(int));
    /* keep space for _DYNAMIC pointer, if present */
    put32(ptr, 0);
    /* two dummy got entries */
    put32(ptr + 4, 0);
    put32(ptr + 8, 0);
}

/* put a got entry corresponding to a symbol in symtab_section. 'size'
   and 'info' can be modifed if more precise info comes from the DLL */
static void put_got_entry(TCCState *s1,
                          int reloc_type, unsigned long size, int info, 
                          int sym_index)
{
    int index;
    const char *name;
    Elf32_Sym *sym;
    unsigned long offset;
    int *ptr;

    if (!s1->got)
        build_got(s1);

    /* if a got entry already exists for that symbol, no need to add one */
    if (sym_index < s1->nb_got_offsets &&
        s1->got_offsets[sym_index] != 0)
        return;
    
    put_got_offset(s1, sym_index, s1->got->data_offset);

    if (s1->dynsym) {
        sym = &((Elf32_Sym *)symtab_section->data)[sym_index];
        name = symtab_section->link->data + sym->st_name;
        offset = sym->st_value;
#ifdef TCC_TARGET_I386
        if (reloc_type == R_386_JMP_SLOT) {
            Section *plt;
            uint8_t *p;
            int modrm;

            /* if we build a DLL, we add a %ebx offset */
            if (s1->output_type == TCC_OUTPUT_DLL)
                modrm = 0xa3;
            else
                modrm = 0x25;

            /* add a PLT entry */
            plt = s1->plt;
            if (plt->data_offset == 0) {
                /* first plt entry */
                p = section_ptr_add(plt, 16);
                p[0] = 0xff; /* pushl got + 4 */
                p[1] = modrm + 0x10;
                put32(p + 2, 4);
                p[6] = 0xff; /* jmp *(got + 8) */
                p[7] = modrm;
                put32(p + 8, 8);
            }

            p = section_ptr_add(plt, 16);
            p[0] = 0xff; /* jmp *(got + x) */
            p[1] = modrm;
            put32(p + 2, s1->got->data_offset);
            p[6] = 0x68; /* push $xxx */
            put32(p + 7, (plt->data_offset - 32) >> 1);
            p[11] = 0xe9; /* jmp plt_start */
            put32(p + 12, -(plt->data_offset));

            /* the symbol is modified so that it will be relocated to
               the PLT */
            if (s1->output_type == TCC_OUTPUT_EXE)
                offset = plt->data_offset - 16;
        }
#elif defined(TCC_TARGET_ARM)
	if (reloc_type == R_ARM_JUMP_SLOT) {
            Section *plt;
            uint8_t *p;
            
            /* if we build a DLL, we add a %ebx offset */
            if (s1->output_type == TCC_OUTPUT_DLL)
                error("DLLs unimplemented!");

            /* add a PLT entry */
            plt = s1->plt;
            if (plt->data_offset == 0) {
                /* first plt entry */
                p = section_ptr_add(plt, 16);
		put32(p     , 0xe52de004);
		put32(p +  4, 0xe59fe010);
		put32(p +  8, 0xe08fe00e);
		put32(p + 12, 0xe5bef008);
            }

            p = section_ptr_add(plt, 16);
	    put32(p  , 0xe59fc004);
	    put32(p+4, 0xe08fc00c);
	    put32(p+8, 0xe59cf000);
	    put32(p+12, s1->got->data_offset);

            /* the symbol is modified so that it will be relocated to
               the PLT */
            if (s1->output_type == TCC_OUTPUT_EXE)
                offset = plt->data_offset - 16;
        }
#elif defined(TCC_TARGET_C67)
        error("C67 got not implemented");
#else
#error unsupported CPU
#endif
        index = put_elf_sym(s1->dynsym, offset, 
                            size, info, 0, sym->st_shndx, name);
        /* put a got entry */
        put_elf_reloc(s1->dynsym, s1->got, 
                      s1->got->data_offset, 
                      reloc_type, index);
    }
    ptr = section_ptr_add(s1->got, sizeof(int));
    *ptr = 0;
}

/* build GOT and PLT entries */
static void build_got_entries(TCCState *s1)
{
    Section *s, *symtab;
    Elf32_Rel *rel, *rel_end;
    Elf32_Sym *sym;
    int i, type, reloc_type, sym_index;

    for(i = 1; i < s1->nb_sections; i++) {
        s = s1->sections[i];
        if (s->sh_type != SHT_REL)
            continue;
        /* no need to handle got relocations */
        if (s->link != symtab_section)
            continue;
        symtab = s->link;
        rel_end = (Elf32_Rel *)(s->data + s->data_offset);
        for(rel = (Elf32_Rel *)s->data;
            rel < rel_end;
            rel++) {
            type = ELF32_R_TYPE(rel->r_info);
            switch(type) {
#if defined(TCC_TARGET_I386)
            case R_386_GOT32:
            case R_386_GOTOFF:
            case R_386_GOTPC:
            case R_386_PLT32:
                if (!s1->got)
                    build_got(s1);
                if (type == R_386_GOT32 || type == R_386_PLT32) {
                    sym_index = ELF32_R_SYM(rel->r_info);
                    sym = &((Elf32_Sym *)symtab_section->data)[sym_index];
                    /* look at the symbol got offset. If none, then add one */
                    if (type == R_386_GOT32)
                        reloc_type = R_386_GLOB_DAT;
                    else
                        reloc_type = R_386_JMP_SLOT;
                    put_got_entry(s1, reloc_type, sym->st_size, sym->st_info, 
                                  sym_index);
                }
                break;
#elif defined(TCC_TARGET_ARM)
	    case R_ARM_GOT32:
            case R_ARM_GOTOFF:
            case R_ARM_GOTPC:
            case R_ARM_PLT32:
                if (!s1->got)
                    build_got(s1);
                if (type == R_ARM_GOT32 || type == R_ARM_PLT32) {
                    sym_index = ELF32_R_SYM(rel->r_info);
                    sym = &((Elf32_Sym *)symtab_section->data)[sym_index];
                    /* look at the symbol got offset. If none, then add one */
                    if (type == R_ARM_GOT32)
                        reloc_type = R_ARM_GLOB_DAT;
                    else
                        reloc_type = R_ARM_JUMP_SLOT;
                    put_got_entry(s1, reloc_type, sym->st_size, sym->st_info, 
                                  sym_index);
                }
                break;
#elif defined(TCC_TARGET_C67)
	    case R_C60_GOT32:
            case R_C60_GOTOFF:
            case R_C60_GOTPC:
            case R_C60_PLT32:
                if (!s1->got)
                    build_got(s1);
                if (type == R_C60_GOT32 || type == R_C60_PLT32) {
                    sym_index = ELF32_R_SYM(rel->r_info);
                    sym = &((Elf32_Sym *)symtab_section->data)[sym_index];
                    /* look at the symbol got offset. If none, then add one */
                    if (type == R_C60_GOT32)
                        reloc_type = R_C60_GLOB_DAT;
                    else
                        reloc_type = R_C60_JMP_SLOT;
                    put_got_entry(s1, reloc_type, sym->st_size, sym->st_info, 
                                  sym_index);
                }
                break;
#else
#error unsupported CPU
#endif
            default:
                break;
            }
        }
    }
}

static Section *new_symtab(TCCState *s1,
                           const char *symtab_name, int sh_type, int sh_flags,
                           const char *strtab_name, 
                           const char *hash_name, int hash_sh_flags)
{
    Section *symtab, *strtab, *hash;
    int *ptr, nb_buckets;

    symtab = new_section(s1, symtab_name, sh_type, sh_flags);
    symtab->sh_entsize = sizeof(Elf32_Sym);
    strtab = new_section(s1, strtab_name, SHT_STRTAB, sh_flags);
    put_elf_str(strtab, "");
    symtab->link = strtab;
    put_elf_sym(symtab, 0, 0, 0, 0, 0, NULL);
    
    nb_buckets = 1;

    hash = new_section(s1, hash_name, SHT_HASH, hash_sh_flags);
    hash->sh_entsize = sizeof(int);
    symtab->hash = hash;
    hash->link = symtab;

    ptr = section_ptr_add(hash, (2 + nb_buckets + 1) * sizeof(int));
    ptr[0] = nb_buckets;
    ptr[1] = 1;
    Memset(ptr + 2, 0, (nb_buckets + 1) * sizeof(int));
    return symtab;
}

static void add_init_array_defines(TCCState *s1, const char *section_name)
{
    Section *s;
    long end_offset;
    char sym_start[1024];
    char sym_end[1024];
    
    Snprintf(sym_start, sizeof(sym_start), "__%s_start", section_name + 1);
    Snprintf(sym_end, sizeof(sym_end), "__%s_end", section_name + 1);

    s = find_section(s1, section_name);
    if (!s) {
        end_offset = 0;
        s = data_section;
    } else {
        end_offset = s->data_offset;
    }

    add_elf_sym(symtab_section, 
                0, 0,
                ELF32_ST_INFO(STB_GLOBAL, STT_NOTYPE), 0,
                s->sh_num, sym_start);
    add_elf_sym(symtab_section, 
                end_offset, 0,
                ELF32_ST_INFO(STB_GLOBAL, STT_NOTYPE), 0,
                s->sh_num, sym_end);
}

/* add tcc runtime libraries */
static void tcc_add_runtime(TCCState *s1)
{
    char buf[1024];

#ifdef CONFIG_TCC_BCHECK
    if (do_bounds_check) {
        unsigned long *ptr;
        Section *init_section;
        unsigned char *pinit;
        int sym_index;

        /* XXX: add an object file to do that */
        ptr = section_ptr_add(bounds_section, sizeof(unsigned long));
        *ptr = 0;
        add_elf_sym(symtab_section, 0, 0, 
                    ELF32_ST_INFO(STB_GLOBAL, STT_NOTYPE), 0,
                    bounds_section->sh_num, "__bounds_start");
        /* add bound check code */
        Snprintf(buf, sizeof(buf), "%s/%s", tcc_lib_path, "bcheck.o");
        /*tcc_add_file(s1, buf);*/
#ifdef TCC_TARGET_I386
        if (s1->output_type != TCC_OUTPUT_MEMORY) {
            /* add 'call __bound_init()' in .init section */
            init_section = find_section(s1, ".init");
            pinit = section_ptr_add(init_section, 5);
            pinit[0] = 0xe8;
            put32(pinit + 1, -4);
            sym_index = find_elf_sym(symtab_section, "__bound_init");
            put_elf_reloc(symtab_section, init_section, 
                          init_section->data_offset - 4, R_386_PC32, sym_index);
        }
#endif
    }
#endif
    /* add libc */
    if (!s1->nostdlib) {
        /*tcc_add_library(s1, "c");*/

        Snprintf(buf, sizeof(buf), "%s/%s", tcc_lib_path, "libtcc1.a");
        /*tcc_add_file(s1, buf);*/
    }
    /* add crt end if not memory output */
    if (s1->output_type != TCC_OUTPUT_MEMORY && !s1->nostdlib) {
      /*tcc_add_file(s1, CONFIG_TCC_CRT_PREFIX "/crtn.o");*/
    }
}

/* add various standard linker symbols (must be done after the
   sections are filled (for example after allocating common
   symbols)) */
static void tcc_add_linker_symbols(TCCState *s1)
{
    char buf[1024];
    int i;
    Section *s;

    add_elf_sym(symtab_section, 
                text_section->data_offset, 0,
                ELF32_ST_INFO(STB_GLOBAL, STT_NOTYPE), 0,
                text_section->sh_num, "_etext");
    add_elf_sym(symtab_section, 
                data_section->data_offset, 0,
                ELF32_ST_INFO(STB_GLOBAL, STT_NOTYPE), 0,
                data_section->sh_num, "_edata");
    add_elf_sym(symtab_section, 
                bss_section->data_offset, 0,
                ELF32_ST_INFO(STB_GLOBAL, STT_NOTYPE), 0,
                bss_section->sh_num, "_end");
    /* horrible new standard ldscript defines */
    add_init_array_defines(s1, ".preinit_array");
    add_init_array_defines(s1, ".init_array");
    add_init_array_defines(s1, ".fini_array");
    
    /* add start and stop symbols for sections whose name can be
       expressed in C */
    for(i = 1; i < s1->nb_sections; i++) {
        s = s1->sections[i];
        if (s->sh_type == SHT_PROGBITS &&
            (s->sh_flags & SHF_ALLOC)) {
            const char *p;
            int ch;

            /* check if section name can be expressed in C */
            p = s->name;
            for(;;) {
                ch = *p;
                if (!ch)
                    break;
                if (!isid(ch) && !isnum(ch))
                    goto next_sec;
                p++;
            }
            Snprintf(buf, sizeof(buf), "__start_%s", s->name);
            add_elf_sym(symtab_section, 
                        0, 0,
                        ELF32_ST_INFO(STB_GLOBAL, STT_NOTYPE), 0,
                        s->sh_num, buf);
            Snprintf(buf, sizeof(buf), "__stop_%s", s->name);
            add_elf_sym(symtab_section,
                        s->data_offset, 0,
                        ELF32_ST_INFO(STB_GLOBAL, STT_NOTYPE), 0,
                        s->sh_num, buf);
        }
    next_sec: ;
    }
}

typedef struct SectionMergeInfo {
    Section *s;            /* corresponding existing section */
    unsigned long offset;  /* offset of the new section in the existing section */
    uint8_t new_section;       /* true if section 's' was added */
    uint8_t link_once;         /* true if link once section */
} SectionMergeInfo;


#define ARMAG  "!<arch>\012"	/* For COFF and a.out archives */

typedef struct ArchiveHeader {
    char ar_name[16];		/* name of this member */
    char ar_date[12];		/* file mtime */
    char ar_uid[6];		/* owner uid; printed as decimal */
    char ar_gid[6];		/* owner gid; printed as decimal */
    char ar_mode[8];		/* file mode, printed as octal   */
    char ar_size[10];		/* file size, printed as decimal */
    char ar_fmag[2];		/* should contain ARFMAG */
} ArchiveHeader;

