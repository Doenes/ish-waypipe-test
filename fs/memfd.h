#ifndef FS_MEMFD_H
#define FS_MEMFD_H

#include <stdbool.h>
#include "misc.h"
#include "fs/fd.h"

#define MEMFD_NAME_MAX 249

#define MFD_CLOEXEC_ 0x1
#define MFD_ALLOW_SEALING_ 0x2
#define MFD_HUGETLB_ 0x4
#define MFD_NOEXEC_SEAL_ 0x8
#define MFD_EXEC_ 0x10

#define F_ADD_SEALS_ 1033
#define F_GET_SEALS_ 1034

#define F_SEAL_SEAL_ 0x1
#define F_SEAL_SHRINK_ 0x2
#define F_SEAL_GROW_ 0x4
#define F_SEAL_WRITE_ 0x8
#define F_SEAL_FUTURE_WRITE_ 0x10
#define F_SEAL_EXEC_ 0x20
#define F_SEAL_ALL_ (F_SEAL_SEAL_ | F_SEAL_SHRINK_ | F_SEAL_GROW_ | \
        F_SEAL_WRITE_ | F_SEAL_FUTURE_WRITE_ | F_SEAL_EXEC_)

fd_t sys_memfd_create(addr_t name_addr, uint_t flags);

bool is_memfd(struct fd *fd);
int memfd_add_seals(struct fd *fd, unsigned seals);
int memfd_get_seals(struct fd *fd);

#endif
