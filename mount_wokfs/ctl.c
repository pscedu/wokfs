/* $Id$ */
/*
 * %ISC_START_LICENSE%
 * ---------------------------------------------------------------------
 * Copyright 2015, Google, Inc.
 * Copyright 2015, Pittsburgh Supercomputing Center
 * All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 * --------------------------------------------------------------------
 * %END_LICENSE%
 */

/*
 * Interface for controlling live operation of a wokfs instance.
 */

#include <sys/param.h>
#include <sys/socket.h>

#include <dlfcn.h>

#include "pfl/cdefs.h"
#include "pfl/ctl.h"
#include "pfl/ctlsvr.h"
#include "pfl/fs.h"
#include "pfl/net.h"
#include "pfl/str.h"

#include "ctl.h"
#include "mount_wokfs.h"

int
wokctl_getcreds(int s, struct pscfs_creds *pcrp)
{
	uid_t uid;
	gid_t gid;
	int rc;

	rc = pfl_socket_getpeercred(s, &uid, &gid);
	pcrp->pcr_uid = uid;
	pcrp->pcr_gid = gid;
	pcrp->pcr_ngid = 1;
	return (rc);
}

int
wokctl_getclientctx(__unusedx int s, struct pscfs_clientctx *pfcc)
{
	pfcc->pfcc_pid = -1;
	return (0);
}

int
wokctlcmd_insert(int fd, struct psc_ctlmsghdr *mh, void *msg)
{
	struct wokctlmsg_modspec *wcms = msg;
	struct pscfs_creds pcr;
	struct wok_module *wm;
	int rc;

	rc = wokctl_getcreds(fd, &pcr);
	if (rc)
		return (psc_ctlsenderr(fd, mh, "insert: %s",
		    strerror(rc)));
	if (pcr.pcr_uid)
		return (psc_ctlsenderr(fd, mh, "insert: %s",
		    strerror(EPERM)));

	pflfs_modules_wrpin();

	if (wcms->wcms_pos < 0 ||
	    wcms->wcms_pos > psc_dynarray_len(&pscfs_modules)) {
		pflfs_modules_wrunpin();
		return (psc_ctlsenderr(fd, mh, "insert %d: invalid "
		    "position", wcms->wcms_pos));
	}

	rc = mod_load(wcms->wcms_pos, wcms->wcms_path);
	pflfs_modules_wrunpin();
	if (rc) {
		const char *error;

		switch (rc) {
		case -1:
			error = dlerror();
			break;
		case ENXIO:
			error = "module does not define symbol "
			    "'pscfs_module_load' ";
			break;
		default:
			error = strerror(rc);
			break;
		}
		return (psc_ctlsenderr(fd, mh, "insert %s: %s",
		    wcms->wcms_path, error));
	}
	return (1);
}

int
wokctlcmd_list(int fd, struct psc_ctlmsghdr *mh, void *msg)
{
	struct wokctlmsg_modspec *wcms = msg;
	struct wok_module *wm;
	struct pscfs *m;
	int i, rc = 1;

	pflfs_modules_rdpin();
	DYNARRAY_FOREACH(m, i, &pscfs_modules) {
		/*
		 * Skip default module which is internal to pflfs
		 * itself and is only used for handing unmount.
		 */
		if (i == psc_dynarray_len(&pscfs_modules) - 1)
			break;

		wm = m->pf_private;
		strlcpy(wcms->wcms_path, wm->wm_path,
		    sizeof(wcms->wcms_path));
		wcms->wcms_pos = i;
		rc = psc_ctlmsg_sendv(fd, mh, wcms);
		if (!rc)
			break;
	}
	pflfs_modules_rdunpin();
	return (rc);
}

/*
 * Replace a module in the file system processing stack.  Note that the
 * order of operations here is tricky due to instantiation issues with
 * the old and new modules in the case of reloading the same module.
 */
int
wokctlcmd_reload(__unusedx int fd, __unusedx struct psc_ctlmsghdr *mh,
    void *msg)
{
	struct wokctlmsg_modctl *wcmc = msg;
	struct pscfs_creds pcr;
	struct wok_module *wm;
	struct pscfs *m;
	char *path;
	int rc;

	rc = wokctl_getcreds(fd, &pcr);
	if (rc)
		return (psc_ctlsenderr(fd, mh, "reload: %s",
		    strerror(rc)));
	if (pcr.pcr_uid)
		return (psc_ctlsenderr(fd, mh, "reload: %s",
		    strerror(EPERM)));

	pflfs_modules_wrpin();

	if (wcmc->wcmc_pos < 0 ||
	    wcmc->wcmc_pos >= psc_dynarray_len(&pscfs_modules) - 1) {
		pflfs_modules_wrunpin();
		return (psc_ctlsenderr(fd, mh, "reload %d: invalid "
		    "position", wcmc->wcmc_pos));
	}

	m = psc_dynarray_getpos(&pscfs_modules, wcmc->wcmc_pos);
	wm = m->pf_private;
	path = pfl_strdup(wm->wm_path);
	pflfs_module_destroy(m);

	mod_destroy(wm);

	rc = mod_load(wcmc->wcmc_pos, path);
	PSCFREE(path);

	pflfs_modules_wrunpin();

	if (rc)
		return (psc_ctlsenderr(fd, mh, "reload %d: %s",
		    wcmc->wcmc_pos, dlerror()));

	return (1);
}

int
wokctlcmd_remove(__unusedx int fd, __unusedx struct psc_ctlmsghdr *mh,
    void *msg)
{
	struct wokctlmsg_modctl *wcmc = msg;
	struct pscfs_creds pcr;
	struct wok_module *wm;
	struct pscfs *m;
	int rc;

	rc = wokctl_getcreds(fd, &pcr);
	if (rc)
		return (psc_ctlsenderr(fd, mh, "remove: %s",
		    strerror(rc)));
	if (pcr.pcr_uid)
		return (psc_ctlsenderr(fd, mh, "remove: %s",
		    strerror(EPERM)));

	pflfs_modules_wrpin();

	if (wcmc->wcmc_pos < 0 ||
	    wcmc->wcmc_pos >= psc_dynarray_len(&pscfs_modules) - 1) {
		pflfs_modules_wrunpin();
		return (psc_ctlsenderr(fd, mh, "remove %d: invalid "
		    "position", wcmc->wcmc_pos));
	}

	m = pflfs_module_remove(wcmc->wcmc_pos);
	wm = m->pf_private;
	pflfs_module_destroy(m);
	pflfs_modules_wrunpin();

	mod_destroy(wm);

	return (1);
}

struct psc_ctlop ctlops[] = {
	PSC_CTLDEFOPS,
	{ wokctlcmd_insert,		sizeof(struct wokctlmsg_modspec) },
	{ wokctlcmd_list,		sizeof(struct wokctlmsg_modspec) },
	{ wokctlcmd_reload,		sizeof(struct wokctlmsg_modctl) },
	{ wokctlcmd_remove,		sizeof(struct wokctlmsg_modctl) },
};

psc_ctl_thrget_t psc_ctl_thrgets[] = {
};

PFLCTL_SVR_DEFS;

void
ctlacthr_main(__unusedx struct psc_thread *thr)
{
	psc_ctlthr_main(ctlsockfn, ctlops, nitems(ctlops),
	    THRT_CTLAC);
}

void
ctlthr_spawn(void)
{
	struct psc_thread *thr;

	psc_ctlparam_register("faults", psc_ctlparam_faults);
	psc_ctlparam_register("log.file", psc_ctlparam_log_file);
	psc_ctlparam_register("log.format", psc_ctlparam_log_format);
	psc_ctlparam_register("log.level", psc_ctlparam_log_level);
	psc_ctlparam_register("log.points", psc_ctlparam_log_points);
	psc_ctlparam_register("opstats", psc_ctlparam_opstats);
	psc_ctlparam_register("pause", psc_ctlparam_pause);
	psc_ctlparam_register("pool", psc_ctlparam_pool);
	psc_ctlparam_register("rlim", psc_ctlparam_rlim);
	psc_ctlparam_register("run", psc_ctlparam_run);
	psc_ctlparam_register("rusage", psc_ctlparam_rusage);

	psc_ctlparam_register_var("sys.mountpoint", PFLCTL_PARAMT_STR,
	    0, mountpoint);
//	psc_ctlparam_register_simple("sys.uptime", ctlparam_uptime_get,
//	    NULL);
//	psc_ctlparam_register_simple("sys.version",
//	    ctlparam_version_get, NULL);

	thr = pscthr_init(THRT_CTL, ctlacthr_main, NULL,
	    sizeof(struct psc_ctlthr), "ctlacthr");
	pscthr_setready(thr);
}
