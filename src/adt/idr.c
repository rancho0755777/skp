#include <skp/adt/idr.h>
#include <skp/mm/slab.h>

#define BITS_PER_PAGE_MASK (BITS_PER_PAGE - 1)

static inline bool check_idt(struct idt *idt, int id)
{
	if (skp_unlikely(id < 0 || id < idt->offset ||
			id >= idt->offset + idt->nr_bit))
		return false;
	return true;
}

int idt_init(struct idt *idt, uint32_t start, uint32_t end)
{
	struct idmap *idmap;
	uint32_t i, pages, last_bits;

	idt->last = U32_MAX;
	idt->offset = start;
	idt->nr_bit = end - start + 1;
	atomic_set(&idt->nr_free, idt->nr_bit);

	BUG_ON(end <= start);
	BUG_ON(start >= S32_MAX);
	BUG_ON(end >= S32_MAX);
	BUG_ON(idt->nr_bit >= S32_MAX);

	pages = DIV_ROUND_UP(idt->nr_bit, BITS_PER_PAGE);
	idmap = malloc(sizeof(*idmap) * pages);
	if (skp_unlikely(!idmap))
		return -ENOMEM;

	for (i = 0; i < pages - 1; i++) {
		idmap[i].page = NULL;
		idmap[i].nr_bit = BITS_PER_PAGE;
		atomic_set(&idmap[i].nr_free, BITS_PER_PAGE);
	}

	/*最后一个可能没有那么多的可分配位*/
	last_bits = (idt->nr_bit & BITS_PER_PAGE_MASK)?:BITS_PER_PAGE;
	idmap[i].page = NULL;
	idmap[i].nr_bit = last_bits;
	atomic_set(&idmap[i].nr_free, last_bits);
	idt->idmap = idmap;

	log_debug("initilize integer id allocator , "
		"offset %u, bits %u, last_bits : %u", idt->offset, idt->nr_bit,
		last_bits);
	return 0;
}

void idt_destroy(struct idt *idt)
{
	uint32_t i, pages = DIV_ROUND_UP(idt->nr_bit, BITS_PER_PAGE);
	uint32_t last_bits = (idt->nr_bit & BITS_PER_PAGE_MASK)?:BITS_PER_PAGE;

	if (skp_unlikely(!idt->idmap))
		return;

	WARN_ON(atomic_read(&idt->nr_free) != idt->nr_bit);

	for (i = 0; i < pages - 1; i++) {
		WARN_ON(atomic_read(&idt->idmap[i].nr_free) != BITS_PER_PAGE);
		if (idt->idmap[i].page)
			free(idt->idmap[i].page);
	}

	WARN_ON(atomic_read(&idt->idmap[i].nr_free) != last_bits);
	if (idt->idmap[i].page)
		free(idt->idmap[i].page);

	free(idt->idmap);
}

#define mk_id(map, offset, idt) \
	((uint32_t)(map - idt->idmap) * BITS_PER_PAGE + (uint32_t)offset)

int idt_next(struct idt *idt, int id)
{
	struct idmap *idmap;
	unsigned long offset;
	uint32_t pages = DIV_ROUND_UP(idt->nr_bit, BITS_PER_PAGE);

	if (!check_idt(idt, id))
		return -ENOENT;

	id -= idt->offset;
	offset = id & BITS_PER_PAGE_MASK;
	idmap = &idt->idmap[id / BITS_PER_PAGE];

	for (; idmap < &idt->idmap[pages]; idmap++) {
		if (idmap->page) {
			offset = find_next_bit(idmap->page, idmap->nr_bit, offset);
			if (offset < idmap->nr_bit) {
				id = mk_id(idmap, offset, idt);
				return id + idt->offset;
			}
		}
		offset = 0;
	}

	return -ENOENT;
}

int idt_alloc(struct idt *idt)
{
	struct idmap *idmap;
	unsigned long offset;
	uint32_t id, max_scan, last = idt->last;
	uint32_t pages = DIV_ROUND_UP(idt->nr_bit, BITS_PER_PAGE);

	if (skp_unlikely(!atomic_read(&idt->nr_free)))
		return -ENOSPC;

	id = last + 1;
	if (id >= idt->nr_bit)
		id = 0;

	offset = id & BITS_PER_PAGE_MASK;
	idmap = &idt->idmap[id / BITS_PER_PAGE];
	max_scan = pages - !offset;

	for (uint32_t i = 0; i <= max_scan; i++) {
		if (skp_unlikely(!READ_ONCE(idmap->page))) {
			void *page = malloc(PAGE_SIZE);
			if (skp_unlikely(!page))
				break;
			memset(page, 0, PAGE_SIZE);
			big_lock();
			if (skp_likely(!idmap->page)) {
				WRITE_ONCE(idmap->page, page);
			} else {
				free(page);
			}
			big_unlock();
		}

		if (skp_likely(atomic_read(&idmap->nr_free))) {
			/*无锁分配*/
			for (;;) {
				if (!test_and_set_bit(offset, idmap->page)) {
					/*成功分配*/
					atomic_dec(&idmap->nr_free);
					atomic_dec(&idt->nr_free);
					WRITE_ONCE(idt->last, id);
					return (int)(id + idt->offset);
				}
				offset = find_next_zero_bit(
					idmap->page, idmap->nr_bit, offset);
				if (offset >= idmap->nr_bit)
					break;
				id = mk_id(idmap, offset, idt);
			}
		}

		if (idmap < &idt->idmap[pages - 1]) {
			idmap++;
			offset = 0;
		} else {
			idmap = &idt->idmap[0];
			offset = 0;
		}
		id = mk_id(idmap, offset, idt);
	}

	return -ENOSPC;
}

bool __idt_remove(struct idt *idt, int id, bool ring)
{
	unsigned long offset;
	struct idmap *idmap;

	if (!check_idt(idt, id))
		return false;

	id -= idt->offset;
	offset = id & BITS_PER_PAGE_MASK;
	idmap = &idt->idmap[id / BITS_PER_PAGE];
	if (WARN_ON(!idmap->page))
		return false;
	if (WARN_ON(!test_and_clear_bit(offset, idmap->page)))
		return false;
	/*回收起始ID？ */
	if (ring && id <= idt->last && idt->last != U32_MAX)
		idt->last = ((uint32_t)id) - 1;
	atomic_inc(&idt->nr_free);
	atomic_inc(&idmap->nr_free);
	return true;
}

int idr_alloc(struct idr *idr, void *ptr)
{
	int rc, id;
	uint32_t iid;

	if (WARN_ON(!ptr))
		return -EINVAL;

	id = idt_alloc(&idr->idt);
	if (skp_unlikely(id < 0))
		return id;
	iid = id - idr->idt.offset;
	rc = radix_tree_insert(&idr->rdtree, iid, ptr);
	if (skp_unlikely(rc)) {
		BUG_ON(rc == -EEXIST);
		idt_remove(&idr->idt, id);
		return -ENOMEM;
	}
	return id;
}

void *idr_remove(struct idr *idr, int id)
{
	void *ptr;
	uint32_t iid = id - idr->idt.offset;
	if (!check_idt(&idr->idt, id))
		return NULL;

	ptr = radix_tree_delete(&idr->rdtree, iid);
	if (skp_unlikely(!ptr))
		return NULL;

	BUG_ON(!idt_remove(&idr->idt, id));

	return ptr;
}

void *idr_find(struct idr *idr, int id)
{
	uint32_t iid = id - idr->idt.offset;
	if (!check_idt(&idr->idt, id))
		return NULL;
	return radix_tree_lookup(&idr->rdtree, iid);
}

int idr_init_base(struct idr *idr, uint32_t start, uint32_t end)
{
	int rc = idt_init(&idr->idt, start, end);
	if (skp_unlikely(rc))
		return rc;
	INIT_RADIX_TREE(&idr->rdtree);
	return 0;
}

static inline uint32_t __bit2id(unsigned long bit, struct idt *idt, uint32_t idx)
{
	return (uint32_t)bit + idt->offset + idx * BITS_PER_PAGE;
}

void idr_destroy(struct idr *idr)
{
	void *ptr;
	unsigned long bit;
	struct idt *idt = &idr->idt;
	struct idmap *idmap = idt->idmap;
	uint32_t i, pages, last_bits;
	pages = DIV_ROUND_UP(idt->nr_bit, BITS_PER_PAGE);
	for (i = 0; i < pages - 1; i++) {
		if (atomic_read(&idmap[i].nr_free) == BITS_PER_PAGE)
			continue;
		for_each_set_bit(bit, idmap[i].page, BITS_PER_PAGE) {
			ptr = idr_remove(idr, __bit2id(bit, idt, i));
			BUG_ON(!ptr);
		}
	}
	last_bits = (idt->nr_bit & BITS_PER_PAGE_MASK)?:BITS_PER_PAGE;
	if (atomic_read(&idmap[i].nr_free) != last_bits) {
		for_each_set_bit(bit, idmap[i].page, last_bits) {
			ptr = idr_remove(idr, __bit2id(bit, idt, i));
			BUG_ON(!ptr);
		}
	}
	BUG_ON(idr->rdtree.rnode);

	idt_destroy(&idr->idt);
}
