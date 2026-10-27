#include <string.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "kernel/memory.h"
#include "kernel/mm.h"

struct mm *mm_new(void) {
    struct mm *mm = malloc(sizeof(struct mm));
    if (mm == NULL)
        return NULL;
    mem_init(&mm->mem);
    mm->start_brk = mm->brk = 0; // should get overwritten by exec
    mm->exefile = NULL;
    mm->refcount = 1;
    return mm;
}

struct mm *mm_copy(struct mm *mm) {
    struct mm *new_mm = malloc(sizeof(struct mm));
    if (new_mm == NULL)
        return NULL;
    *new_mm = *mm;
    // Fix wrlock_init failing because it thinks it's reinitializing the same lock
    memset(&new_mm->mem.lock, 0, sizeof(new_mm->mem.lock));
    new_mm->refcount = 1;
    mem_init(&new_mm->mem);
    fd_retain(new_mm->exefile);
    write_wrlock(&mm->mem.lock);
    pt_copy_on_write(&mm->mem, &new_mm->mem, 0, MEM_PAGES);
    write_wrunlock(&mm->mem.lock);
    return new_mm;
}

void mm_retain(struct mm *mm) {
    mm->refcount++;
}

void mm_release(struct mm *mm) {
    if (--mm->refcount == 0) {
        if (mm->exefile != NULL)
            fd_close(mm->exefile);
        mem_destroy(&mm->mem);
        free(mm);
    }
}

static addr_t do_mmap(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset) {
    int err;
    pages_t pages = PAGE_ROUND_UP(len);
    if (!pages) return _EINVAL;
    page_t page;
    if (addr != 0) {
        if (PGOFFSET(addr) != 0)
            return _EINVAL;
        page = PAGE(addr);
        if (!(flags & MMAP_FIXED) && !pt_is_hole(current->mem, page, pages)) {
            addr = 0;
        }
    } else {
        page = pt_find_hole(current->mem, pages);
        if (page == BAD_PAGE)
            return _ENOMEM;
    }

    if (flags & MMAP_SHARED)
        prot |= P_SHARED;

    if (flags & MMAP_ANONYMOUS) {
        if ((err = pt_map_nothing(current->mem, page, pages, prot)) < 0)
            return err;
    } else {
        // fd must be valid
        struct fd *fd = f_get(fd_no);
        if (fd == NULL)
            return _EBADF;
        if (fd->ops->mmap == NULL)
            return _ENODEV;
        if ((err = fd->ops->mmap(fd, current->mem, page, pages, offset, prot, flags)) < 0)
            return err;
        mem_pt(current->mem, page)->data->fd = fd_retain(fd);
        mem_pt(current->mem, page)->data->file_offset = offset;
    }
    return page << PAGE_BITS;
}

static addr_t mmap_common(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset) {
    STRACE("mmap(0x%x, 0x%x, 0x%x, 0x%x, %d, %d)", addr, len, prot, flags, fd_no, offset);
    if (len == 0)
        return _EINVAL;
    if (prot & ~P_RWX)
        return _EINVAL;
    if ((flags & MMAP_PRIVATE) && (flags & MMAP_SHARED))
        return _EINVAL;

    write_wrlock(&current->mem->lock);
    addr_t res = do_mmap(addr, len, prot, flags, fd_no, offset);
    write_wrunlock(&current->mem->lock);
    return res;
}

addr_t sys_mmap2(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset) {
    return mmap_common(addr, len, prot, flags, fd_no, offset << PAGE_BITS);
}

struct mmap_arg_struct {
    dword_t addr, len, prot, flags, fd, offset;
};

addr_t sys_mmap(addr_t args_addr) {
    struct mmap_arg_struct args;
    if (user_get(args_addr, args))
        return _EFAULT;
    return mmap_common(args.addr, args.len, args.prot, args.flags, args.fd, args.offset);
}

int_t sys_munmap(addr_t addr, uint_t len) {
    STRACE("munmap(0x%x, 0x%x)", addr, len);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (len == 0)
        return _EINVAL;
    write_wrlock(&current->mem->lock);
    int err = pt_unmap_always(current->mem, PAGE(addr), PAGE_ROUND_UP(len));
    write_wrunlock(&current->mem->lock);
    if (err < 0)
        return _EINVAL;
    return 0;
}

#define MREMAP_MAYMOVE_ 1
#define MREMAP_FIXED_ 2

// Map more of a file mapping at page, continuing a file mapping that has
// pt_flags and starts at file_offset.
static int mremap_map_file(struct fd *fd, page_t page, pages_t pages, size_t file_offset, dword_t pt_flags) {
    int flags = pt_flags & P_SHARED ? MMAP_SHARED : MMAP_PRIVATE;
    int err = fd->ops->mmap(fd, current->mem, page, pages, file_offset, pt_flags & (P_RWX | P_SHARED), flags);
    if (err < 0)
        return err;
    mem_pt(current->mem, page)->data->fd = fd_retain(fd);
    mem_pt(current->mem, page)->data->file_offset = file_offset;
    return 0;
}

static int_t do_mremap(addr_t addr, dword_t old_len, dword_t new_len, dword_t flags) {
    pages_t old_pages = PAGE_ROUND_UP(old_len);
    pages_t new_pages = PAGE_ROUND_UP(new_len);
    if (new_pages == 0)
        return _EINVAL;

    // shrinking always works
    if (new_pages <= old_pages) {
        if (new_pages < old_pages) {
            int err = pt_unmap(current->mem, PAGE(addr) + new_pages, old_pages - new_pages);
            if (err < 0)
                return _EFAULT;
        }
        return addr;
    }

    struct pt_entry *entry = mem_pt(current->mem, PAGE(addr));
    if (entry == NULL)
        return _EFAULT;
    dword_t pt_flags = entry->flags;
    struct data *data = entry->data;
    for (page_t page = PAGE(addr); page < PAGE(addr) + old_pages; page++) {
        entry = mem_pt(current->mem, page);
        if (entry == NULL || entry->flags != pt_flags)
            return _EFAULT;
    }

    page_t extra_start = PAGE(addr) + old_pages;
    pages_t extra_pages = new_pages - old_pages;
    bool can_grow = pt_is_hole(current->mem, extra_start, extra_pages);

    if (pt_flags & P_ANONYMOUS) {
        if (!can_grow)
            return _ENOMEM;
        int err = pt_map_nothing(current->mem, extra_start, extra_pages, pt_flags);
        if (err < 0)
            return err;
        return addr;
    }

    // File mapping, e.g. a wl_shm pool being resized by a compositor
    struct fd *fd = data->fd;
    if (fd == NULL || fd->ops->mmap == NULL) {
        FIXME("mremap grow on non-file non-anonymous mapping");
        return _EFAULT;
    }
    struct pt_entry *first = mem_pt(current->mem, PAGE(addr));
    size_t file_offset = data->file_offset + first->offset - (data->file_offset % real_page_size);

    if (can_grow) {
        int err = mremap_map_file(fd, extra_start, extra_pages,
                file_offset + ((size_t) old_pages << PAGE_BITS), pt_flags);
        if (err < 0)
            return err;
        return addr;
    }

    if (!(flags & MREMAP_MAYMOVE_))
        return _ENOMEM;
    // Moving a private mapping would lose any modifications made to it
    if (!(pt_flags & P_SHARED)) {
        FIXME("mremap move private file mapping");
        return _ENOMEM;
    }
    page_t new_page = pt_find_hole(current->mem, new_pages);
    if (new_page == BAD_PAGE)
        return _ENOMEM;
    // keep the fd alive while the old mapping goes away
    fd_retain(fd);
    int err = mremap_map_file(fd, new_page, new_pages, file_offset, pt_flags);
    if (err >= 0)
        pt_unmap(current->mem, PAGE(addr), old_pages);
    fd_close(fd);
    if (err < 0)
        return err;
    return new_page << PAGE_BITS;
}

int_t sys_mremap(addr_t addr, dword_t old_len, dword_t new_len, dword_t flags) {
    STRACE("mremap(%#x, %#x, %#x, %d)", addr, old_len, new_len, flags);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (flags & ~(MREMAP_MAYMOVE_ | MREMAP_FIXED_))
        return _EINVAL;
    if (flags & MREMAP_FIXED_) {
        FIXME("missing MREMAP_FIXED");
        return _EINVAL;
    }
    write_wrlock(&current->mem->lock);
    int_t res = do_mremap(addr, old_len, new_len, flags);
    write_wrunlock(&current->mem->lock);
    return res;
}

int_t sys_mprotect(addr_t addr, uint_t len, int_t prot) {
    STRACE("mprotect(0x%x, 0x%x, 0x%x)", addr, len, prot);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (prot & ~P_RWX)
        return _EINVAL;
    pages_t pages = PAGE_ROUND_UP(len);
    write_wrlock(&current->mem->lock);
    int err = pt_set_flags(current->mem, PAGE(addr), pages, prot);
    write_wrunlock(&current->mem->lock);
    return err;
}

dword_t sys_madvise(addr_t UNUSED(addr), dword_t UNUSED(len), dword_t UNUSED(advice)) {
    // portable applications should not rely on linux's destructive semantics for MADV_DONTNEED.
    return 0;
}

dword_t sys_mbind(addr_t UNUSED(addr), dword_t UNUSED(len), int_t UNUSED(mode),
        addr_t UNUSED(nodemask), dword_t UNUSED(maxnode), uint_t UNUSED(flags)) {
    return 0;
}

int_t sys_mlock(addr_t UNUSED(addr), dword_t UNUSED(len)) {
    return 0;
}

int_t sys_msync(addr_t UNUSED(addr), dword_t UNUSED(len), int_t UNUSED(flags)) {
    return 0;
}

addr_t sys_brk(addr_t new_brk) {
    STRACE("brk(0x%x)", new_brk);
    struct mm *mm = current->mm;

    write_wrlock(&mm->mem.lock);
    if (new_brk < mm->start_brk)
        goto out;
    addr_t old_brk = mm->brk;

    if (new_brk > old_brk) {
        // expand heap: map region from old_brk to new_brk
        // round up because of the definition of brk: "the first location after the end of the uninitialized data segment." (brk(2))
        // if the brk is 0x2000, page 0x2000 shouldn't be mapped, but it should be if the brk is 0x2001.
        page_t start = PAGE_ROUND_UP(old_brk);
        pages_t size = PAGE_ROUND_UP(new_brk) - PAGE_ROUND_UP(old_brk);
        if (!pt_is_hole(&mm->mem, start, size))
            goto out;
        int err = pt_map_nothing(&mm->mem, start, size, P_WRITE);
        if (err < 0)
            goto out;
    } else if (new_brk < old_brk) {
        // shrink heap: unmap region from new_brk to old_brk
        // first page to unmap is PAGE(new_brk)
        // last page to unmap is PAGE(old_brk)
        pt_unmap_always(&mm->mem, PAGE(new_brk), PAGE(old_brk) - PAGE(new_brk));
    }

    mm->brk = new_brk;
out:;
    addr_t brk = mm->brk;
    write_wrunlock(&mm->mem.lock);
    return brk;
}
