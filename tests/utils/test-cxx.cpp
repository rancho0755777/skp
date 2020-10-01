#include <skp/utils/utils.h>
#include <skp/utils/spinlock.h>
#include <skp/utils/mutex.h>
#include <skp/utils/rwsem.h>
#include <skp/utils/rwlock.h>
#include <skp/utils/uref.h>
#include <skp/utils/pbuff.h>
#include <skp/utils/seqlock.h>
#include <skp/utils/bitmap.h>
#include <skp/utils/bitops.h>
#include <skp/utils/atomic.h>
#include <skp/utils/hash.h>
#include <skp/utils/mutex.h>
#include <skp/utils/string.h>
#include <skp/utils/cpumask.h>
#include <skp/process/thread.h>
#include <skp/process/event.h>
#include <skp/process/wait.h>
#include <skp/process/signal.h>
#include <skp/process/workqueue.h>
#include <skp/process/completion.h>
#include <skp/mm/pgalloc.h>
#include <skp/adt/idr.h>
#include <skp/adt/rbtree.h>
#include <skp/adt/radix_tree.h>
#include <skp/adt/vector.h>
#include <skp/adt/list.h>
#include <skp/adt/slist.h>
#include <skp/adt/heap.h>
#include <skp/adt/hlist_table.h>

#include <iostream>
#include <chrono>
#include <algorithm>
#include <thread>
#include <map>
#include <vector>
#include <list>

#include <skp/mm/slab.h>

int main(void)
{
    log_info("test CXX compile");
    return EXIT_SUCCESS;
}
