#include <skp/utils/utils.h>
#include <skp/mm/slab.h>

const cpumask_t *cpu_online_mask(void)
{
	int cores = get_cpu_cores();
	static cpumask_t online_mask = {};
	
	BUG_ON(cores > NR_CPUS);	
	if (!test_bit(0, online_mask.bits)) {
		big_lock();
		if (!test_bit(0, online_mask.bits)) {
			bitmap_fill(online_mask.bits, cores);
		}
		big_unlock();
	}
	
	return &online_mask;
}

const cpumask_t *cpu_possible_mask(void)
{
	static cpumask_t possible_mask = {};
	
	if (!test_bit(0, possible_mask.bits)) {
		big_lock();
		if (!test_bit(0, possible_mask.bits)) {
			bitmap_fill(possible_mask.bits, NR_CPUS);
		}
		big_unlock();
	}
	
	return &possible_mask;
}

void *__alloc_percpu(size_t elem_size, size_t align)
{
	int i = 0;
	struct percpu_data *data = malloc(sizeof(*data));
	if (skp_unlikely(!data))
		return NULL;

	memset(data, 0, sizeof(*data));
	for (i = 0; i < NR_CPUS; i++) {
		data->ptrs[i] = aligned_alloc(align, elem_size);
		if (skp_unlikely(!data->ptrs[i]))
			goto oom;
	}
	
	return (void*)(~(unsigned long)data);
oom:
	while (i-- > 0) {
		free(data->ptrs[i]);
	}
	free(data);
	return NULL;
}

void free_percpu(void *addr)
{
	struct percpu_data *p = 
		(struct percpu_data *) (~(unsigned long) addr);

	for (int i = 0; i < NR_CPUS; i++) {
		free(p->ptrs[i]);
	}
	free(p);
}
