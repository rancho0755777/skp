#include <sys/stat.h>
#include <skp/utils/utils.h>

int main(int argc, char **argv)
{
	int rc, fd;
	struct stat st;
	if (argc!=2) {
out:
		printf("usage test-fchg <dir/file>\n");
		return EXIT_FAILURE;
	}

	rc = stat(argv[1], &st);
	if (rc < 0) {

		goto out;
	}

	if (S_ISREG(st.st_mode)) {
		fd = open(argv[1], O_RDWR);
	} else if (S_ISDIR(st.st_mode)) {
		fd = open(argv[1], O_RDONLY);
	} else {
		goto out;
	}

	if (fd < 0)
		goto out;

	do {
		int ev = file_changed(fd, 1000);
		if (ev == fchg_failed) {
			if (errno==ETIMEDOUT)
				continue;
			log_error("file_changed() failed : %s", strerror_local());
			break;
		}

		if (ev&fchg_write)
			printf("-- content of vnode was changed\n");

		if (ev&fchg_rename)
			printf("-- vnode was rename\n");

		if (ev&fchg_attrib)
			printf("-- attributes of vnode was changed\n");

		if (ev&fchg_extend)
			printf("-- size of vnode was increased\n");

		if (ev&fchg_delete) {
			printf("-- vnode was delete\n");
			break;
		}

	} while (1);

	close(fd);

	return EXIT_SUCCESS;
}
