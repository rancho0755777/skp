#include <pthread.h>
#include <skp/adt/radix_tree.h>

int main(int argc, char **argv)
{
	/*测试三层上一些特殊的位置*/
	unsigned long indexs[] = {
		0, 1024, 2049, 4095, 4096, 5121, 6144, 8188, 8190, 8192,
	};
	unsigned long tags_0[] = {
		0, 1024, 2049, 4096, 6144, 8192,
	};
	unsigned long tags_0_no[] = {
		4095, 5121, 8190,
	};
	unsigned long tags_1[] = {
		1024, 2049, 4095, 5121, 8188, 8190, 8192,
	};
	unsigned long tags_1_no[] = {
		0, 4096, 6144,
	};

	RADIX_TREE(root);

	for (int i = 0; i < ARRAY_SIZE(indexs); i++) {
		int rc = radix_tree_insert(&root, indexs[i],
				(void*)((uintptr_t)indexs[i] + 1));
		BUG_ON(rc);
	}

	void *ptr = radix_tree_lookup(&root, indexs[0]);
	BUG_ON(ptr != (void*)((uintptr_t)indexs[0] + 1));

	void *result[ARRAY_SIZE(indexs)];

	unsigned nr = radix_tree_gang_lookup(&root, 1023, result, ARRAY_SIZE(result));
	BUG_ON(nr != ARRAY_SIZE(indexs) - 1);

	for (int i = 0; i < nr; i++) {
		BUG_ON(result[i] != (void*)((uintptr_t)indexs[i + 1] + 1));
	}

	int j = 0;
	radix_tree_for_each(ptr, &root, 0) {
		BUG_ON(ptr != (void*)((uintptr_t)indexs[j] + 1));
		BUG_ON(radix_tree_iter_index() != indexs[j]);
		j++;
	}

	for (int i = 0; i < ARRAY_SIZE(tags_0); i++) {
		ptr = radix_tree_tag_set(&root, tags_0[i], 0);
		BUG_ON(ptr != (void*)((uintptr_t)tags_0[i] + 1));
	}

	for (int i = 0; i < ARRAY_SIZE(tags_1); i++) {
		ptr = radix_tree_tag_set(&root, tags_1[i], 1);
		BUG_ON(ptr != (void*)((uintptr_t)tags_1[i] + 1));
	}

	BUG_ON(!radix_tree_tagged(&root, 0));
	BUG_ON(!radix_tree_tagged(&root, 1));

	for (int i = 0; i < ARRAY_SIZE(tags_0); i++) {
		BUG_ON(!radix_tree_tag_get(&root, tags_0[i], 0));
	}

	for (int i = 0; i < ARRAY_SIZE(tags_1); i++) {
		BUG_ON(!radix_tree_tag_get(&root, tags_1[i], 1));
	}

	for (int i = 0; i < ARRAY_SIZE(tags_0_no); i++) {
		BUG_ON(radix_tree_tag_get(&root, tags_0_no[i], 0));
	}

	for (int i = 0; i < ARRAY_SIZE(tags_1_no); i++) {
		BUG_ON(radix_tree_tag_get(&root, tags_1_no[i], 1));
	}

	radix_tree_insert(&root, 1U<<20, 0xffUL);

	nr = radix_tree_gang_lookup_tag(&root, 1024, result, ARRAY_SIZE(result), 0);
	BUG_ON(nr != ARRAY_SIZE(tags_0) - 1);

	for (int i = 0; i < nr; i++) {
		BUG_ON(result[i] != (void*)((uintptr_t)tags_0[i + 1] + 1));
	}

	nr = radix_tree_gang_lookup_tag(&root, 0, result, ARRAY_SIZE(tags_1) - 1, 1);
	BUG_ON(nr != ARRAY_SIZE(tags_1) - 1);

	for (int i = 0; i < nr; i++) {
		BUG_ON(result[i] != (void*)((uintptr_t)tags_1[i] + 1));
	}

	j = 0;
	radix_tree_for_each_tagged(ptr, &root, 0, 0) {
		BUG_ON(ptr != (void*)((uintptr_t)tags_0[j] + 1));
		BUG_ON(radix_tree_iter_index() != tags_0[j]);
		j++;
	}

	j = 0;
	radix_tree_for_each_tagged(ptr, &root, 0, 1) {
		BUG_ON(ptr != (void*)((uintptr_t)tags_1[j] + 1));
		BUG_ON(radix_tree_iter_index() != tags_1[j]);
		j++;
	}

	for (int i = 0; i < ARRAY_SIZE(tags_0); i++) {
		ptr = radix_tree_tag_clear(&root, tags_0[i], 0);
		BUG_ON(ptr != (void*)((uintptr_t)tags_0[i] + 1));
	}

	radix_tree_release(&root, NULL, NULL);

	pthread_exit(NULL);
	
	return EXIT_SUCCESS;
}
