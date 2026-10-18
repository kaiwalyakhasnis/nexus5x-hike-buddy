// hike-buddy-reboot [bootloader|recovery|...]: reboot with an argument, which
// busybox reboot can't pass. LK reads it (e.g. "bootloader" -> fastboot).
#include <linux/reboot.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char** argv) {
  sync();
  if (argc > 1)
    return syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                   LINUX_REBOOT_CMD_RESTART2, argv[1]);
  return syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                 LINUX_REBOOT_CMD_RESTART, 0);
}
