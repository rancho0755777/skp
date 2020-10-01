#include <math.h>
#include <skp/utils/utils.h>
#include <skp/utils/string.h>

int main(void)
{
	/*创建文件夹*/
{
}
{
	int rc;
	char path[64];
	snprintf(path, sizeof(path), "///tmp///ensure/1///2/3///dir%d///", get_process_id());

	rc = make_dir(path, 0755);
	BUG_ON(rc);
	rc = make_dir(path, 0700);
	BUG_ON(rc!=-EEXIST);
}

{
	int rc;
	int64_t v_i;
	int64_t v_f;

	rc = str2int("-01", -1, &v_i);
	assert(rc==1);
	assert(v_i==-1);
	rc = str2float("0.02999359", -1, &v_f, 10000);
	assert(!rc);
	assert(v_f==299);
	rc = str2float("0.02999359", -1, &v_f, 10);
	assert(!rc);
	assert(v_f==0);
	rc = str2float("1.201", -1, &v_f, 10000);
	assert(!rc);
	assert(v_f==12010);
	rc = str2float("1.201", -1, &v_f, 100);
	assert(!rc);
	assert(v_f==120);
	rc = str2float("1.201E3", -1, &v_f, 10000);
	assert(!rc);
	assert(v_f==12010000);
	rc = str2float("1.201E3", -1, &v_f, 1);
	assert(!rc);
	assert(v_f==1201);
	rc = str2float("1.201E-3", -1, &v_f, 1);
	assert(rc < 0);
	rc = str2float("1.201E-3", -1, &v_f, 1000);
	assert(!rc);
	assert(v_f==1);
	rc = str2float("1.201E-3", -1, &v_f, 10000);
	assert(!rc);
	assert(v_f==12);
	rc = str2float("0.0201E-3", -1, &v_f, 100000);
	assert(rc < 0);
	rc = str2float("1.0201E-3", -1, &v_f, 100000);
	assert(!rc);
	assert(v_f==102);
}

{
	char array1[5];
	BUILD_BUG_ON(ARRAY_SIZE(array1) != 5);

	char array2[4][5];
	BUILD_BUG_ON(ARRAY_SIZE(array2) != 4);

	BUILD_BUG_ON(round_up(1, 4) != 4);
	BUILD_BUG_ON(round_up(5, 4) != 8);
	BUILD_BUG_ON(round_down(3, 4) != 0);
	BUILD_BUG_ON(round_down(9, 8) != 8);

	BUILD_BUG_ON(roundup(1, 3) != 3);
	BUILD_BUG_ON(roundup(5, 7) != 7);
	BUILD_BUG_ON(rounddown(9, 7) != 7);
	BUILD_BUG_ON(rounddown(15, 7) != 14);

	BUILD_BUG_ON(min_t(int, 1, 11) != 1);
	BUILD_BUG_ON(min3(1, 22, 11) != 1);

	BUILD_BUG_ON(max_t(int, 1, 11) != 11);
	BUILD_BUG_ON(max3(1, 22, 11) != 22);
}

{

	int v1 = 1;
	int v2 = 2;
	swap_t(int, v1, v2);
	BUG_ON(v1 != 2);
	BUG_ON(v2 != 1);
}

{
	int v1 = 1, v2 = 11, v3 = 1111;
	BUG_ON(clamp_t(int, v1, 11, 111) != 11);
	BUG_ON(clamp_t(int, v2, 1, 111) != 11);
	BUG_ON(clamp_t(int, v3, 1, 111) != 111);
}

{
	log_info("|-> %8.2f", 0.2f);
	log_info("test log ...");
	log_info("test clockfreq : %u ...", get_clockfreq());
	log_info("test sterror_local : %s ...", __strerror_local(EINVAL));
	log_info("test prandom integer : %ld ...", prandom_int(1, 100000));
	log_info("test prandom real : %.4lf ...", prandom_real(1, 100000));
	log_info("test prandom chance : %s ...", prandom_chance(.3f) ? "true" : "false");
	log_info("test timestamp : %llu ...", abstime(0, 0));
	log_info("test similar timestamp : %llu ...", similar_abstime(0, 0));
	log_info("test timestamp : %llu ...", calendartime(0, 0));
	log_info("test similar timestamp : %llu ...", similar_calendartime(0, 0));
}

{
	int v1;
	v1 = get_cpu_cores();
	BUG_ON(v1 < 0);
	log_info("test get cpu cores : %d ...", v1);
	v1 = thread_bind(-1);
	BUG_ON(v1< 0);
	log_info("test bind thread : %d ...", v1);
	v1 = get_thread_id();
	BUG_ON(v1 < 0);
	log_info("test get thread id : %d ...", v1);
	v1 = get_process_id();
	BUG_ON(v1 < 0);
	log_info("test get process id : %d ...", v1);
}

{
	const char *ptr;
	time_t t = time(NULL);
	ptr = format_time(0, 0, t, TM_FORMAT_DATETIME);
	BUG_ON(!ptr);
	log_info("test format time : %s ...", ptr);
	time_t ti = parse_time(ptr, TM_FORMAT_DATETIME);
	BUG_ON(ti == -1);
	log_info("test parse time : %ld ...", ti);
	BUG_ON(ti!=t);
}
	return 0;
}
