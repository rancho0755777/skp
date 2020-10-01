#include <skp/adt/vector.h>

struct student {
	char name;
	uint8_t age;
	char gender;
} __aligned(sizeof(int));

static int __compare(const char *elem, const char *key)
{
	struct student *s = (struct student*)elem;
	return s->name == *key ? 0 : -1;
}

const struct vector_ops vec_ops = {
	.compare_elem = __compare,
};

DEFINE_VECTOR(s_vec, sizeof(struct student), &vec_ops);

int main(void)
{
	int rc;
	char name, *ptr, *old;
	for (char i = 'a'; i <= 'z'; i++) {
		struct student s = { i , 30 + i - 'a', i & 1 ? '\x1' : '\x2' };
		old = __vector_insert(&s_vec, &s.name, (const char *)&s);
		BUG_ON(old);
	}

	struct student s = { 'a' , 30, '\x2' };
	old = __vector_insert(&s_vec, "a", (const char *)&s);
	BUG_ON(!old);

	rc = __vector_remove(&s_vec, "a", (void*)&s);
	BUG_ON(rc);
	BUG_ON(s.name != 'a');
	rc = __vector_remove(&s_vec, "$", (void*)&s);
	BUG_ON(!rc);
	rc = __vector_remove(&s_vec, "z", (void*)&s);
	BUG_ON(rc);
	BUG_ON(s.name != 'z');

	name = 'b';
	vector_for_each_elem(ptr, &s_vec) {
		struct student *sptr = (struct student *)ptr;
		BUG_ON(name != sptr->name);
		name += 1;
		log_debug("student : %c, %u, %c", sptr->name, sptr->age,
			sptr->gender == '\x1' ? 'f' :
				(sptr->gender == '\x2' ? 'm' : (assert(0), 'x')));
	}

	vector_release(&s_vec, 0, 0);

	return 0;
}
