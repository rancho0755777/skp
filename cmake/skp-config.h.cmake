#ifndef __skp_config_h__
#define __skp_config_h__

#cmakedefine SPINLOCK_DEBUG 1
#cmakedefine RWLOCK_DEBUG 1
#cmakedefine MUTEX_DEBUG 1
#cmakedefine RWSEM_DEBUG 1
#cmakedefine BUDDY_DEBUG 1
#cmakedefine SLAB_DEBUG 1
#cmakedefine DICT_DEBUG 1
#cmakedefine EVENT_SINGLE 1
#cmakedefine UMALLOC_MANGLE 1
#cmakedefine ENABLE_SSL 1

#define CONFIG_CPU_CORES @OS_CPU_CORES@

#endif