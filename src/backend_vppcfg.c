/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Najieb Haswell
 *
 * backend_vppcfg.c — Clixon backend plugin that applies the vppcfg.yang model to VPP through vppcfg.
 *
 * Each transaction phase writes the configuration tree to a temporary XML file and calls the helper:
 *   validate: clixon-vppcfg validate  (vppcfg check + plan against the running VPP; changes nothing)
 *   commit:   clixon-vppcfg apply     (write /etc/vpp/vppcfg.yaml, vppcfg apply, regenerate the boot file)
 *   revert:   clixon-vppcfg apply     with the previous configuration
 * The helper's error output is returned to the client (CLI/NETCONF) as an rpc-error.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cligen/cligen.h>
#include <clixon/clixon.h>
#include <clixon/clixon_backend.h>

#define HELPER  "/usr/local/libexec/clixon-vppcfg/clixon-vppcfg"
#define TMPDIR  "/run/clixon-vppcfg"
#define ERRMAX  8192

/*! Run the helper on XML tree xt; the helper's stdout+stderr is collected in errbuf.
 * @retval  0  helper succeeded
 * @retval -1  helper failed or system error (errbuf holds the explanation)
 */
static int
run_helper(const char *verb,
           cxobj      *xt,
           char       *errbuf,
           size_t      errlen)
{
    char    path[] = TMPDIR "/cfg-XXXXXX";
    int     fd;
    int     pfd[2];
    int     status = 0;
    ssize_t n;
    size_t  got = 0;
    char    discard[512];
    FILE   *f;
    pid_t   pid;

    errbuf[0] = '\0';
    if ((fd = mkstemp(path)) < 0) {
        snprintf(errbuf, errlen, "mkstemp %s: %s", path, strerror(errno));
        return -1;
    }
    if ((f = fdopen(fd, "w")) == NULL) {
        snprintf(errbuf, errlen, "fdopen: %s", strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    if (xt != NULL && clixon_xml2file(f, xt, 0, 0, NULL, fprintf, 0, 0) < 0) {
        snprintf(errbuf, errlen, "failed to write the configuration XML");
        fclose(f);
        unlink(path);
        return -1;
    }
    fclose(f);
    if (pipe(pfd) < 0) {
        snprintf(errbuf, errlen, "pipe: %s", strerror(errno));
        unlink(path);
        return -1;
    }
    if ((pid = fork()) < 0) {
        snprintf(errbuf, errlen, "fork: %s", strerror(errno));
        close(pfd[0]);
        close(pfd[1]);
        unlink(path);
        return -1;
    }
    if (pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        execl(HELPER, HELPER, verb, path, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    while ((n = read(pfd[0], got < errlen - 1 ? errbuf + got : discard,
                     got < errlen - 1 ? errlen - 1 - got : sizeof(discard))) > 0)
        if (got < errlen - 1)
            got += n;
    errbuf[got] = '\0';
    close(pfd[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    unlink(path);
    while (got > 0 && (errbuf[got-1] == '\n' || errbuf[got-1] == ' '))
        errbuf[--got] = '\0';
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    if (got == 0)
        snprintf(errbuf, errlen, "%s exited with status %d", HELPER,
                 WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return -1;
}

int
vppcfg_validate(clixon_handle    h,
                transaction_data td)
{
    char errbuf[ERRMAX];

    if (run_helper("validate", transaction_target(td), errbuf, sizeof(errbuf)) < 0) {
        clixon_err(OE_PLUGIN, 0, "%s", errbuf);
        return -1;
    }
    return 0;
}

int
vppcfg_commit(clixon_handle    h,
              transaction_data td)
{
    char errbuf[ERRMAX];
    char errbuf2[ERRMAX];

    if (run_helper("apply", transaction_target(td), errbuf, sizeof(errbuf)) < 0) {
        /* Restore the dataplane to the previous configuration before reporting the failure. */
        if (run_helper("apply", transaction_src(td), errbuf2, sizeof(errbuf2)) < 0)
            clixon_log(h, LOG_ERR, "vppcfg: restoring the previous configuration also failed: %s", errbuf2);
        clixon_err(OE_PLUGIN, 0, "%s", errbuf);
        return -1;
    }
    clixon_log(h, LOG_NOTICE, "vppcfg: configuration applied to VPP");
    return 0;
}

int
vppcfg_revert(clixon_handle    h,
              transaction_data td)
{
    char errbuf[ERRMAX];

    if (run_helper("apply", transaction_src(td), errbuf, sizeof(errbuf)) < 0) {
        clixon_log(h, LOG_ERR, "vppcfg: revert failed: %s", errbuf);
        return -1;
    }
    return 0;
}

clixon_plugin_api *clixon_plugin_init(clixon_handle h);

static clixon_plugin_api api = {
    .ca_name           = "backend_vppcfg",
    .ca_init           = clixon_plugin_init,
    .ca_trans_validate = vppcfg_validate,
    .ca_trans_commit   = vppcfg_commit,
    .ca_trans_revert   = vppcfg_revert,
};

clixon_plugin_api *
clixon_plugin_init(clixon_handle h)
{
    return &api;
}
