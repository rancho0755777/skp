#ifdef _WIN32
# include <windows.h>
#else
# include <unistd.h>
# include <sys/syscall.h>
#endif
#include <stdio.h>
int main(int argc, const char** argv)
{
    int result;
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    result = si.dwNumberOfProcessors;
#else
    result = sysconf(_SC_NPROCESSORS_CONF);
#endif
    printf("%d", result/2);
    return 0;
}