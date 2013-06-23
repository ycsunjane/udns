/* $Id: rblcheck.c,v 1.2 2004/07/03 01:14:36 mjt Exp $
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include "udns.h"

static const char *version = "udns-rblcheck 0.1";
static char *progname;

struct rbl {
  const char *zone;
  struct rbl *next;
};

struct rblookup {
  struct ipcheck *parent;
  struct in_addr key;
  const char *zone;
  struct dns_rr_a4  *addr;
  struct dns_rr_txt *txt;
};

struct ipcheck {
  const char *name;
  int naddr;
  int listed;
  struct rblookup *lookup;
};

#define notlisted ((void*)1)

static int nzones;
struct rbl *rbllist, **lastrbl = &rbllist;

static int do_txt;
static int verbose = 1;
/* verbosity level:
 * <0 - only bare As/TXTs
 * 0 - what RBL result
 * 1(default) - what is listed by RBL: result
 * 2          - what is[not ]listed by RBL: result, name lookups
 */

static int listed;
static int failures;

static time_t now;

static void *ecalloc(int size, int cnt) {
  void *t = calloc(size, cnt);
  if (!t) {
    fprintf(stderr, "%s: out of memory\n", progname);
    exit(1);
  }
  return t;
}

static void dnserror(struct rblookup *ipl, const char *what) {
  fprintf(stderr, "%s: unable to %s for %s(%s): %s\n",
          progname, what, inet_ntoa(ipl->key), ipl->zone,
          dns_strerror(dns_status(0)));
  ++failures;
}

static void display_result(struct ipcheck *x) {
  int j;
  struct rblookup *l, *le;
  if (!x->naddr) return;
  for (l = x->lookup, le = l + nzones * x->naddr; l < le; ++l) {
    if (!l->addr) continue;
    if (verbose < 2 && l->addr == notlisted) continue;
    if (verbose >= 0) {
      if (x->name) printf("%s[%s]", x->name, inet_ntoa(l->key));
      else printf("%s", inet_ntoa(l->key));
    }
    if (l->addr == notlisted) {
      printf(" is NOT listed by %s\n", l->zone);
      continue;
    }
    else if (verbose >= 1)
      printf(" is listed by %s: ", l->zone);
    else if (verbose >= 0)
      printf(" %s ", l->zone);
    if (verbose >= 1 || !do_txt)
      for (j = 0; j < l->addr->dnsa4_nrr; ++j)
        printf("%s%s", j ? " " : "", inet_ntoa(l->addr->dnsa4_addr[j]));
    if (!do_txt) ;
    else if (l->txt) {
      for(j = 0; j < l->txt->dnstxt_nrr; ++j) {
        unsigned char *t = l->txt->dnstxt_txt[j].txt;
        unsigned char *e = t + l->txt->dnstxt_txt[j].len;
        printf("%s\"", verbose > 0 ? "\n\t" : j ? " " : "");
        while(t < e) {
          if (*t < ' ' || *t >= 127) printf("\\x%02x", *t);
          else if (*t == '\\' || *t == '"') printf("\\%c", *t);
          else putchar(*t);
          ++t;
        }
        putchar('"');
      }
      free(l->txt);
    }
    else
      printf("<no text available>");
    free(l->addr);
    putchar('\n');
  }
  free(x->lookup);
}

static void txtcb(struct dns_ctx *ctx, struct dns_rr_txt *r, void *data) {
  struct rblookup *ipl = data;
  if (r)
    ipl->txt = r;
  else if (dns_status(ctx) != DNS_E_NXDOMAIN)
    dnserror(ipl, "lookup DNSBL TXT record");
}

static void a4cb(struct dns_ctx *ctx, struct dns_rr_a4 *r, void *data) {
  struct rblookup *ipl = data;
  if (r) {
    ipl->addr = r;
    ++listed;
    ++ipl->parent->listed;
    if (do_txt &&
        !dns_submit_a4dnsbl_txt(0, &ipl->key, ipl->zone, txtcb, ipl, now))
      dnserror(ipl, "submit DNSBL TXT record");
  }
  else if (dns_status(ctx) != DNS_E_NXDOMAIN)
    dnserror(ipl, "lookup DNSBL A record");
  else
    ipl->addr = notlisted;
}

static int submit_a_queries(struct ipcheck *ipc,
		int naddr, const struct in_addr *addr) {
  int z, a;
  struct rbl *rbl = rbllist;
  struct rblookup *rl = ecalloc(sizeof(*rl), nzones * naddr);
  ipc->lookup = rl;
  ipc->naddr = naddr;
  for(z = 0; z < nzones; ++z) {
    for(a = 0; a < naddr; ++a) {
      rl->key = addr[a];
      rl->zone = rbl->zone;
      rl->parent = ipc;
      if (!dns_submit_a4dnsbl(0, &rl->key, rl->zone, a4cb, rl, now))
        dnserror(rl, "submit DNSBL A query");
      ++rl;
    }
    rbl = rbl->next;
  }
  return 0;
}

static void namecb(struct dns_ctx *ctx, struct dns_rr_a4 *rr, void *data) {
  struct ipcheck *ipc = data;
  if (rr) {
    submit_a_queries(ipc, rr->dnsa4_nrr, rr->dnsa4_addr);
    free(rr);
  }
  else
    fprintf(stderr, "%s: unable to lookup %s: %s\n",
            progname, ipc->name, dns_strerror(dns_status(ctx)));
}

static int submit(struct ipcheck *ipc) {
  struct in_addr addr;
  if (inet_aton(ipc->name, &addr)) {
    submit_a_queries(ipc, 1, &addr);
    ipc->name = NULL;
  }
  else if (!dns_submit_a4(0, ipc->name, 0, namecb, ipc, now))
    fprintf(stderr, "%s: unable to submit name query for %s: %s\n",
            progname, ipc->name, dns_strerror(dns_status(0)));
  return 0;
}

static void waitdns(void) {
  struct timeval tv;
  fd_set fds;
  int c;
  int fd = dns_sock(NULL);
  FD_ZERO(&fds);
  while((c = dns_timeouts(NULL, -1, now)) > 0) {
    FD_SET(fd, &fds);
    tv.tv_sec = c;
    tv.tv_usec = 0;
    c = select(fd+1, &fds, NULL, NULL, &tv);
    now = time(NULL);
    if (c > 0)
      dns_ioevent(NULL, now);
  }
}

int main(int argc, char **argv) {
  int c;
  struct rbl *rbl;
  struct ipcheck ipc;

  if (!(progname = strrchr(argv[0], '/'))) progname = argv[0];
  else argv[0] = ++progname;

  while((c = getopt(argc, argv, "hqtvms:n:")) != EOF) switch(c) {
  case 's':
    rbl = ecalloc(sizeof(*rbl), 1);
    rbl->zone = optarg;
    *lastrbl = rbl;
    lastrbl = &rbl->next;
    ++nzones;
    continue;
  case 'q': --verbose; continue;
  case 'v': ++verbose; continue;
  case 't': do_txt = 1; continue;
  case 'm': continue;
  case 'h':
    printf("%s: %s.\n", progname, version);
    printf("Usage is: %s [options] address..\n", progname);
    printf(
"Where options are:\n"
" -h - print this help and exit\n"
" -q - quiet mode, no output\n"
" -t - obtain and print TXT records if any\n"
" -m - stop checking after first address match in any list\n"
" -s service - add the service (DNSBL zone) to teh serice list\n"
    );
    return 0;
  default:
    fprintf(stderr, "%s: use `%s -h' for help\n", progname, progname);
    return 1;
  }

  *lastrbl = NULL;
  if (!nzones) {
    fprintf(stderr, "%s: no service (zone) list specified (-s option)\n",
            progname);
    return 1;
  }

  if (dns_init(1) < 0) {
    fprintf(stderr, "%s: unable to initialize DNS library: %s\n",
            progname, strerror(errno));
    return 1;
  }

  argv += optind;
  argc -= optind;

  now = time(NULL);
  for (c = 0; c < argc; ++c) {
    if (c && (verbose > 1 || (verbose == 1 && do_txt))) putchar('\n');
    ipc.name = argv[c];
    submit(&ipc);
    waitdns();
    display_result(&ipc);
  }

  return listed ? 100 : failures ? 2 : 0;
}