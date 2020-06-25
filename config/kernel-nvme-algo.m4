dnl #
dnl # Check if kernel has NVMe algo support
dnl # This is used to offload compression to NoLoad
dnl # accelerators
AC_DEFUN([ZFS_AC_KERNEL_SRC_NVME_ALGO], [
	ZFS_LINUX_TEST_SRC([nvme_algo], [
		#include <linux/module.h>
	],[
		#ifndef CONFIG_NVME_ALGO
		#error "NVME ALGO not supported"
		#endif
	])
])

AC_DEFUN([ZFS_AC_KERNEL_NVME_ALGO], [
	AC_MSG_CHECKING([whether kernel has NVMe algo support])
	ZFS_LINUX_TEST_RESULT([nvme_algo], [
		AC_MSG_RESULT(yes)
		AC_DEFINE(HAVE_NVME_ALGO, 1,
			[kernel has nvme algo support])
	],[
		AC_MSG_RESULT(no)
	])
])
