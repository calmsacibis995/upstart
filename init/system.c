/* upstart
 *
 * system.c - core system functions
 *
 * Copyright © 2010 Canonical Ltd.
 * Author: Scott James Remnant <scott@netsplit.com>.
 * 
 * Portions Copyright (c) 2022 Stefanos Stefanidis.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif /* HAVE_CONFIG_H */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

#include <nih/macros.h>
#include <nih/alloc.h>
#include <nih/string.h>
#include <nih/error.h>
#include <nih/logging.h>

#include "paths.h"
#include "system.h"
#include "job_class.h"


/**
 * system_kill:
 * @pid: process id of process,
 * @signal: signal to send.
 *
 * Send all processes in the same process group as @pid, which may not
 * necessarily be the group leader the @signal.
 *
 * Returns: zero on success, negative value on raised error.
 **/
int
system_kill (pid_t pid,
	     int   signal)
{
	pid_t pgid;

	nih_assert (pid > 0);

	pgid = getpgid (pid);

	if (kill (pgid > 0 ? -pgid : pid, signal) < 0)
		nih_return_system_error (-1);

	return 0;
}


/**
 * system_setup_console:
 * @type: console type,
 * @reset: reset console to sane defaults.
 *
 * Set up the standard input, output and error file descriptors for the
 * current process based on the console @type given.  If @reset is TRUE then
 * the console device will be reset to sane defaults.
 *
 * Returns: zero on success, negative value on raised error.
 **/
int
system_setup_console (ConsoleType type,
		      int         reset)
{
	int fd = -1, i;

	/* Close the standard file descriptors since we're about to re-open
	 * them; it may be that some of these aren't already open, we get
	 * called in some very strange ways.
	 */
	for (i = 0; i < 3; i++)
		close (i);

	/* Open the new first file descriptor, which should always become
	 * file zero.
	 */
	switch (type) {
	case CONSOLE_OUTPUT:
	case CONSOLE_OWNER:
		/* Ordinary console input and output */
		fd = open (CONSOLE, O_RDWR | O_NOCTTY);
		if (fd < 0)
			nih_return_system_error (-1);

		if (type == CONSOLE_OWNER)
			ioctl (fd, TIOCSCTTY, 1);
		break;
		/* FALLTHROUGH */
	case CONSOLE_LOG:
	case CONSOLE_NONE:
		/* No console really means /dev/null */
		fd = open (DEV_NULL, O_RDWR | O_NOCTTY);
		if (fd < 0)
			nih_return_system_error (-1);
		break;
	}

	/* Reset to sane defaults, cribbed from sysvinit, initng, etc. */
	if (reset) {
		struct termios tty;

		tcgetattr (0, &tty);

		tty.c_cflag &= (CBAUD | CBAUDEX | CSIZE | CSTOPB
				| PARENB | PARODD);
		tty.c_cflag |= (HUPCL | CLOCAL | CREAD);

		/* Set up usual keys */
		tty.c_cc[VINTR]  = 3;   /* ^C */
		tty.c_cc[VQUIT]  = 28;  /* ^\ */
		tty.c_cc[VERASE] = 127;
		tty.c_cc[VKILL]  = 24;  /* ^X */
		tty.c_cc[VEOF]   = 4;   /* ^D */
		tty.c_cc[VTIME]  = 0;
		tty.c_cc[VMIN]   = 1;
		tty.c_cc[VSTART] = 17;  /* ^Q */
		tty.c_cc[VSTOP]  = 19;  /* ^S */
		tty.c_cc[VSUSP]  = 26;  /* ^Z */

		/* Pre and post processing */
		tty.c_iflag = (IGNPAR | ICRNL | IXON | IXANY);
		tty.c_oflag = (OPOST | ONLCR);
		tty.c_lflag = (ISIG | ICANON | ECHO | ECHOCTL
			       | ECHOPRT | ECHOKE);

		/* Set the terminal line and flush it */
		tcsetattr (0, TCSANOW, &tty);
		tcflush (0, TCIOFLUSH);
	}

	/* Copy to standard output and standard error */
	while (dup (fd) < 2)
		;

	return 0;
}


/**
 * system_mount:
 * @type: filesystem type,
 * @dir: mountpoint,
 * @flags: mount flags,
 * @options: mount options.
 *
 * Mount the kernel filesystem @type at @dir with @flags and mount options
 * @options, if not already mounted.  This is used to ensure that the proc
 * and sysfs filesystems are always available.
 *
 * Returns: zero on success, negative value on raised error.
 **/
int
system_mount (const char *type,
	      const char *dir,
	      unsigned long flags,
	      const char *options)
{
	nih_local char *parent = NULL;
	char *          ptr;
	struct stat     parent_stat;
	struct stat     dir_stat;

	nih_assert (type != NULL);
	nih_assert (dir != NULL);

	/* Stat the parent directory of the mountpoint to obtain the dev_t */
	ptr = strrchr (dir, '/');
	nih_assert (ptr != NULL);

	parent = NIH_MUST (nih_strndup (NULL, dir, ptr == dir ? 1 : ptr - dir));
	if (stat (parent, &parent_stat) < 0)
		nih_return_system_error (-1);

	/* Also stat the mountpoint to obtain the dev_t */
	if (stat (dir, &dir_stat) < 0)
		nih_return_system_error (-1);

	/* If the two dev_ts do not match, then there is already a filesystem
	 * mounted and we needn't do anything.
	 */
	if (parent_stat.st_dev != dir_stat.st_dev)
		return 0;

	/* Mount the filesystem */
	if (mount ("none", dir, type, flags, options) < 0)
		nih_return_system_error (-1);

	return 0;
}

/**
 * system_mknod:
 *
 * @path: full path,
 * @mode: mode to create device with,
 * @dev: device major and minor numbers.
 *
 * Create specified device.
 *
 * Note that depending on the device, if an error occurs
 * it may not be reportable, hence no return value,
 * but an attempt to display an error.
 **/
void
system_mknod (const char *path, mode_t mode, dev_t dev)
{
	nih_assert (path);

	if (mknod (path, mode, dev) < 0 && errno != EEXIST)
		nih_error ("%s: %s", _("Unable to create device"), path);
}

/**
 * system_check_file:
 *
 * @path: full path,
 * @type: file type,
 * @dev: device major and minor numbers (only checked for character and
 * block devices).
 *
 * Perform checks on specified file.
 *
 * Returns: 0 if device exists and has the specified @path,
 * @type and @dev attributes, else -1.
 **/
int
system_check_file (const char *path, mode_t type, dev_t dev)
{
	struct stat  statbuf;
	int          ret;

	nih_assert (path);

	ret = stat (path, &statbuf);

	if (ret < 0 || ! ((statbuf.st_mode & S_IFMT) == type))
		return -1;

	if (type == S_IFCHR || type == S_IFBLK) {
		if (major (statbuf.st_rdev) != major (dev)
			|| minor (statbuf.st_rdev) != minor (dev))
		return -1;
	}

	return 0;
}
