/*
 * The LibVMI Library is an introspection library that simplifies access to 
 * memory in a target virtual machine or in a file containing a dump of 
 * a system's physical memory.  LibVMI is based on the XenAccess Library.
 *
 * Copyright (C) 2010 Sandia National Laboratories
 * Author: Bryan D. Payne (bpayne@sandia.gov)
 */

// Three kinds of cache:
//  1) PID --> DTB
//  2) Symbol --> Virtual address
//  3) Virtual address --> physical address

#include "libvmi.h"
#include "private.h"

#define _GNU_SOURCE
#include <glib.h>
#include <time.h>
#include <string.h>

//
// PID --> DTB cache implementation
// Note: DTB is a physical address
struct pid_cache_entry{
    int pid;
    addr_t dtb;
    time_t last_used;
};
typedef struct pid_cache_entry *pid_cache_entry_t;

static void pid_cache_key_free (gpointer data)
{
    if (data) free(data);
}

static void pid_cache_entry_free (gpointer data)
{
    pid_cache_entry_t entry = (pid_cache_entry_t) data;
    if (entry) free(entry);
}

static pid_cache_entry_t pid_cache_entry_create (int pid, addr_t dtb)
{
    pid_cache_entry_t entry = (pid_cache_entry_t) safe_malloc(sizeof(struct pid_cache_entry));
    entry->pid = pid;
    entry->dtb = dtb;
    entry->last_used = time(NULL);
    return entry;
}

void pid_cache_init (vmi_instance_t vmi)
{
    vmi->pid_cache = g_hash_table_new_full(g_int_hash, g_int_equal, pid_cache_key_free, pid_cache_entry_free);
}

void pid_cache_destroy (vmi_instance_t vmi)
{
    g_hash_table_unref(vmi->pid_cache);
}

status_t pid_cache_get (vmi_instance_t vmi, int pid, addr_t *dtb)
{
    pid_cache_entry_t entry = NULL;
    gint key = (gint) pid;

    if ((entry = g_hash_table_lookup(vmi->pid_cache, &key)) != NULL){
        entry->last_used = time(NULL);
        *dtb = entry->dtb;
        dbprint("--PID cache hit %d -- 0x%.8x\n", pid, *dtb);
        return VMI_SUCCESS;
    }

    return VMI_FAILURE;
}

void pid_cache_set (vmi_instance_t vmi, int pid, addr_t dtb)
{
    gint *key = (gint *) safe_malloc(sizeof(gint));
    *key = pid;
    pid_cache_entry_t entry = pid_cache_entry_create(pid, dtb);
    g_hash_table_insert(vmi->pid_cache, key, entry);
    dbprint("--PID cache set %d -- 0x%.8x\n", pid, dtb);
}

status_t pid_cache_del (vmi_instance_t vmi, int pid)
{
    gint key = (gint) pid;
    dbprint("--PID cache del %d\n", pid);
    if (TRUE == g_hash_table_remove(vmi->pid_cache, &key)){
        return VMI_SUCCESS;
    }
    else{
        return VMI_FAILURE;
    }
}

//
// Symbol --> Virtual address cache implementation
struct sym_cache_entry{
    char *sym;
    addr_t va;
    time_t last_used;
};
typedef struct sym_cache_entry *sym_cache_entry_t;

static void sym_cache_entry_free (gpointer data)
{
    sym_cache_entry_t entry = (sym_cache_entry_t) data;
    if (entry){
        if (entry->sym) free(entry->sym);
        free(entry);
    }
}

static sym_cache_entry_t sym_cache_entry_create (char *sym, addr_t va)
{
    sym_cache_entry_t entry = (sym_cache_entry_t) safe_malloc(sizeof(struct sym_cache_entry));
    entry->sym = strdup(sym);
    entry->va = va;
    entry->last_used = time(NULL);
    return entry;
}

void sym_cache_init (vmi_instance_t vmi)
{
    vmi->sym_cache = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, sym_cache_entry_free);
}

void sym_cache_destroy (vmi_instance_t vmi)
{
    g_hash_table_unref(vmi->sym_cache);
}

status_t sym_cache_get (vmi_instance_t vmi, char *sym, addr_t *va)
{
    sym_cache_entry_t entry = NULL;

    if ((entry = g_hash_table_lookup(vmi->sym_cache, sym)) != NULL){
        entry->last_used = time(NULL);
        *va = entry->va;
        dbprint("--SYM cache hit %s -- 0x%.8x\n", sym, *va);
        return VMI_SUCCESS;
    }

    return VMI_FAILURE;
}

void sym_cache_set (vmi_instance_t vmi, char *sym, addr_t va)
{
    sym_cache_entry_t entry = sym_cache_entry_create(sym, va);
    g_hash_table_insert(vmi->sym_cache, sym, entry);
    dbprint("--SYM cache set %s -- 0x%.8x\n", sym, va);
}

status_t sym_cache_del (vmi_instance_t vmi, char *sym)
{
    dbprint("--SYM cache del %s\n", sym);
    if (TRUE == g_hash_table_remove(vmi->sym_cache, sym)){
        return VMI_SUCCESS;
    }
    else{
        return VMI_FAILURE;
    }
}

//
// Virtual address --> Physical address cache implementation
struct v2p_cache_entry{
    addr_t va;
    addr_t dtb;
    addr_t pa;
    time_t last_used;
};
typedef struct v2p_cache_entry *v2p_cache_entry_t;

static void v2p_cache_key_free (gpointer data)
{
    if (data) free(data);
}

static void v2p_cache_entry_free (gpointer data)
{
    v2p_cache_entry_t entry = (v2p_cache_entry_t) data;
    if (entry) free(entry);
}

static v2p_cache_entry_t v2p_cache_entry_create (addr_t va, addr_t dtb, addr_t pa)
{
    v2p_cache_entry_t entry = (v2p_cache_entry_t) safe_malloc(sizeof(struct v2p_cache_entry));
    entry->va = va;
    entry->dtb = dtb;
    entry->pa = pa;
    entry->last_used = time(NULL);
    return entry;
}

//TODO this function assumes a 32-bit address, will need to fix this for 64-bit support
static gint64 *v2p_build_key (vmi_instance_t vmi, uint32_t va, uint32_t dtb)
{
    uint64_t *key = (uint64_t *) safe_malloc(sizeof(uint64_t));
    *key = 0;
    *key |= ((uint64_t) (va & ~(vmi->page_size - 1))) << 32;
    *key |= ((uint64_t) dtb);
    return (gint64 *) key;
}

void v2p_cache_init (vmi_instance_t vmi)
{
    vmi->v2p_cache = g_hash_table_new_full(g_int64_hash, g_int64_equal, v2p_cache_key_free, v2p_cache_entry_free);
}

void v2p_cache_destroy (vmi_instance_t vmi)
{
    g_hash_table_unref(vmi->v2p_cache);
}

status_t v2p_cache_get (vmi_instance_t vmi, addr_t va, addr_t dtb, addr_t *pa)
{
    v2p_cache_entry_t entry = NULL;
    gint64 *key = v2p_build_key(vmi, va, dtb);

    if ((entry = g_hash_table_lookup(vmi->v2p_cache, key)) != NULL){
        entry->last_used = time(NULL);
        *pa = entry->pa | ((vmi->page_size - 1) & va);
        dbprint("--V2P cache hit 0x%.8x -- 0x%.8x (0x%.16llx)\n", va, *pa, *key);
        return VMI_SUCCESS;
    }

    return VMI_FAILURE;
}

void v2p_cache_set (vmi_instance_t vmi, addr_t va, addr_t dtb, addr_t pa)
{
    if (!va || !dtb || !pa){
        return;
    }
    gint64 *key = v2p_build_key(vmi, va, dtb);
    pa &= ~(vmi->page_size - 1);
    v2p_cache_entry_t entry = v2p_cache_entry_create(va, dtb, pa);
    g_hash_table_insert(vmi->v2p_cache, key, entry);
    dbprint("--V2P cache set 0x%.8x -- 0x%.8x (0x%.16llx)\n", va, pa, *key);
}

status_t v2p_cache_del (vmi_instance_t vmi, addr_t va, addr_t dtb)
{
    gint64 *key = v2p_build_key(vmi, va, dtb);
    dbprint("--V2P cache del 0x%.8x (0x%.16llx)\n", va, *key);
    if (TRUE == g_hash_table_remove(vmi->v2p_cache, key)){
        free(key);
        return VMI_SUCCESS;
    }
    else{
        free(key);
        return VMI_FAILURE;
    }
}
