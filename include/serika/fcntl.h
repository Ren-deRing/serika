#pragma once

#define O_ACCMODE   00000003
#define O_RDONLY    00000000
#define O_WRONLY    00000001
#define O_RDWR      00000002
#define O_CREAT     00000100
#define O_NOCTTY    00000400
#define O_EXCL      00000200
#define O_TRUNC     00001000
#define O_APPEND    00002000
#define O_NONBLOCK  00004000
#define O_DIRECTORY 00200000
#define O_NOFOLLOW  00400000
#define O_PATH      010000000 /* the link or file itself, to stat it. nothing can be read or written */
#define O_CLOEXEC   02000000

#define AT_FDCWD            -100
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_EMPTY_PATH       0x1000

#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030
#define F_ADD_SEALS     1033
#define F_GET_SEALS     1034

#define FD_CLOEXEC 1

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
