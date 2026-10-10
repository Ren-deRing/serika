#pragma once

#define SYS_EXIT 60

long syscall_dispatch(long nr, const long args[6]);
