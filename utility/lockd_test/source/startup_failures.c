#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* POSIX account declarations use the IDs declared by sys/types.h. */
#include <sys/types.h>
#include <grp.h>
#include <unistd.h>

static int fail_operation(const char* const operation)
{
    const char* const requested = getenv("LOCKD_TEST_FAIL");
    if (requested && strcmp(requested, operation) == 0) {
        errno = EPERM; // NOLINT(misc-include-cleaner): POSIX errno.h exports this OS error constant.
        return -1;
    }
    return 0;
}

/* Replace only the OS boundary. The production startup code decides whether to continue. */
int initgroups(const char* const user, const gid_t group)
{
    (void)user;
    (void)group;
    return fail_operation("initgroups");
}

int setgid(const gid_t gid)
{
    (void)gid;
    return fail_operation("setgid");
}

int setuid(const uid_t uid)
{
    (void)uid;
    return fail_operation("setuid");
}

pid_t fork(void)
{
    errno = EAGAIN; // NOLINT(misc-include-cleaner): POSIX errno.h exports this OS error constant.
    return -1;
}
