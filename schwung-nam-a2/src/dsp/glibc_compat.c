/*
 * Symbols the build host's glibc/libstdc++ pull in that Move's older glibc
 * does not export.
 *
 * Move's rootfs tops out at GLIBC_2.34 (measured from the shipped
 * schwung-nam module, which loads on the device: its highest versioned
 * reference is @GLIBC_2.34 and it has no unversioned ones at all). A .so
 * that references anything above that, or anything unversioned that the
 * device's libc lacks, fails RTLD_NOW with no output the device can show -
 * the module simply never loads, which is exactly the symptom this fixes.
 *
 * scripts/build.sh now checks the linked .so against that ceiling and fails
 * the build rather than shipping one that cannot load, so this file only
 * has to cover what the toolchain insists on emitting.
 *
 * arc4random: glibc 2.36+. Reached from libstdc++'s std::random_device,
 * which is linked in statically. getrandom(2) has been in the kernel since
 * 3.17 and in glibc since 2.25, so it is available on Move; the urandom
 * path is a fallback for a refusing kernel rather than a real expectation.
 *
 * __isoc23_strto*: glibc 2.38+ headers redirect strtol and friends to these
 * C23 variants. They should no longer appear now that every phase of the
 * build compiles against the jammy (2.35) sysroot, but a dependency that
 * reaches a host header some other way would reintroduce them silently, and
 * they cost four lines to neutralise. The C23 variants differ from the C17
 * ones only in accepting a 0b/0B binary prefix under base 0 or 2, which
 * nothing here parses.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/random.h>
#include <errno.h>

long          __isoc23_strtol(const char *p, char **e, int b)   { return strtol(p, e, b); }
long long     __isoc23_strtoll(const char *p, char **e, int b)  { return strtoll(p, e, b); }
unsigned long __isoc23_strtoul(const char *p, char **e, int b)  { return strtoul(p, e, b); }
unsigned long long __isoc23_strtoull(const char *p, char **e, int b) { return strtoull(p, e, b); }

void arc4random_buf(void *buf, size_t n)
{
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;

    while (got < n) {
        ssize_t r = getrandom(p + got, n - got, 0);
        if (r > 0) { got += (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        break;
    }
    if (got == n) return;

    /* getrandom refused; fall back to /dev/urandom, then to a value that is
     * at least not a constant. Nothing in this module uses randomness for
     * anything but libstdc++'s hash-table seeding. */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        while (got < n) {
            ssize_t r = read(fd, p + got, n - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fd);
    }
    for (; got < n; got++)
        p[got] = (unsigned char)(got * 31u + 17u);
}

uint32_t arc4random(void)
{
    uint32_t v = 0;
    arc4random_buf(&v, sizeof(v));
    return v;
}
