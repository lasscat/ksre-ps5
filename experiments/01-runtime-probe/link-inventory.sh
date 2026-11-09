#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Link-time inventory of libc/POSIX functions for the PS5 payload target.
#
# CPython's configure decides most HAVE_* macros with link tests, which run on
# the build machine even when cross-compiling. This script performs the same
# kind of test with the SDK's prospero-clang so the CPython bring-up starts
# from evidence. "link=yes" proves only that some SDK library or stub exports
# the name; whether the console's firmware provides it at run time is checked
# by the probe payload itself (dl.symbols lines).
#
# Usage: link-inventory.sh OUTPUT_DIR
set -euo pipefail

out_dir=${1:?usage: link-inventory.sh OUTPUT_DIR}
: "${PS5_PAYLOAD_SDK:?PS5_PAYLOAD_SDK must point at a ps5-payload-sdk install}"
cc="${PS5_PAYLOAD_SDK}/bin/prospero-clang"
[ -x "$cc" ] || { echo "missing $cc" >&2; exit 2; }
"${PS5_PAYLOAD_SDK}/bin/prospero-llvm-config" --version >/dev/null 2>&1 || {
  echo "prospero-llvm-config cannot find a host LLVM (install clang + lld)" >&2; exit 2; }

mkdir -p "$out_dir"
work=$(mktemp -d "${out_dir}/work.XXXXXX")
trap 'rm -rf "$work"' EXIT

# Functions CPython 3.12's configure/posixmodule, its extension modules, and
# Ren'Py's C code commonly depend on. Grouped for readability only.
funcs=(
  # process control
  fork vfork execv execve posix_spawn posix_spawnp waitpid wait3 wait4 kill killpg
  getpid getppid getpgid setsid setpgid getsid nice getpriority setpriority
  # files and directories
  openat fstatat fchmodat fchownat linkat mkdirat mkfifoat mknodat readlinkat
  renameat symlinkat unlinkat utimensat futimens futimes lutimes fdopendir
  realpath readlink symlink link lchown lchmod chroot truncate ftruncate
  fsync fdatasync posix_fallocate posix_fadvise pread pwrite preadv pwritev
  copy_file_range sendfile statvfs fstatvfs getcwd dup3 pipe2 mkfifo mknod
  # memory and dynamic loading
  mmap munmap mprotect madvise mremap memfd_create shm_open shm_unlink
  dlopen dlsym dlerror dladdr
  # threads and synchronisation
  pthread_create pthread_join pthread_detach pthread_kill pthread_sigmask
  pthread_condattr_setclock pthread_getcpuclockid pthread_getattr_np
  pthread_attr_getstack sem_open sem_unlink sem_timedwait sem_clockwait
  sched_yield sched_get_priority_max sched_setaffinity
  # time
  clock_gettime clock_getres clock_settime clock_nanosleep nanosleep
  gettimeofday localtime_r gmtime_r timegm strftime wcsftime tzset times
  setitimer getitimer
  # signals
  sigaction sigaltstack siginterrupt sigpending sigwait sigwaitinfo
  sigtimedwait sigfillset sigsetjmp siglongjmp alarm pause
  # users, terminals, environment
  getpwuid getpwnam getpwuid_r getpwnam_r getgrgid getgrnam getgrouplist
  getgroups getlogin getuid geteuid setuid seteuid ttyname isatty openpty
  forkpty tcgetattr tcsetattr ctermid setenv unsetenv
  # sockets and networking
  socket socketpair accept4 getaddrinfo getnameinfo gethostbyname_r
  inet_aton inet_pton if_nameindex getifaddrs kqueue kevent epoll_create1
  poll select
  # locale, text, entropy, misc
  setlocale nl_langinfo iconv_open mbrtowc wcscoll wcsxfrm strsignal
  getentropy getrandom arc4random_buf getloadavg uname confstr sysconf
  fpathconf backtrace posix_memalign aligned_alloc
)

ok=0
missing=()
report="${out_dir}/link-inventory.txt"
: > "$report"
for f in "${funcs[@]}"; do
  cat > "${work}/t.c" <<EOF
char ${f}(void);
int main(void) { return (int)(long)&${f}; }
EOF
  if "$cc" -w -o "${work}/t.elf" "${work}/t.c" >/dev/null 2>"${work}/err.txt"; then
    echo "link=yes ${f}" >> "$report"
    ok=$((ok + 1))
  else
    echo "link=no  ${f}" >> "$report"
    missing+=("$f")
  fi
done

echo "link inventory: ${ok}/${#funcs[@]} functions link for x86_64-sie-ps5"
echo "not linkable (${#missing[@]}): ${missing[*]+"${missing[*]}"}"
echo "full report: ${report}"
