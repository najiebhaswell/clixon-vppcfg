/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Najieb Haswell
 *
 * cli_vppcfg.c — Cisco-style Clixon CLI plugin for the vppcfg.yang model.
 *
 * Configuration commands only modify the candidate datastore (edit-config); VPP changes
 * on "commit" (backend plugin -> vppcfg). Operational "show" commands run vppctl/birdc
 * directly, without a shell.
 *
 * The interface name selects the table that is edited:
 *   loopN          -> /loopbacks/loopback
 *   BondEthernetN  -> /bondethernets/bondethernet (members, mode) + /interfaces/interface (L3)
 *   parent.N       -> /interfaces/interface[parent]/sub-interface[N]
 *   anything else  -> /interfaces/interface (PHY)
 */
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cligen/cligen.h>
#include <clixon/clixon.h>
#include <clixon/clixon_cli.h>

#define NS      "urn:clixon-vppcfg:params:xml:ns:yang:vppcfg"
#define NC_NS   "urn:ietf:params:xml:ns:netconf:base:1.0"
#define HELPER  "/usr/local/libexec/clixon-vppcfg/clixon-vppcfg"
#define VPPCTL  "/usr/bin/vppctl"

enum iftype { IF_PHY, IF_SUB, IF_LOOP, IF_BOND };

static char _ifname[64]; /* current interface in the interface configuration modes */

static int
all_digits(const char *s)
{
    if (*s == '\0')
        return 0;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s))
            return 0;
    return 1;
}

static int
if_classify(const char *name,
            char       *parent,
            size_t      plen,
            char       *subid,
            size_t      slen)
{
    const char *dot;

    if (strncmp(name, "loop", 4) == 0 && all_digits(name + 4))
        return IF_LOOP;
    if (strncmp(name, "BondEthernet", 12) == 0 && all_digits(name + 12))
        return IF_BOND;
    if ((dot = strrchr(name, '.')) != NULL && dot != name && all_digits(dot + 1)) {
        snprintf(parent, plen, "%.*s", (int)(dot - name), name);
        snprintf(subid, slen, "%s", dot + 1);
        return IF_SUB;
    }
    return IF_PHY;
}

static int
name_ok(const char *s)
{
    size_t len = s ? strlen(s) : 0;

    if (len == 0 || len > 63)
        return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && strchr("._/-", *s) == NULL)
            return 0;
    return 1;
}

static void
xesc(cbuf       *cb,
     const char *s)
{
    for (; *s; s++)
        switch (*s) {
        case '&':  cprintf(cb, "&amp;");  break;
        case '<':  cprintf(cb, "&lt;");   break;
        case '>':  cprintf(cb, "&gt;");   break;
        case '"':  cprintf(cb, "&quot;"); break;
        case '\'': cprintf(cb, "&apos;"); break;
        default:   cprintf(cb, "%c", *s);
        }
}

/*! Cisco-style operator input error ("% ..."). Not a system error, so callbacks return 0. */
static int
user_error(const char *fmt, ...)
{
    va_list ap;
    char    msg[1024];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    cligen_output(stdout, "%% %s\n", msg);
    return 0;
}

/*! Send edit-config (merge) to the candidate for the entry of interface ifname.
 * @param[in] bondtbl  1: /bondethernets table, 0: L3 table (interfaces/loopbacks)
 * @param[in] inner    child elements inside the entry (may be "")
 * @param[in] entryop  NETCONF operation on the entry itself (e.g. "remove"), or NULL
 */
static int
edit_if(clixon_handle h,
        const char   *ifname,
        int           bondtbl,
        const char   *inner,
        const char   *entryop)
{
    char  parent[64];
    char  subid[16];
    char  op[48] = "";
    cbuf *cb;
    int   t;
    int   ret;

    t = if_classify(ifname, parent, sizeof(parent), subid, sizeof(subid));
    if (entryop)
        snprintf(op, sizeof(op), " nc:operation=\"%s\"", entryop);
    if ((cb = cbuf_new()) == NULL)
        return -1;
    cprintf(cb, "<config xmlns:nc=\"%s\">", NC_NS);
    if (bondtbl) {
        cprintf(cb, "<bondethernets xmlns=\"%s\"><bondethernet%s><name>", NS, op);
        xesc(cb, ifname);
        cprintf(cb, "</name>%s</bondethernet></bondethernets>", inner);
    }
    else if (t == IF_LOOP) {
        cprintf(cb, "<loopbacks xmlns=\"%s\"><loopback%s><name>", NS, op);
        xesc(cb, ifname);
        cprintf(cb, "</name>%s</loopback></loopbacks>", inner);
    }
    else if (t == IF_SUB) {
        cprintf(cb, "<interfaces xmlns=\"%s\"><interface><name>", NS);
        xesc(cb, parent);
        cprintf(cb, "</name><sub-interface%s><id>%s</id>%s</sub-interface></interface></interfaces>",
                op, subid, inner);
    }
    else {
        cprintf(cb, "<interfaces xmlns=\"%s\"><interface%s><name>", NS, op);
        xesc(cb, ifname);
        cprintf(cb, "</name>%s</interface></interfaces>", inner);
    }
    cprintf(cb, "</config>");
    ret = clicon_rpc_edit_config(h, "candidate", OP_MERGE, cbuf_get(cb));
    cbuf_free(cb);
    return ret < 0 ? -1 : 0;
}

/*! Check whether a leaf applies to the current interface type. @retval NULL allowed, otherwise the reason. */
static const char *
leaf_check(const char *leaf,
           int         t,
           int         bondtbl)
{
    if (bondtbl)
        return t == IF_BOND ? NULL : "only applies to BondEthernet interfaces";
    if (strcmp(leaf, "encapsulation") == 0)
        return t == IF_SUB ? NULL : "only applies to sub-interfaces (e.g. cx5-0.100)";
    if (strcmp(leaf, "sflow") == 0 || strcmp(leaf, "device-type") == 0)
        return t == IF_PHY ? NULL : "only applies to physical interfaces";
    if (strcmp(leaf, "mac") == 0)
        return t == IF_SUB ? "does not apply to sub-interfaces" : NULL;
    if (strcmp(leaf, "l2xc") == 0 || strcmp(leaf, "state") == 0)
        return t == IF_LOOP ? "does not apply to loopbacks" : NULL;
    return NULL;
}

/*! Return clispec argument i as a string, or NULL if absent. */
static const char *
arg_str(cvec *argv,
        int   i)
{
    return cvec_len(argv) > i ? cv_string_get(cvec_i(argv, i)) : NULL;
}

/*! Resolve the leaf context (table, type check). Returns the interface type, -1 if rejected (message printed). */
static int
leaf_context(const char *leaf,
             cvec       *argv,
             int         argi_bond,
             int        *bondtbl)
{
    char        parent[64];
    char        subid[16];
    const char *why;
    const char *b = arg_str(argv, argi_bond);
    int         t;

    if (_ifname[0] == '\0') {
        user_error("No interface selected");
        return -1;
    }
    t = if_classify(_ifname, parent, sizeof(parent), subid, sizeof(subid));
    *bondtbl = (b != NULL && strcmp(b, "bond") == 0);
    if (strcmp(leaf, "mac") == 0 && t == IF_BOND)
        *bondtbl = 1; /* the bond MAC lives in the bondethernets table */
    if ((why = leaf_check(leaf, t, *bondtbl)) != NULL) {
        user_error("'%s' %s", leaf, why);
        return -1;
    }
    return t;
}

/*! CLI variable value as a new (malloc'ed) string, or the literal if src starts with '='. */
static char *
var_value(cvec       *cvv,
          const char *src)
{
    cg_var *cv;

    if (src == NULL)
        return NULL;
    if (src[0] == '=')
        return strdup(src + 1);
    if ((cv = cvec_find(cvv, src)) == NULL)
        return NULL;
    return cv2str_dup(cv);
}

/*! VPP interface names from "vppctl show interface" (excluding local0 and linux-cp TAPs). */
static int
vpp_ifnames(cvec *out)
{
    FILE *p;
    char  line[512];
    char  ifn[128];

    if ((p = popen(VPPCTL " show interface 2>/dev/null", "r")) == NULL)
        return -1;
    while (fgets(line, sizeof(line), p) != NULL) {
        if (!isalnum((unsigned char)line[0]) || sscanf(line, "%127s", ifn) != 1)
            continue;
        if (strcmp(ifn, "Name") == 0 || strcmp(ifn, "local0") == 0 || strncmp(ifn, "tap", 3) == 0)
            continue;
        cvec_add_string(out, NULL, ifn);
    }
    return pclose(p) == 0 ? 0 : -1;
}

/*! Reject a physical interface (or the physical parent of a sub-interface) that does not exist in VPP.
 * @retval 0 exists or may be created, 1 missing (message printed), -1 system error
 * loopN and BondEthernetN may be new because vppcfg creates them on commit.
 * Commit-time validation still applies to NETCONF/RESTCONF clients. */
static int
check_phy_exists(const char *name)
{
    char    parent[64];
    char    subid[16];
    const char *phy = name;
    cvec   *names;
    cg_var *cv = NULL;
    cbuf   *list;
    int     t = if_classify(name, parent, sizeof(parent), subid, sizeof(subid));
    int     found = 0;

    if (t == IF_SUB) {
        phy = parent;
        t = if_classify(parent, parent, sizeof(parent), subid, sizeof(subid));
    }
    if (t != IF_PHY)
        return 0;
    if ((names = cvec_new(0)) == NULL)
        return -1;
    if (vpp_ifnames(names) < 0) {
        cvec_free(names);
        clixon_err(OE_PLUGIN, 0, "Cannot read the interface list from VPP (vppctl)");
        return -1;
    }
    list = cbuf_new();
    while ((cv = cvec_each(names, cv)) != NULL) {
        if (strcmp(cv_string_get(cv), phy) == 0)
            found = 1;
        if (if_classify(cv_string_get(cv), parent, sizeof(parent), subid, sizeof(subid)) == IF_PHY)
            cprintf(list, " %s", cv_string_get(cv));
    }
    if (!found)
        user_error("Physical interface %s does not exist in VPP. Available physical interfaces:%s", phy, cbuf_get(list));
    cbuf_free(list);
    cvec_free(names);
    return found ? 0 : 1;
}

/* interface <ifname>: select/create an interface and enter the mode for its type, so that '?'
 * only lists commands that apply: configure-if (physical), configure-subif, configure-loop,
 * configure-bond. */
int
vc_if_enter(clixon_handle h,
            cvec         *cvv,
            cvec         *argv)
{
    char  parent[64];
    char  subid[16];
    char       *name = cvec_find_str(cvv, "ifname");
    const char *mode;
    int         ret;

    if (!name_ok(name))
        return user_error("Invalid interface name: %s", name ? name : "");
    if ((ret = check_phy_exists(name)) != 0)
        return ret < 0 ? -1 : 0;
    if (if_classify(name, parent, sizeof(parent), subid, sizeof(subid)) == IF_BOND &&
        edit_if(h, name, 1, "", NULL) < 0)
        return -1;
    if (edit_if(h, name, 0, "", NULL) < 0)
        return -1;
    snprintf(_ifname, sizeof(_ifname), "%s", name);
    switch (if_classify(name, parent, sizeof(parent), subid, sizeof(subid))) {
    case IF_SUB:  mode = "configure-subif"; break;
    case IF_LOOP: mode = "configure-loop"; break;
    case IF_BOND: mode = "configure-bond"; break;
    default:      mode = "configure-if"; break;
    }
    return cli_set_syntax_mode(h, (char *)mode) < 0 ? -1 : 0;
}

/* no interface <ifname>: remove all configuration of the interface from the candidate */
int
vc_no_interface(clixon_handle h,
                cvec         *cvv,
                cvec         *argv)
{
    char  parent[64];
    char  subid[16];
    char *name = cvec_find_str(cvv, "ifname");

    if (!name_ok(name))
        return user_error("Invalid interface name: %s", name ? name : "");
    if (if_classify(name, parent, sizeof(parent), subid, sizeof(subid)) == IF_BOND &&
        edit_if(h, name, 1, "", "remove") < 0)
        return -1;
    return edit_if(h, name, 0, "", "remove");
}

/* <leaf> <value>: argv = (leaf, variable|"=literal" [, "bond"]) */
int
vc_set_leaf(clixon_handle h,
            cvec         *cvv,
            cvec         *argv)
{
    const char *leaf = arg_str(argv, 0);
    char       *val;
    cbuf       *cb;
    int         bondtbl;
    int         ret;

    if (leaf_context(leaf, argv, 2, &bondtbl) < 0)
        return 0;
    if ((val = var_value(cvv, arg_str(argv, 1))) == NULL)
        return user_error("Missing value for %s", leaf);
    cb = cbuf_new();
    cprintf(cb, "<%s>", leaf);
    xesc(cb, val);
    cprintf(cb, "</%s>", leaf);
    ret = edit_if(h, _ifname, bondtbl, cbuf_get(cb), NULL);
    cbuf_free(cb);
    free(val);
    return ret;
}

/* no <leaf>: argv = (leaf [, "bond"]) */
int
vc_del_leaf(clixon_handle h,
            cvec         *cvv,
            cvec         *argv)
{
    const char *leaf = arg_str(argv, 0);
    char        inner[96];
    int         bondtbl;

    if (leaf_context(leaf, argv, 1, &bondtbl) < 0)
        return 0;
    snprintf(inner, sizeof(inner), "<%s nc:operation=\"remove\"/>", leaf);
    return edit_if(h, _ifname, bondtbl, inner, NULL);
}

/* Add/remove one leaf-list value: argv = (leaf, variable [, "bond"]) */
static int
leaflist_op(clixon_handle h,
            cvec         *cvv,
            cvec         *argv,
            int           remove)
{
    const char *leaf = arg_str(argv, 0);
    char       *val;
    cbuf       *cb;
    int         bondtbl;
    int         ret;

    if (leaf_context(leaf, argv, 2, &bondtbl) < 0)
        return 0;
    if ((val = var_value(cvv, arg_str(argv, 1))) == NULL)
        return user_error("Missing value for %s", leaf);
    cb = cbuf_new();
    cprintf(cb, "<%s%s>", leaf, remove ? " nc:operation=\"remove\"" : "");
    xesc(cb, val);
    cprintf(cb, "</%s>", leaf);
    ret = edit_if(h, _ifname, bondtbl, cbuf_get(cb), NULL);
    cbuf_free(cb);
    free(val);
    return ret;
}

int
vc_add_ll(clixon_handle h,
          cvec         *cvv,
          cvec         *argv)
{
    return leaflist_op(h, cvv, argv, 0);
}

int
vc_del_ll(clixon_handle h,
          cvec         *cvv,
          cvec         *argv)
{
    return leaflist_op(h, cvv, argv, 1);
}

/* encapsulation dot1q|dot1ad <outer> [inner-dot1q <inner>] [exact-match]: argv = ("dot1q"|"dot1ad") */
int
vc_encap(clixon_handle h,
         cvec         *cvv,
         cvec         *argv)
{
    const char *kind = arg_str(argv, 0);
    cg_var     *outer = cvec_find(cvv, "outer");
    cg_var     *inner = cvec_find(cvv, "inner");
    char        xml[256];
    int         bondtbl;

    if (leaf_context("encapsulation", argv, 1, &bondtbl) < 0)
        return 0;
    snprintf(xml, sizeof(xml),
             "<encapsulation nc:operation=\"replace\"><%s>%u</%s>", kind, cv_uint16_get(outer), kind);
    if (inner)
        snprintf(xml + strlen(xml), sizeof(xml) - strlen(xml),
                 "<inner-dot1q>%u</inner-dot1q>", cv_uint16_get(inner));
    if (cvec_find(cvv, "exact"))
        snprintf(xml + strlen(xml), sizeof(xml) - strlen(xml), "<exact-match>true</exact-match>");
    snprintf(xml + strlen(xml), sizeof(xml) - strlen(xml), "</encapsulation>");
    return edit_if(h, _ifname, 0, xml, NULL);
}

/*! Write datastore db to a temporary file; path receives the file name. */
static int
dump_db(clixon_handle h,
        const char   *db,
        char         *path)
{
    cxobj *xt = NULL;
    cxobj *xerr;
    FILE  *f;
    int    fd;
    int    retval = -1;

    if (clicon_rpc_get_config(h, NULL, db, "/", NULL, NULL, &xt) < 0)
        goto done;
    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL) {
        clixon_err_netconf(h, OE_NETCONF, 0, xerr, "get-config %s", db);
        goto done;
    }
    if ((fd = mkstemp(path)) < 0) {
        clixon_err(OE_UNIX, errno, "mkstemp");
        goto done;
    }
    f = fdopen(fd, "w");
    clixon_xml2file(f, xt, 0, 0, NULL, fprintf, 0, 0);
    fclose(f);
    retval = 0;
 done:
    if (xt)
        xml_free(xt);
    return retval;
}

/*! Run a program (no shell); output goes straight to the terminal. @retval exit status */
static int
run_prog(char *const args[],
         int         quiet)
{
    pid_t pid;
    int   status = 0;

    fflush(stdout);
    if ((pid = fork()) < 0) {
        clixon_err(OE_UNIX, errno, "fork");
        return -1;
    }
    if (pid == 0) {
        if (quiet) {
            freopen("/dev/null", "w", stdout);
            freopen("/dev/null", "w", stderr);
        }
        execv(args[0], args);
        fprintf(stderr, "%% Cannot run %s: %s\n", args[0], strerror(errno));
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/*! @retval 1 candidate differs from running, 0 identical, -1 error */
static int
has_changes(clixon_handle h)
{
    char  a[] = "/tmp/rtr-running-XXXXXX";
    char  b[] = "/tmp/rtr-candidate-XXXXXX";
    char *args[] = {HELPER, "changed", a, b, NULL};
    int   st;

    if (dump_db(h, "running", a) < 0)
        return -1;
    if (dump_db(h, "candidate", b) < 0) {
        unlink(a);
        return -1;
    }
    st = run_prog(args, 1);
    unlink(a);
    unlink(b);
    return st == 1 ? 1 : (st == 0 ? 0 : -1);
}

/* configure [terminal]: enter configuration mode. The candidate is shared by all sessions, so
 * warn if it already holds changes (from another session, or left over from a rolled-back
 * confirmed commit: Clixon's rollback restores running but does not clean the candidate). */
int
vc_configure(clixon_handle h,
             cvec         *cvv,
             cvec         *argv)
{
    if (has_changes(h) == 1)
        cligen_output(stdout, "%% Warning: the candidate already contains uncommitted changes\n"
                      "%% (from another session, or left over from a rolled-back confirmed commit).\n"
                      "%% Review with 'show configuration diff'; discard with 'abort' if unwanted.\n");
    return cli_set_syntax_mode(h, "configure") < 0 ? -1 : 0;
}

/* show running-config | candidate-config: argv = (db) */
int
vc_show_config(clixon_handle h,
               cvec         *cvv,
               cvec         *argv)
{
    char  path[] = "/tmp/rtr-config-XXXXXX";
    char *args[] = {HELPER, "show-config", path, NULL};

    if (dump_db(h, arg_str(argv, 0), path) < 0)
        return -1;
    run_prog(args, 0);
    unlink(path);
    return 0;
}

/* show configuration diff */
int
vc_show_diff(clixon_handle h,
             cvec         *cvv,
             cvec         *argv)
{
    char  a[] = "/tmp/rtr-running-XXXXXX";
    char  b[] = "/tmp/rtr-candidate-XXXXXX";
    char *args[] = {HELPER, "diff", a, b, NULL};

    if (dump_db(h, "running", a) < 0)
        return -1;
    if (dump_db(h, "candidate", b) < 0) {
        unlink(a);
        return -1;
    }
    run_prog(args, 0);
    unlink(a);
    unlink(b);
    return 0;
}

/* show ip interface brief */
int
vc_ip_brief(clixon_handle h,
            cvec         *cvv,
            cvec         *argv)
{
    char *args[] = {HELPER, "ip-brief", NULL};

    run_prog(args, 0);
    return 0;
}

/* Run an operational command: argv = program, arguments...; "$var" is replaced by the CLI
 * variable value (skipped when an optional variable was not given). */
int
vc_run(clixon_handle h,
       cvec         *cvv,
       cvec         *argv)
{
    char *args[32];
    int   n = 0;
    int   i;

    for (i = 0; i < cvec_len(argv) && n < 31; i++) {
        const char *a = cv_string_get(cvec_i(argv, i));
        if (a[0] == '$') {
            cg_var *cv = cvec_find(cvv, a + 1);
            if (cv == NULL)
                continue;
            if ((args[n] = cv2str_dup(cv)) == NULL)
                goto fail;
        }
        else if ((args[n] = strdup(a)) == NULL)
            goto fail;
        n++;
    }
    args[n] = NULL;
    run_prog(args, 0);
 fail:
    while (n > 0)
        free(args[--n]);
    return 0;
}

/* Confirmed-commit persist token. With persist, rollback only happens when the timeout expires
 * (not when the CLI session closes) and the confirmation may come from another session. Plain
 * commits always carry this persist-id; the backend ignores it when no confirmed commit is pending. */
#define CONFIRM_PERSIST "rtr"

/*! Plain <commit> with <persist-id>. clicon_rpc_commit() in Clixon 7.9 does not send persist-id
 * for a commit without <confirmed/>, so a persistent confirmed commit could never be confirmed.
 * @retval 1 OK, 0 rpc-error (already printed), -1 system error */
static int
rpc_commit_persist_id(clixon_handle h)
{
    cxobj *xret = NULL;
    cxobj *xerr;
    char   xml[256];
    int    retval = -1;

    snprintf(xml, sizeof(xml), "<rpc xmlns=\"%s\" %s><commit><persist-id>%s</persist-id></commit></rpc>",
             NETCONF_BASE_NAMESPACE, NETCONF_MESSAGE_ID_ATTR, CONFIRM_PERSIST);
    if (clicon_rpc_netconf(h, xml, &xret, NULL) < 0)
        goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL) {
        clixon_err_netconf(h, OE_NETCONF, 0, xerr, "Commit failed");
        retval = 0;
        goto done;
    }
    retval = 1;
 done:
    if (xret)
        xml_free(xret);
    return retval;
}

/* commit [confirmed <minutes>] */
int
vc_commit(clixon_handle h,
          cvec         *cvv,
          cvec         *argv)
{
    cg_var  *mv = cvec_find(cvv, "minutes");
    int      confirmed = cvec_find(cvv, "confirmed") != NULL;
    uint32_t minutes = mv ? cv_uint32_get(mv) : 0;
    int      ret;

    if (confirmed) {
        if (clicon_rpc_commit(h, 1, 0, minutes * 60, CONFIRM_PERSIST, CONFIRM_PERSIST) < 1)
            return 0; /* reason already printed by vc_errmsg */
        cligen_output(stdout, "%% Commit is provisional. Run 'commit' within %u minute(s) (from any "
                      "session) to confirm; otherwise the configuration rolls back automatically.\n", minutes);
        return 0;
    }
    if ((ret = rpc_commit_persist_id(h)) < 1)
        return ret < 0 ? -1 : 0;
    /* A commit that confirms a confirmed commit does not apply candidate changes made after it;
     * apply any remaining changes with a second commit. */
    if (has_changes(h) == 1 && (ret = rpc_commit_persist_id(h)) < 1)
        return ret < 0 ? -1 : 0;
    cligen_output(stdout, "%% Commit complete.\n");
    return 0;
}

/* commit cancel: cancel the pending confirmed commit (roll back now) */
int
vc_commit_cancel(clixon_handle h,
                 cvec         *cvv,
                 cvec         *argv)
{
    if (clicon_rpc_commit(h, 0, 1, 0, NULL, CONFIRM_PERSIST) < 1)
        return 0;
    cligen_output(stdout, "%% Confirmed commit cancelled; configuration rolled back.\n");
    return 0;
}

/* validate: check the candidate (including vppcfg check + plan) without applying it */
int
vc_validate(clixon_handle h,
            cvec         *cvv,
            cvec         *argv)
{
    if (clicon_rpc_validate(h, "candidate") < 1)
        return 0; /* reason already printed by vc_errmsg */
    cligen_output(stdout, "%% Validation OK.\n");
    return 0;
}

/* abort: discard candidate changes and return to exec mode */
int
vc_abort(clixon_handle h,
         cvec         *cvv,
         cvec         *argv)
{
    if (clicon_rpc_discard_changes(h) < 0)
        return -1;
    _ifname[0] = '\0';
    cligen_output(stdout, "%% Uncommitted changes discarded.\n");
    return cli_set_syntax_mode(h, "exec") < 0 ? -1 : 0;
}

/* end: return to exec mode; refused while there are uncommitted changes */
int
vc_end(clixon_handle h,
       cvec         *cvv,
       cvec         *argv)
{
    int ch = has_changes(h);

    if (ch < 0)
        return -1;
    if (ch == 1) {
        cligen_output(stdout, "%% There are uncommitted changes (see 'show configuration diff').\n"
                      "%% Use 'commit' to apply them or 'abort' to discard them.\n");
        return 0;
    }
    _ifname[0] = '\0';
    return cli_set_syntax_mode(h, "exec") < 0 ? -1 : 0;
}

/* exit from an interface mode to global configuration mode */
int
vc_if_exit(clixon_handle h,
           cvec         *cvv,
           cvec         *argv)
{
    _ifname[0] = '\0';
    return cli_set_syntax_mode(h, "configure") < 0 ? -1 : 0;
}

/*! Interfaces defined in the candidate datastore (physical, sub-interfaces, loopbacks, bonds).
 * Each entry has the interface name as cv name and its description (or "") as value. */
static int
candidate_ifnames(clixon_handle h,
                  cvec         *out)
{
    const char *paths[] = {"/vc:interfaces/vc:interface", "/vc:loopbacks/vc:loopback",
                           "/vc:bondethernets/vc:bondethernet"};
    cxobj      *xt = NULL;
    cxobj     **vec = NULL;
    cxobj      *xs;
    cvec       *nsc = NULL;
    size_t      len;
    size_t      i;
    size_t      p;
    char        buf[160];
    char       *name;
    char       *desc;
    char       *id;

    if (clicon_rpc_get_config(h, NULL, "candidate", "/", NULL, NULL, &xt) < 0 || xt == NULL)
        return -1;
    nsc = xml_nsctx_init("vc", NS);
    for (p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
        if (xpath_vec(xt, nsc, "%s", &vec, &len, paths[p]) < 0)
            break;
        for (i = 0; i < len; i++) {
            if ((name = xml_find_body(vec[i], "name")) == NULL)
                continue;
            desc = xml_find_body(vec[i], "description");
            cvec_add_string(out, name, desc ? desc : "");
            xs = NULL;
            while ((xs = xml_child_each(vec[i], xs, CX_ELMNT)) != NULL)
                if (strcmp(xml_name(xs), "sub-interface") == 0 && (id = xml_find_body(xs, "id")) != NULL) {
                    snprintf(buf, sizeof(buf), "%s.%s", name, id);
                    desc = xml_find_body(xs, "description");
                    cvec_add_string(out, buf, desc ? desc : "");
                }
        }
        free(vec);
        vec = NULL;
    }
    cvec_free(nsc);
    xml_free(xt);
    return 0;
}

/*! Description of ifname in the candidate list, or NULL. */
static const char *
candidate_desc(cvec       *cand,
               const char *ifname)
{
    cg_var *cv = NULL;

    while ((cv = cvec_each(cand, cv)) != NULL)
        if (strcmp(cv_name_get(cv), ifname) == 0 && cv_string_get(cv)[0] != '\0')
            return cv_string_get(cv);
    return NULL;
}

/*! Help text for a completion candidate: interface type, commit state and description. */
static void
if_help(const char *name,
        int         in_vpp,
        const char *desc,
        char       *buf,
        size_t      len)
{
    char        parent[64];
    char        subid[16];
    const char *type;

    switch (if_classify(name, parent, sizeof(parent), subid, sizeof(subid))) {
    case IF_SUB:  type = "Sub-interface"; break;
    case IF_LOOP: type = "Loopback"; break;
    case IF_BOND: type = "Bond (LAG)"; break;
    default:      type = "Physical interface"; break;
    }
    snprintf(buf, len, "%s%s%s%s", type,
             in_vpp ? "" : " (not yet committed)",
             desc ? ": " : "", desc ? desc : "");
}

/*! Add a completion candidate with help text unless it is already listed. */
static void
add_completion(cvec       *commands,
               cvec       *helptexts,
               const char *name,
               const char *help)
{
    cg_var *cv = NULL;

    while ((cv = cvec_each(commands, cv)) != NULL)
        if (strcmp(cv_string_get(cv), name) == 0)
            return;
    cvec_add_string(commands, NULL, (char *)name);
    if (helptexts)
        cvec_add_string(helptexts, NULL, (char *)help);
}

/*! Next free index for names "<prefix>N" among the listed names. */
static int
next_index(cvec       *names,
           const char *prefix)
{
    cg_var     *cv = NULL;
    size_t      plen = strlen(prefix);
    const char *n;
    int         next = 0;

    while ((cv = cvec_each(names, cv)) != NULL) {
        n = cv_string_get(cv);
        if (strncmp(n, prefix, plen) == 0 && all_digits(n + plen) && atoi(n + plen) >= next)
            next = atoi(n + plen) + 1;
    }
    return next;
}

/*! Tab completion of interface names: VPP interfaces, interfaces only present in the candidate,
 * and (with clispec argument "new") the next free loopN and BondEthernetN names.
 * With argument "phy", only physical interfaces that exist in VPP (bond members).
 * Help texts show the interface type and its configured description. */
int
vc_expand_ifs(clixon_handle h,
              char         *name,
              cvec         *cvv,
              cvec         *argv,
              cvec         *commands,
              cvec         *helptexts)
{
    cvec       *vpp = cvec_new(0);
    cvec       *cand = cvec_new(0);
    cg_var     *cv = NULL;
    const char *mode = arg_str(argv, 0);
    const char *ifn;
    char        parent[64];
    char        subid[16];
    char        help[160];
    char        buf[64];
    int         phy_only = mode != NULL && strcmp(mode, "phy") == 0;

    vpp_ifnames(vpp);
    candidate_ifnames(h, cand);
    while ((cv = cvec_each(vpp, cv)) != NULL) {
        ifn = cv_string_get(cv);
        if (phy_only && if_classify(ifn, parent, sizeof(parent), subid, sizeof(subid)) != IF_PHY)
            continue;
        if_help(ifn, 1, candidate_desc(cand, ifn), help, sizeof(help));
        add_completion(commands, helptexts, ifn, help);
    }
    if (!phy_only)
        while ((cv = cvec_each(cand, cv)) != NULL) {
            ifn = cv_name_get(cv);
            if_help(ifn, 0, candidate_desc(cand, ifn), help, sizeof(help));
            add_completion(commands, helptexts, ifn, help);
        }
    if (mode != NULL && strcmp(mode, "new") == 0) {
        snprintf(buf, sizeof(buf), "loop%d", next_index(commands, "loop"));
        add_completion(commands, helptexts, buf, "New loopback (created on commit)");
        snprintf(buf, sizeof(buf), "BondEthernet%d", next_index(commands, "BondEthernet"));
        add_completion(commands, helptexts, buf, "New bond (LAG), created on commit; add members with 'member'");
    }
    cvec_free(vpp);
    cvec_free(cand);
    return 0;
}

/*! Print every error to the terminal synchronously, Cisco style ("% ...").
 * rtr runs clixon_cli with logging to syslog (-l s), so Clixon's own timestamped messages stay
 * off the terminal but are still recorded in the journal.
 * Backend rpc-errors are condensed to "% Commit failed: <reason from vppcfg>". */
int
vc_errmsg(clixon_handle        h,
          const char          *fn,
          const int            line,
          enum clixon_log_type type,
          int                 *category,
          int                 *suberr,
          cxobj               *xerr,
          const char          *format,
          va_list              ap,
          cbuf               **cbmsg)
{
    const char *what = "Operation";
    char       *msg;
    char        buf[2048];
    va_list     ap2;

    if (type != LOG_TYPE_ERR)
        return 0;
    if (xerr != NULL && (msg = xml_find_body(xerr, "error-message")) != NULL) {
        if (strncmp(format, "Commit", 6) == 0)
            what = "Commit";
        else if (strncmp(format, "Validate", 8) == 0)
            what = "Validation";
        cligen_output(stdout, "%% %s failed: %s\n", what, msg);
        return 0;
    }
    va_copy(ap2, ap);
    vsnprintf(buf, sizeof(buf), format, ap2);
    va_end(ap2);
    cligen_output(stdout, "%% %s\n", buf);
    return 0;
}

clixon_plugin_api *clixon_plugin_init(clixon_handle h);

static clixon_plugin_api api = {
    .ca_name   = "cli_vppcfg",
    .ca_init   = clixon_plugin_init,
    .ca_errmsg = vc_errmsg,
};

clixon_plugin_api *
clixon_plugin_init(clixon_handle h)
{
    return &api;
}
