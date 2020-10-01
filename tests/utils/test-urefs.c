#include <skp/utils/uref.h>

static bool call_release = false;

static void release(uref_t *ref)
{
	BUG_ON(!ref);
	call_release = true;
}

int main(void)
{
	uref_t refs;
	uref_init(&refs);	
	BUG_ON(!uref_read(&refs));	
	uref_get(&refs);
	BUG_ON(uref_read(&refs) != 2);	
	__uref_put(&refs);	
	BUG_ON(uref_read(&refs) != 1);	
	uref_put(&refs, release);	
	BUG_ON(!call_release);	
	BUG_ON(uref_get_unless_zero(&refs));

	return 0;
}