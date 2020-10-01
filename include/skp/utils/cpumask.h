#ifndef __US_CPUMASK_H__
#define __US_CPUMASK_H__

#include "bitmap.h"

__BEGIN_DECLS

#ifndef CONFIG_CPU_CORES
# define NR_CPUS 8
#else
# if CONFIG_CPU_CORES < 2
#  define NR_CPUS 2
# else
#  define NR_CPUS ((int)roundup_pow_of_two(CONFIG_CPU_CORES))
# endif
#endif

#define NR_CPUS_SHIFT (ilog2(NR_CPUS))

typedef struct cpumask { DECLARE_BITMAP(bits, NR_CPUS); } cpumask_t;

static inline int cpumask_next(int n, const cpumask_t *mask)
{
	return (int)find_next_bit(&mask->bits[0], NR_CPUS, n + 1);
}

static inline void cpumask_set(int n, cpumask_t *mask)
{
	__set_bit(n, &mask->bits[0]);
}

extern const cpumask_t *cpu_online_mask(void);
extern const cpumask_t *cpu_possible_mask(void);

#define for_each_cpu(cpu, mask) \
	for((cpu) = -1; (cpu) = cpumask_next((cpu), (mask)), cpu < NR_CPUS;)

#define for_each_online_cpu(cpu) for_each_cpu(cpu, cpu_online_mask())
#define for_each_possible_cpu(cpu) for_each_cpu(cpu, cpu_possible_mask())

#define DEFINE_PER_CPU(type, name) __typeof__(type) name[NR_CPUS]
#define DECLARE_PER_CPU(type, name) extern __typeof__(type) name[]
#define DEFINE_PER_CPU_AIGNED(type, name) \
	DEFINE_PER_CPU(type, name) __cacheline_aligned

#define per_cpu(var, cpu) (var[(cpu)])

/*仅仅为了提示，这是一个 percpu 变量，需要特殊的解码操作*/
#define __percpu

struct percpu_data {
	void *ptrs[NR_CPUS];
};

extern void *__alloc_percpu(size_t elem_size, size_t align);
extern void free_percpu(void *ptr);

#define alloc_percpu(type) \
	((typeof(type) *)__alloc_percpu(sizeof(type), __alignof__(type)))

/*解码per-cpu中的数据指针*/
#define per_cpu_ptr(ptr, cpu) ({	\
	struct percpu_data *__p = (struct percpu_data *)~(unsigned long)(ptr); \
	(__typeof__(ptr))__p->ptrs[(cpu)]; })

__END_DECLS

#endif
