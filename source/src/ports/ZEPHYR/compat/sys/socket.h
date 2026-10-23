/*
 * OpENer's endianconv.c includes <netinet/in.h> and <sys/socket.h> on every non-Windows, non-STM32 target.
 * Only OpENer is compiled with this directory on its include path; both names lead to the native mapping.
 */
#include "platform_network_includes.h"
