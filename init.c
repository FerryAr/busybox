/* vi: set sw=4 ts=4: */
/*
 * Mini init implementation for busybox
 *
 *
 * Copyright (C) 1995, 1996 by Bruce Perens <bruce@pixar.com>.
 * Adjusted by so many folks, it's impossible to keep track.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include "internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <paths.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/kdaemon.h>
#include <sys/sysmacros.h>
#include <asm/types.h>
#include <linux/serial.h>		/* for serial_struct */
#include <sys/vt.h>				/* for vt_stat */
#include <sys/ioctl.h>
#include <linux/version.h>
#ifdef BB_SYSLOGD
#include <sys/syslog.h>
#endif

#if ! defined BB_FEATURE_USE_PROCFS
#error Sorry, I depend on the /proc filesystem right now.
#endif

#ifndef KERNEL_VERSION
#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))
#endif


#define VT_PRIMARY      "/dev/tty1"	/* Primary virtual console */
#define VT_SECONDARY    "/dev/tty2"	/* Virtual console */
#define VT_LOG          "/dev/tty3"	/* Virtual console */
#define SERIAL_CON0     "/dev/ttyS0"	/* Primary serial console */
#define SERIAL_CON1     "/dev/ttyS1"	/* Serial console */
#define SHELL           "/bin/sh"	/* Default shell */
#define INITTAB         "/etc/inittab"	/* inittab file location */
#ifndef INIT_SCRIPT
#define INIT_SCRIPT	"/etc/init.d/rcS"	/* Default sysinit script. */
#endif

#define LOG             0x1
#define CONSOLE         0x2

/* Allowed init action types */
typedef enum {
	SYSINIT = 1,
	RESPAWN,
	ASKFIRST,
	WAIT,
	ONCE
} initActionEnum;

/* And now a list of the actions we support in the version of init */
typedef struct initActionType {
	const char *name;
	initActionEnum action;
} initActionType;

static const struct initActionType actions[] = {
	{"sysinit", SYSINIT},
	{"respawn", RESPAWN},
	{"askfirst", ASKFIRST},
	{"wait", WAIT},
	{"once", ONCE},
	{0}
};

/* Set up a linked list of initactions, to be read from inittab */
typedef struct initActionTag initAction;
struct initActionTag {
	pid_t pid;
	char process[256];
	char console[256];
	initAction *nextPtr;
	initActionEnum action;
};
initAction *initActionList = NULL;


static char *secondConsole = VT_SECONDARY;
static char *log = VT_LOG;
static int kernelVersion = 0;
static char termType[32] = "TERM=ansi";
static char console[32] = _PATH_CONSOLE;


/* print a message to the specified device:
 * device may be bitwise-or'd from LOG | CONSOLE */
void message(int device, char *fmt, ...)
{
	va_list arguments;
	int fd;

#ifdef BB_SYSLOGD

	/* Log the message to syslogd */
	if (device & LOG) {
		char msg[1024];

		va_start(arguments, fmt);
		vsnprintf(msg, sizeof(msg), fmt, arguments);
		va_end(arguments);
		openlog("init", 0, LOG_DAEMON);
		syslog(LOG_DAEMON | LOG_NOTICE, msg);
		closelog();
	}
#else
	static int log_fd = -1;

	/* Take full control of the log tty, and never close it.
	 * It's mine, all mine!  Muhahahaha! */
	if (log_fd < 0) {
		if (log == NULL) {
			/* don't even try to log, because there is no such console */
			log_fd = -2;
			/* log to main console instead */
			device = CONSOLE;
		} else if ((log_fd = device_open(log, O_RDWR | O_NDELAY)) < 0) {
			log_fd = -1;
			fprintf(stderr, "Bummer, can't write to log on %s!\r\n", log);
			fflush(stderr);
			return;
		}
	}
	if ((device & LOG) && (log_fd >= 0)) {
		va_start(arguments, fmt);
		vdprintf(log_fd, fmt, arguments);
		va_end(arguments);
	}
#endif

	if (device & CONSOLE) {
		/* Always send console messages to /dev/console so people will see them. */
		if (
			(fd =
			 device_open(_PATH_CONSOLE,
						 O_WRONLY | O_NOCTTY | O_NDELAY)) >= 0) {
			va_start(arguments, fmt);
			vdprintf(fd, fmt, arguments);
			va_end(arguments);
			close(fd);
		} else {
			fprintf(stderr, "Bummer, can't print: ");
			va_start(arguments, fmt);
			vfprintf(stderr, fmt, arguments);
			fflush(stderr);
			va_end(arguments);
		}
	}
}


/* Set terminal settings to reasonable defaults */
void set_term(int fd)
{
	struct termios tty;
	static const char control_characters[] = {
		'\003', '\034', '\177', '\025', '\004', '\0',
		'\1', '\0', '\021', '\023', '\032', '\0', '\022',
		'\017', '\027', '\026', '\0'
	};

	tcgetattr(fd, &tty);

	/* set control chars */
	memcpy(tty.c_cc, control_characters, sizeof(control_characters));

	/* use line dicipline 0 */
	tty.c_line = 0;

	/* Make it be sane */
	//tty.c_cflag &= CBAUD|CBAUDEX|CSIZE|CSTOPB|PARENB|PARODD;
	//tty.c_cflag |= HUPCL|CLOCAL;

	/* input modes */
	tty.c_iflag = ICRNL | IXON | IXOFF;

	/* output modes */
	tty.c_oflag = OPOST | ONLCR;

	/* local modes */
	tty.c_lflag =
		ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;

	tcsetattr(fd, TCSANOW, &tty);
}

/* How much memory does this machine have? */
static int mem_total()
{
	char s[80];
	char *p = "/proc/meminfo";
	FILE *f;
	const char pattern[] = "MemTotal:";

	if ((f = fopen(p, "r")) < 0) {
		message(LOG, "Error opening %s: %s\n", p, strerror(errno));
		return -1;
	}
	while (NULL != fgets(s, 79, f)) {
		p = strstr(s, pattern);
		if (NULL != p) {
			fclose(f);
			return (atoi(p + strlen(pattern)));
		}
	}
	return -1;
}

static void console_init()
{
	int fd;
	int tried_devcons = 0;
	int tried_vtprimary = 0;
	struct serial_struct sr;
	char *s;

	if ((s = getenv("TERM")) != NULL) {
		snprintf(termType, sizeof(termType) - 1, "TERM=%s", s);
	}

	if ((s = getenv("CONSOLE")) != NULL) {
		snprintf(console, sizeof(console) - 1, "%s", s);
	}
#if #cpu(sparc)
	/* sparc kernel supports console=tty[ab] parameter which is also 
	 * passed to init, so catch it here */
	else if ((s = getenv("console")) != NULL) {
		/* remap tty[ab] to /dev/ttyS[01] */
		if (strcmp(s, "ttya") == 0)
			snprintf(console, sizeof(console) - 1, "%s", SERIAL_CON0);
		else if (strcmp(s, "ttyb") == 0)
			snprintf(console, sizeof(console) - 1, "%s", SERIAL_CON1);
	}
#endif
	else {
		struct vt_stat vt;

		/* 2.2 kernels: identify the real console backend and try to use it */
		if (ioctl(0, TIOCGSERIAL, &sr) == 0) {
			/* this is a serial console */
			snprintf(console, sizeof(console) - 1, "/dev/ttyS%d", sr.line);
		} else if (ioctl(0, VT_GETSTATE, &vt) == 0) {
			/* this is linux virtual tty */
			snprintf(console, sizeof(console) - 1, "/dev/tty%d",
					 vt.v_active);
		} else {
			snprintf(console, sizeof(console) - 1, "%s", _PATH_CONSOLE);
			tried_devcons++;
		}
	}

	while ((fd = open(console, O_RDONLY | O_NONBLOCK)) < 0) {
		/* Can't open selected console -- try /dev/console */
		if (!tried_devcons) {
			tried_devcons++;
			snprintf(console, sizeof(console) - 1, "%s", _PATH_CONSOLE);
			continue;
		}
		/* Can't open selected console -- try vt1 */
		if (!tried_vtprimary) {
			tried_vtprimary++;
			snprintf(console, sizeof(console) - 1, "%s", VT_PRIMARY);
			continue;
		}
		break;
	}
	if (fd < 0) {
		/* Perhaps we should panic here? */
		snprintf(console, sizeof(console) - 1, "/dev/null");
	} else {
		/* check for serial console and disable logging to tty3 & running a
		   * shell to tty2 */
		if (ioctl(0, TIOCGSERIAL, &sr) == 0) {
			message(LOG | CONSOLE,
					"serial console detected.  Disabling virtual terminals.\r\n");
			log = NULL;
			secondConsole = NULL;
		}
		close(fd);
	}
	message(LOG, "console=%s\n", console);
}

static pid_t run(char *command, char *terminal, int get_enter)
{
	int i, fd;
	pid_t pid;
	char *tmpCmd;
	char *cmd[255];
	static const char press_enter[] =

		"\nPlease press Enter to activate this console. ";
	char *environment[] = {
		"HOME=/",
		"PATH=/usr/bin:/bin:/usr/sbin:/sbin",
		"SHELL=/bin/sh",
		termType,
		"USER=root",
		0
	};


	if ((pid = fork()) == 0) {
		pid_t shell_pgid = getpid();

		/* Clean up */
		close(0);
		close(1);
		close(2);
		setsid();

		/* Reset signal handlers set for parent process */
		signal(SIGUSR1, SIG_DFL);
		signal(SIGUSR2, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGHUP, SIG_DFL);

		if ((fd = device_open(terminal, O_RDWR)) < 0) {
			message(LOG | CONSOLE, "Bummer, can't open %s\r\n", terminal);
			exit(1);
		}
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		tcsetpgrp(0, getpgrp());
		set_term(0);

		if (get_enter == TRUE) {
			/*
			 * Save memory by not exec-ing anything large (like a shell)
			 * before the user wants it. This is critical if swap is not
			 * enabled and the system has low memory. Generally this will
			 * be run on the second virtual console, and the first will
			 * be allowed to start a shell or whatever an init script 
			 * specifies.
			 */
			char c;

			message(LOG, "Waiting for enter to start '%s' (pid %d, console %s)\r\n",
					command, shell_pgid, terminal);
			write(fileno(stdout), press_enter, sizeof(press_enter) - 1);
			read(fileno(stdin), &c, 1);
		}

		/* Log the process name and args */
		message(LOG, "Starting pid %d, console %s: '",
				shell_pgid, terminal, command);

		/* Convert command (char*) into cmd (char**, one word per string) */
		for (tmpCmd = command, i = 0;
			 (tmpCmd = strsep(&command, " \t")) != NULL;) {
			if (*tmpCmd != '\0') {
				cmd[i] = tmpCmd;
#ifdef DEBUG_INIT
				message(LOG, "%s ", tmpCmd);
#endif
				tmpCmd++;
				i++;
			}
		}
		cmd[i] = NULL;
		message(LOG, "'\r\n");

		/* Now run it.  The new program will take over this PID, 
		 * so nothing further in init.c should be run. */
		execve(cmd[0], cmd, environment);

		/* We're still here?  Some error happened. */
		message(LOG | CONSOLE, "Bummer, could not run '%s': %s\n", cmd[0],
				strerror(errno));
		exit(-1);
	}
	return pid;
}

static int waitfor(char *command, char *terminal, int get_enter)
{
	int status, wpid;
	int pid = run(command, terminal, get_enter);

	while (1) {
		wpid = wait(&status);
		if (wpid > 0) {
			message(LOG, "Process '%s' (pid %d) exited.\n", command, wpid);
			break;
		}
		if (wpid == pid)
			break;
	}
	return wpid;
}

/* Make sure there is enough memory to do something useful. *
 * Calls swapon if needed so be sure /proc is mounted. */
static void check_memory()
{
	struct stat statBuf;

	if (mem_total() > 3500)
		return;

	if (stat("/etc/fstab", &statBuf) == 0) {
		/* Try to turn on swap */
		waitfor("/bin/swapon swapon -a", log, FALSE);
		if (mem_total() < 3500)
			goto goodnight;
	} else
		goto goodnight;
	return;

  goodnight:
	message(CONSOLE,
			"Sorry, your computer does not have enough memory.\r\n");
	while (1)
		sleep(1);
}

#ifndef DEBUG_INIT
static void shutdown_system(void)
{
	/* first disable our SIGHUP signal */
	signal(SIGHUP, SIG_DFL);

	/* Allow Ctrl-Alt-Del to reboot system. */
	reboot(RB_ENABLE_CAD);
	message(CONSOLE, "\r\nThe system is going down NOW !!\r\n");
	sync();

	/* Send signals to every process _except_ pid 1 */
	message(CONSOLE, "Sending SIGTERM to all processes.\r\n");
	kill(-1, SIGTERM);
	sleep(5);
	sync();

	message(CONSOLE, "Sending SIGKILL to all processes.\r\n");
	kill(-1, SIGKILL);
	sleep(5);

	message(CONSOLE, "Disabling swap.\r\n");
	waitfor("swapoff -a", console, FALSE);
	message(CONSOLE, "Unmounting filesystems.\r\n");
	waitfor("umount -a -r", console, FALSE);
	sync();
	if (kernelVersion > 0 && kernelVersion <= 2 * 65536 + 2 * 256 + 11) {
		/* bdflush, kupdate not needed for kernels >2.2.11 */
		bdflush(1, 0);
		sync();
	}
}

static void halt_signal(int sig)
{
	shutdown_system();
	message(CONSOLE,
			"The system is halted. Press %s or turn off power\r\n",
			(secondConsole == NULL)	/* serial console */
			? "Reset" : "CTRL-ALT-DEL");
	sync();

	/* allow time for last message to reach serial console */
	sleep(5);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,2,0)
	if (sig == SIGUSR2)
		reboot(RB_POWER_OFF);
	else
#endif
		reboot(RB_HALT_SYSTEM);
	exit(0);
}

static void reboot_signal(int sig)
{
	shutdown_system();
	message(CONSOLE, "Please stand by while rebooting the system.\r\n");
	sync();

	/* allow time for last message to reach serial console */
	sleep(2);

	reboot(RB_AUTOBOOT);
	exit(0);
}

#if defined BB_FEATURE_INIT_CHROOT
static void check_chroot(int sig)
{
	char *argv_init[2] = { "init", NULL, };
	char *envp_init[3] = { "HOME=/", "TERM=linux", NULL, };
	char rootpath[256], *tc;
	int fd;

	if ((fd = open("/proc/sys/kernel/init-chroot", O_RDONLY)) == -1) {
		message(CONSOLE,
				"SIGHUP recived, but could not open proc file\r\n");
		sleep(2);
		return;
	}
	if (read(fd, rootpath, sizeof(rootpath)) == -1) {
		message(CONSOLE,
				"SIGHUP recived, but could not read proc file\r\n");
		sleep(2);
		return;
	}
	close(fd);

	if (rootpath[0] == '\0') {
		message(CONSOLE,
				"SIGHUP recived, but new root is not valid: %s\r\n",
				rootpath);
		sleep(2);
		return;
	}

	tc = strrchr(rootpath, '\n');
	*tc = '\0';

	/* Ok, making it this far means we commit */
	message(CONSOLE, "Please stand by, changing root to `%s'.\r\n",
			rootpath);

	/* kill all other programs first */
	message(CONSOLE, "Sending SIGTERM to all processes.\r\n");
	kill(-1, SIGTERM);
	sleep(2);
	sync();

	message(CONSOLE, "Sending SIGKILL to all processes.\r\n");
	kill(-1, SIGKILL);
	sleep(2);
	sync();

	/* ok, we don't need /proc anymore. we also assume that the signaling
	 * process left the rest of the filesystems alone for us */
	umount("/proc");

	/* Ok, now we chroot. Hopefully we only have two things mounted, the
	 * new chroot'd mount point, and the old "/" mount. s,
	 * we go ahead and unmount the old "/". This should trigger the kernel
	 * to set things up the Right Way(tm). */

	if (!chroot(rootpath))
		umount("/dev/root");

	/* If the chroot fails, we are already too far to turn back, so we
	 * continue and hope that executing init below will revive the system */

	/* close all of our descriptors and open new ones */
	close(0);
	close(1);
	close(2);
	open("/dev/console", O_RDWR, 0);
	dup(0);
	dup(0);

	message(CONSOLE, "Executing real init...\r\n");
	/* execute init in the (hopefully) new root */
	execve("/sbin/init", argv_init, envp_init);

	message(CONSOLE,
			"ERROR: Could not exec new init. Press %s to reboot.\r\n",
			(secondConsole == NULL)	/* serial console */
			? "Reset" : "CTRL-ALT-DEL");
	return;
}
#endif							/* BB_FEATURE_INIT_CHROOT */

#endif							/* ! DEBUG_INIT */

void new_initAction(initActionEnum action, char *process, char *cons)
{
	initAction *newAction;

	if (*cons == '\0')
		cons = console;

	/* If BusyBox detects that a serial console is in use, 
	 * then entries not refering to the console or null devices will _not_ be run.
	 * The exception to this rule is the null device.
	 */
	if (secondConsole == NULL && strcmp(cons, console)
		&& strcmp(cons, "/dev/null"))
		return;

	newAction = calloc((size_t) (1), sizeof(initAction));
	if (!newAction) {
		message(LOG | CONSOLE, "Memory allocation failure\n");
		while (1)
			sleep(1);
	}
	newAction->nextPtr = initActionList;
	initActionList = newAction;
	strncpy(newAction->process, process, 255);
	newAction->action = action;
	strncpy(newAction->console, cons, 255);
	newAction->pid = 0;
//    message(LOG|CONSOLE, "process='%s' action='%d' console='%s'\n",
//      newAction->process, newAction->action, newAction->console);
}

void delete_initAction(initAction * action)
{
	initAction *a, *b = NULL;

	for (a = initActionList; a; b = a, a = a->nextPtr) {
		if (a == action) {
			if (b == NULL) {
				initActionList = a->nextPtr;
			} else {
				b->nextPtr = a->nextPtr;
			}
			free(a);
			break;
		}
	}
}

/* NOTE that if BB_FEATURE_USE_INITTAB is NOT defined,
 * then parse_inittab() simply adds in some default
 * actions(i.e runs INIT_SCRIPT and then starts a pair 
 * of "askfirst" shells).  If BB_FEATURE_USE_INITTAB 
 * _is_ defined, but /etc/inittab is missing, this 
 * results in the same set of default behaviors.
 * */
void parse_inittab(void)
{
#ifdef BB_FEATURE_USE_INITTAB
	FILE *file;
	char buf[256], lineAsRead[256], tmpConsole[256];
	char *p, *q, *r, *s;
	const struct initActionType *a = actions;
	int foundIt;


	file = fopen(INITTAB, "r");
	if (file == NULL) {
		/* No inittab file -- set up some default behavior */
#endif
		/* Askfirst shell on tty1 */
		new_initAction(ASKFIRST, SHELL, console);
		/* Askfirst shell on tty2 */
		if (secondConsole != NULL)
			new_initAction(ASKFIRST, SHELL, secondConsole);
		/* sysinit */
		new_initAction(SYSINIT, INIT_SCRIPT, console);

		return;
#ifdef BB_FEATURE_USE_INITTAB
	}

	while (fgets(buf, 255, file) != NULL) {
		foundIt = FALSE;
		for (p = buf; *p == ' ' || *p == '\t'; p++);
		if (*p == '#' || *p == '\n')
			continue;

		/* Trim the trailing \n */
		q = strrchr(p, '\n');
		if (q != NULL)
			*q = '\0';

		/* Keep a copy around for posterity's sake (and error msgs) */
		strcpy(lineAsRead, buf);

		/* Grab the ID field */
		s = p;
		p = strchr(p, ':');
		if (p != NULL || *(p + 1) != '\0') {
			*p = '\0';
			++p;
		}

		/* Now peal off the process field from the end
		 * of the string */
		q = strrchr(p, ':');
		if (q == NULL || *(q + 1) == '\0') {
			message(LOG | CONSOLE, "Bad inittab entry: %s\n", lineAsRead);
			continue;
		} else {
			*q = '\0';
			++q;
		}

		/* Now peal off the action field */
		r = strrchr(p, ':');
		if (r == NULL || *(r + 1) == '\0') {
			message(LOG | CONSOLE, "Bad inittab entry: %s\n", lineAsRead);
			continue;
		} else {
			++r;
		}

		/* Ok, now process it */
		a = actions;
		while (a->name != 0) {
			if (strcmp(a->name, r) == 0) {
				if (*s != '\0') {
					struct stat statBuf;

					strcpy(tmpConsole, "/dev/");
					strncat(tmpConsole, s, 200);
					if (stat(tmpConsole, &statBuf) != 0) {
						message(LOG | CONSOLE,
								"device '%s' does not exist.  Did you read the directions?\n",
								tmpConsole);
						break;
					}
					s = tmpConsole;
				}
				new_initAction(a->action, q, s);
				foundIt = TRUE;
			}
			a++;
		}
		if (foundIt == TRUE)
			continue;
		else {
			/* Choke on an unknown action */
			message(LOG | CONSOLE, "Bad inittab entry: %s\n", lineAsRead);
		}
	}
	return;
#endif
}

extern int init_main(int argc, char **argv)
{
	initAction *a;
	pid_t wpid;
	int status;

#ifndef DEBUG_INIT
	/* Expect to be PID 1 if we are run as init (not linuxrc) */
	if (getpid() != 1 && strstr(argv[0], "init") != NULL) {
		usage("init\n\nInit is the parent of all processes.\n\n"
			  "This version of init is designed to be run only by the kernel\n");
	}
	/* Fix up argv[0] to be certain we claim to be init */
	strncpy(argv[0], "init", strlen(argv[0]));

	/* Set up sig handlers  -- be sure to
	 * clear all of these in run() */
	signal(SIGUSR1, halt_signal);
	signal(SIGUSR2, reboot_signal);
	signal(SIGINT, reboot_signal);
	signal(SIGTERM, reboot_signal);
#if defined BB_FEATURE_INIT_CHROOT
	signal(SIGHUP, check_chroot);
#endif

	/* Turn off rebooting via CTL-ALT-DEL -- we get a 
	 * SIGINT on CAD so we can shut things down gracefully... */
	reboot(RB_DISABLE_CAD);
#endif

	/* Figure out where the default console should be */
	console_init();

	/* Close whatever files are open, and reset the console. */
	close(0);
	close(1);
	close(2);
	set_term(0);
	setsid();

	/* Make sure PATH is set to something sane */
	putenv(_PATH_STDPATH);

	/* Hello world */
#ifndef DEBUG_INIT
	message(LOG | CONSOLE,
			"init started:  BusyBox v%s (%s) multi-call binary\r\n",
			BB_VER, BB_BT);
#else
	message(LOG | CONSOLE,
			"init(%d) started:  BusyBox v%s (%s) multi-call binary\r\n",
			getpid(), BB_VER, BB_BT);
#endif


	/* Mount /proc */
	if (mount("proc", "/proc", "proc", 0, 0) == 0) {
		message(LOG | CONSOLE, "Mounting /proc: done.\n");
		kernelVersion = get_kernel_revision();
	} else
		message(LOG | CONSOLE, "Mounting /proc: failed!\n");

	/* Make sure there is enough memory to do something useful. */
	check_memory();

	/* Check if we are supposed to be in single user mode */
	if (argc > 1 && (!strcmp(argv[1], "single") ||
					 !strcmp(argv[1], "-s") || !strcmp(argv[1], "1"))) {
		/* Ask first then start a shell on tty2 */
		if (secondConsole != NULL)
			new_initAction(ASKFIRST, SHELL, secondConsole);
		/* Start a shell on tty1 */
		new_initAction(RESPAWN, SHELL, console);
	} else {
		/* Not in single user mode -- see what inittab says */

		/* NOTE that if BB_FEATURE_USE_INITTAB is NOT defined,
		 * then parse_inittab() simply adds in some default
		 * actions(i.e runs INIT_SCRIPT and then starts a pair 
		 * of "askfirst" shells */
		parse_inittab();
	}

	/* Now run everything that needs to be run */

	/* First run the sysinit command */
	for (a = initActionList; a; a = a->nextPtr) {
		if (a->action == SYSINIT) {
			waitfor(a->process, a->console, FALSE);
			/* Now remove the "sysinit" entry from the list */
			delete_initAction(a);
		}
	}
	/* Next run anything that wants to block */
	for (a = initActionList; a; a = a->nextPtr) {
		if (a->action == WAIT) {
			waitfor(a->process, a->console, FALSE);
			/* Now remove the "wait" entry from the list */
			delete_initAction(a);
		}
	}
	/* Next run anything to be run only once */
	for (a = initActionList; a; a = a->nextPtr) {
		if (a->action == ONCE) {
			run(a->process, a->console, FALSE);
			/* Now remove the "once" entry from the list */
			delete_initAction(a);
		}
	}
	/* If there is nothing else to do, stop */
	if (initActionList == NULL) {
		message(LOG | CONSOLE,
				"No more tasks for init -- sleeping forever.\n");
		while (1)
			sleep(1);
	}

	/* Now run the looping stuff for the rest of forever */
	while (1) {
		for (a = initActionList; a; a = a->nextPtr) {
			/* Only run stuff with pid==0.  If they have
			 * a pid, that means they are still running */
			if (a->pid == 0) {
				switch (a->action) {
				case RESPAWN:
					/* run the respawn stuff */
					a->pid = run(a->process, a->console, FALSE);
					break;
				case ASKFIRST:
					/* run the askfirst stuff */
					a->pid = run(a->process, a->console, TRUE);
					break;
					/* silence the compiler's incessant whining */
				default:
					break;
				}
			}
		}
		/* Wait for a child process to exit */
		wpid = wait(&status);
		if (wpid > 0) {
			/* Find out who died and clean up their corpse */
			for (a = initActionList; a; a = a->nextPtr) {
				if (a->pid == wpid) {
					a->pid = 0;
					message(LOG,
							"Process '%s' (pid %d) exited.  Scheduling it for restart.\n",
							a->process, wpid);
				}
			}
		}
		sleep(1);
	}
}
