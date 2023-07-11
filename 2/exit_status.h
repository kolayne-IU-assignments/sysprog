#include <sys/wait.h>

#define EXITSTATUS_DEFAULT 0

#if !WIFEXITED(EXITSTATUS_default) || (0 != WEXITSTATUS(EXITSTATUS_default))
#error The value of `EXITSTATUS_DEFAULT` does not satisfy the expected properties
#endif

#define EXITSTATUS_BEDA (255 << 8)

#if !WIFEXITED(EXITSTATUS_BEDA) || (255 != WEXITSTATUS(EXITSTATUS_BEDA))
#error The value of `EXITSTATUS_BEDA` does not satisfy the expected properties
#endif
