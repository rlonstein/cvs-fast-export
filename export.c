/*
 *  Copyright © 2006 Keith Packard <keithp@keithp.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 */
#include <limits.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>

/*
 * Blob compression with zlib is not enabled by default because, (a) in general,
 * any repository large enough to hit a disk-space limit is likely to hit
 * a core limit on metadata sooner, and(b) compression costs time.  The
 * option has been left in place for unusual circumstances and can be enabled
 * from the Makefile.
 */
#ifdef ZLIB
#include <zlib.h>
#endif

#include "cvs.h"

/*
 * This code is somewhat complex because the natural order of operations
 * generated by the file-traversal operations in the rest of the code is
 * not even remotely like the canonical order generated by git-fast-export.
 * We want to emulate the latter in order to make regression-testing and
 * comparisons with other tools as easy as possible.
 */

struct mark {
    serial_t external;
    bool emitted;
};
static struct mark *markmap;
static serial_t seqno, mark;
static char blobdir[PATH_MAX];
static serial_t export_total_commits;

/*
 * GNU CVS default ignores.  We omit from this things that CVS ignores
 * by default but which are highly unlikely to turn up outside an
 * actual CVS repository and should be conspicuous if they do: RCS
 * SCCS CVS CVS.adm RCSLOG cvslog.*
 */
#define CVS_IGNORES "# CVS default ignores begin\ntags\nTAGS\n.make.state\n.nse_depinfo\n*~\n#*\n.#*\n,*\n_$*\n*$\n*.old\n*.bak\n*.BAK\n*.orig\n*.rej\n.del-*\n*.a\n*.olb\n*.o\n*.obj\n*.so\n*.exe\n*.Z\n*.elc\n*.ln\ncore\n# CVS default ignores end\n"

void save_status_end(time_t start_time)
{
    if (!progress)
	return;
    else {
	time_t elapsed = time(NULL) - start_time;
	struct rusage rusage;

	(void)getrusage(RUSAGE_SELF, &rusage);
	progress_end("100%%, %d commits in %dsec (%d commits/sec) using %ldKb.",
		     export_total_commits,
		     (int)elapsed,
		     (int)(export_total_commits / (elapsed > 0 ? elapsed : 1)),
		     rusage.ru_maxrss);
    }
}


void export_init(void)
{
    char *tmp = getenv("TMPDIR");
    if (tmp == NULL) 
    	tmp = "/tmp";
    seqno = mark = 0;
    snprintf(blobdir, sizeof(blobdir), "%s/cvs-fast-export-XXXXXXXXXX", tmp);
    if (mkdtemp(blobdir) == NULL)
	fatal_error("temp dir creation failed\n");
}

static char *blobfile(int serial, bool create)
/* Random-access location of the blob corresponding to the specified serial */
{
    static char path[PATH_MAX];
    int m;

#ifdef FDEBUG
    (void)fprintf(stderr, "-> blobfile(%d, %d)...\n", serial, create);
#endif /* FDEBUG */
    (void)snprintf(path, sizeof(path), "%s", blobdir);
    /*
     * FANOUT should be chosen to be the largest directory size that does not
     * cause slow secondary allocations.  It's something near 256 on ext4
     * (we think...)
     */
#define FANOUT	256
    for (m = serial;;)
    {
	int digit = m % FANOUT;
	if ((m = (m - digit) / FANOUT) == 0) {
	    (void)snprintf(path + strlen(path), sizeof(path) - strlen(path),
			   "/=%x", digit);
#ifdef FDEBUG
	    (void)fprintf(stderr, "path: %s\n", path);
#endif /* FDEBUG */
	    break;
	}
	else
	{
	    (void)snprintf(path + strlen(path), sizeof(path) - strlen(path),
			   "/%x", digit);
	    /* coverity[toctou] */
#ifdef FDEBUG
	    (void)fprintf(stderr, "directory: %s\n", path);
#endif /* FDEBUG */
	    if (create && access(path, R_OK) != 0) {
#ifdef FDEBUG
		(void)fprintf(stderr, "directory: %s\n", path);
#endif /* FDEBUG */
		if (mkdir(path,S_IRWXU | S_IRWXG) != 0)
		    fatal_error("blob subdir creation of %s failed\n", path);
	    }
	}
    }
#undef FANOUT
#ifdef FDEBUG
    (void)fprintf(stderr, "<- ...returned path for %d = %s\n", serial, path);
#endif /* FDEBUG */
    return path;
}

void export_blob(node_t *node, void *buf, size_t len)
/* save the blob where it will be available for random access */
{
    size_t extralen = 0;
#ifndef ZLIB
    FILE *wfp;
#else
    gzFile wfp;
#endif

    if (strcmp(node->file->file_name + striplen, ".cvsignore,v") == 0) {
	extralen = sizeof(CVS_IGNORES) - 1;
    }
    
    if (seqno >= MAX_SERIAL_T)
	fatal_error("snapshot sequence number too large, widen serial_t");
    node->file->serial = ++seqno;

#ifndef ZLIB
    wfp = fopen(blobfile(seqno, true), "w");
#else
    /*
     * Blobs are written compressed.  This costs a little compression time,
     * but we get it back in reduced disk seeks.
     */
    wfp = gzopen(blobfile(seqno, true), "w");
#endif
    if (wfp == NULL)
	fatal_system_error("blobfile open");
#ifndef ZLIB
    fprintf(wfp, "data %zd\n", len + extralen);
    if (extralen > 0)
	fwrite(CVS_IGNORES, extralen, sizeof(char), wfp);
    fwrite(buf, len, sizeof(char), wfp);
    fputc('\n', wfp);
    (void)fclose(wfp);
#else
    gzprintf(wfp, "data %zd\n", len + extralen);
    if (extralen > 0)
	gzwrite(CVS_IGNORES, extralen, sizeof(char), wfp);
    gzwrite(wfp, buf, len);
    gzputc(wfp, '\n');
    (void)gzclose(wfp);
#endif
}

static void drop_path_component(char *string, const char *drop)
{
    char *c;
    int  m;
    m = strlen(drop);
    while ((c = strstr(string, drop)) &&
	   (c == string || c[-1] == '/'))
    {
	int l = strlen(c);
	memmove(c, c + m, l - m + 1);
    }
}

static char *export_filename(rev_file *file, const bool ignoreconv)
{
    static char name[PATH_MAX];
    const char *file_name = file->file_name;
    unsigned len;
    const char *s, *snext;
    char *p;
    
    /*
     * This function is another hot spot.
     * All the path modifications are now made as the result
     * string is constructed.
     */
    p = name;
    s = file_name + striplen;
    while (*s) {
	for (snext = s; *snext; snext++)
	    if (*snext == '/') {
	        ++snext;
		/* assert(*snext != '\0'); */
	        break;
	    }
	len = snext - s;
	/* special processing for final components */
	if (*snext == '\0') {
	    /* trim trailing ,v */
	    if (len > 2 && s[len - 2] == ',' && s[len - 1] == 'v')
	        len -= 2;
	    /* convert foo/.cvsignore to foo/.gitignore */
	    if (ignoreconv && len == 10 && memcmp(s, ".cvsignore", len) == 0)
	    {
	        s = ".gitignore";
	        /* len = 10; */
	    }
	} else { /* s[len-1] == '/' */
	    /* drop some path components */
	    if (len == sizeof "Attic" && memcmp(s, "Attic/", len) == 0)
	        goto skip;
	    if (len == sizeof "RCS" && memcmp(s, "RCS/", len) == 0)
		goto skip;
	}
	/* copy the path component */
	if (p + len >= name + sizeof name)
	    fatal_error("File name %s\n too long\n", file_name);
	memcpy(p, s, len);
	p += len;
    skip:
	s = snext;
    }
    *p = '\0';
    len = p - name;

    return name;
}

void export_wrap(void)
/* clean up after export, removing the blob storage */
{
    (void) puts("done");

    /* Remove files and directories in the order created */
    while (seqno) {
        char *path = blobfile(seqno, false);
        (void) unlink(path);
        int len = strlen(path);
        if (len > 3 && path[len-3] == '/'
                    && path[len-2] == '='
                    && path[len-1] == '0') {
            path[len-3] = '\0';
            (void) rmdir(path);
        }
        seqno--;
    }

    if (rmdir(blobdir) == -1)
	perror(blobdir);
}

static const char *utc_offset_timestamp(const time_t *timep, const char *tz)
{
    static char outbuf[BUFSIZ];
    struct tm *tm;
    char tzbuf[BUFSIZ];
    /* coverity[tainted_string_return_content] */
    char *oldtz = getenv("TZ");

    // make a copy in case original is clobbered
    if (oldtz != NULL)
	strncpy(tzbuf, oldtz, sizeof(tzbuf)-1);

    setenv("TZ", tz, 1);
    tzset();  // just in case ...

    tm = localtime(timep);
#ifndef __CYGWIN__
    strftime(outbuf, sizeof(outbuf), "%s %z", tm);
#else
		// Cygdwin doesn't have %s for strftime
    int x = sprintf(outbuf, "%li", *timep);
    strftime(outbuf + x, sizeof(outbuf) - x, " %z", tm);
#endif
    if (oldtz != NULL)
	setenv("TZ", tzbuf, 1);
    else
	unsetenv("TZ");
    tzset();

    return outbuf;
}

struct fileop {
    char op;
    mode_t mode;
    serial_t serial;
    const char *path;
};

static int fileop_sort(const void *a, const void *b)
/* sort fileops as git fast-export does */
{
    /* As it says, 'Handle files below a directory first, in case they are
     * all deleted and the directory changes to a file or symlink.'
     * Because this doesn't have to handle renames, just sort lexicographically
     * We append a sentinel to make sure "a/b/c" < "a/b" < "a".
     */
    struct fileop *oa = (struct fileop *)a;
    struct fileop *ob = (struct fileop *)b;

    return path_deep_compare(oa->path, ob->path);
}

#define display_date(c, m)	(force_dates ? ((m) * commit_time_window * 2) : ((c)->date + RCS_EPOCH))

/*
 * An iterator structure over the sorted files in a git_commit
 */
typedef struct _file_iter {
    rev_dir * const *dir;
    rev_dir * const *dirmax;
    rev_file **file;
    rev_file **filemax;
} file_iter;

static rev_file *
file_iter_next(file_iter *pos) {
    if (pos->dir == pos->dirmax)
        return NULL;
again:
    if (pos->file != pos->filemax)
	return *pos->file++;
    ++pos->dir;
    if (pos->dir == pos->dirmax)
        return NULL;
    pos->file = (*pos->dir)->files;
    pos->filemax = pos->file + (*pos->dir)->nfiles;
    goto again;
}

static void
file_iter_start(file_iter *pos, const git_commit *commit) {
    pos->dir = commit->dirs;
    pos->dirmax = commit->dirs + commit->ndirs;
    if (pos->dir != pos->dirmax) {
        pos->file = (*pos->dir)->files;
        pos->filemax = pos->file + (*pos->dir)->nfiles;
    } else {
        pos->file = pos->filemax = NULL;
    }
}

static void compute_parent_links(const git_commit *commit)
/* create reciprocal link pairs between file refs in a commit and its parent */
{
    const git_commit *parent = commit->parent;
    file_iter commit_iter, parent_iter;
    rev_file *cf, *pf;
    unsigned nparent, ncommit, maxmatch;

    ncommit = 0;
    file_iter_start(&commit_iter, commit);
    while ((cf = file_iter_next(&commit_iter))) {
	++ncommit;
        cf->u.other = NULL;
    }

    nparent = 0;
    file_iter_start(&parent_iter, parent);
    while ((pf = file_iter_next(&parent_iter))) {
	++nparent;
        pf->u.other = NULL;
    }

    maxmatch = (nparent < ncommit) ? nparent : ncommit;

    file_iter_start(&commit_iter, commit);
    file_iter_start(&parent_iter, parent);
    while ((cf = file_iter_next(&commit_iter))) {
	file_iter it;
	const bloom_t *bloom = atom_bloom(cf->file_name);
	unsigned k;

	for (k = 0; k < BLOOMLENGTH; ++k) {
	    if (bloom->el[k] & parent->bloom.el[k]) {
	        goto next;
	    }
	}

	/* Because the commit file lists are sorted,
	 * we can restart the iterator after the
	 * last successful match */
	it = parent_iter;
	while ((pf = file_iter_next(&it))) {
	    if (cf->file_name == pf->file_name) {
		cf->u.other = pf;
		pf->u.other = cf;
		if (--maxmatch == 0)
		    return;
		parent_iter = it;
		break;
	    }
	}

        next:;
    }
}

#ifdef ORDERDEBUG
static void dump_file(rev_file *rev_file, FILE *fp)
{
    char buf[CVS_MAX_REV_LEN + 1];
    fprintf(fp, "   file name: %s %s\n", rev_file->file_name, 
	    cvs_number_string(&rev_file->number, buf, sizeof(buf)));
 }

static void dump_dir(rev_dir *rev_dir, FILE *fp)
{
    int i;

    fprintf(fp, "   file count: %d\n", rev_dir->nfiles);
    for (i = 0; i < rev_dir->nfiles; i++)
	dump_file(rev_dir->files[i], fp);
}

static void dump_commit(git_commit *commit, FILE *fp)
{
    int i;
    fprintf(fp, "commit %p seq %d mark %d nfiles: %d, ndirs = %d\n", 
	    commit, seqno, markmap[seqno].external, commit->nfiles, commit->ndirs);
    for (i = 0; i < commit->ndirs; i++)
	dump_dir(commit->dirs[i], fp);
}
#endif /* ORDERDEBUG */

static void export_commit(git_commit *commit, 
			  const char *branch_prefix, const char *branch, 
			  bool report, FILE *revmap,
			  bool reposurgeon, bool force_dates)
/* export a commit(and the blobs it is the first to reference) */
{
#define OP_CHUNK	32
    cvs_author *author;
    const char *full;
    const char *email;
    const char *timezone;
    char *revpairs = NULL;
    size_t revpairsize = 0;
    time_t ct;
    rev_file	*f;
    int		i, j;
    struct fileop *operations, *op, *op2;
    int noperations;
    serial_t here;
    static const char *s_gitignore;

    if (!s_gitignore) s_gitignore = atom(".gitignore");

    if (reposurgeon || revmap != NULL)
    {
	revpairs = xmalloc((revpairsize = 1024), "revpair allocation");
	revpairs[0] = '\0';
    }

    /*
     * Precompute mutual parent-child pointers.
     */
    if (commit->parent) 
	compute_parent_links(commit);

    noperations = OP_CHUNK;
    op = operations = xmalloc(sizeof(struct fileop) * noperations, "fileop allocation");
    for (i = 0; i < commit->ndirs; i++) {
	rev_dir	*dir = commit->dirs[i];
	
	for (j = 0; j < dir->nfiles; j++) {
	    char *stripped;
	    bool present, changed;
	    f = dir->files[j];
	    stripped = export_filename(f, true);
	    present = false;
	    changed = false;
	    if (commit->parent) {
		present = (f->u.other != NULL);
		changed = present && (f->serial != f->u.other->serial);
	    }
	    if (!present || changed) {

		op->op = 'M';
		// git fast-import only supports 644 and 755 file modes
		if (f->mode & 0100)
		    op->mode = 0755;
		else
		    op->mode = 0644;
		op->serial = f->serial;
		op->path = atom(stripped);
		op++;
		if (op == operations + noperations)
		{
		    noperations += OP_CHUNK;
		    operations = xrealloc(operations,
					  sizeof(struct fileop) * noperations, __func__);
		    // realloc can move operations
		    op = operations + noperations - OP_CHUNK;
		}

		if (revmap != NULL || reposurgeon) {
		    char fr[BUFSIZ];
		    stringify_revision(export_filename(f, false), 
				  " ", &f->number, fr, sizeof fr);
		    if (reposurgeon)
		    {
			if (strlen(revpairs) + strlen(fr) + 2 > revpairsize)
			{
			    revpairsize *= 2;
			    revpairs = xrealloc(revpairs, revpairsize, "revpair allocation");
			}
			strcat(revpairs, fr);
			strcat(revpairs, "\n");
		    }
		}
	    }
	}
    }

    if (commit->parent)
    {
	for (i = 0; i < commit->parent->ndirs; i++) {
	    rev_dir	*dir = commit->parent->dirs[i];

	    for (j = 0; j < dir->nfiles; j++) {
		bool present;
		f = dir->files[j];
		present = (f->u.other != NULL);
		if (!present) {
		    op->op = 'D';
		    op->path = atom(export_filename(f, true));
		    op++;
		    if (op == operations + noperations)
		    {
			noperations += OP_CHUNK;
			operations = xrealloc(operations,
					      sizeof(struct fileop) * noperations,
					      __func__);
			// realloc can move operations
			op = operations + noperations - OP_CHUNK;
		    }
		}
	    }
	}
    }

    for (op2 = operations; op2 < op; op2++)
    {
	if (op2->op == 'M' && !markmap[op2->serial].emitted)
	{
	    markmap[op2->serial].external = ++mark;
	    if (report) {
		char *fn = blobfile(op2->serial, false);
#ifndef ZLIB
		FILE *rfp = fopen(fn, "r");
#else
		gzFile rfp = gzopen(fn, "r");
#endif
		if (rfp)
		{
		    int c;
		    printf("blob\nmark :%d\n", mark);
#ifndef ZLIB
		    while ((c = fgetc(rfp)) != EOF)
#else
		    while ((c = gzgetc(rfp)) != EOF)
#endif
			putchar(c);
		    (void) unlink(fn);
		    markmap[op2->serial].emitted = true;
#ifndef ZLIB
		    (void)fclose(rfp);
#else
		    (void)gzclose(rfp);
#endif
		}
	    }
	}
    }

    /* sort operations into canonical order */
    qsort((void *)operations, op - operations, sizeof(struct fileop), fileop_sort); 

    author = fullname(commit->author);
    if (!author) {
	full = commit->author;
	email = commit->author;
	timezone = "UTC";
    } else {
	full = author->full;
	email = author->email;
	timezone = author->timezone ? author->timezone : "UTC";
    }

    if (report)
	printf("commit %s%s\n", branch_prefix, branch);
    here = markmap[++seqno].external = ++mark;
#ifdef ORDERDEBUG2
    /* can't move before mark is updated */
    dump_commit(commit, stderr);
#endif /* ORDERDEBUG2 */
    if (report)
	printf("mark :%d\n", mark);
    commit->serial = seqno;
    if (report) {
	static bool need_ignores = true;
	const char *ts;
	ct = display_date(commit, mark);
	ts = utc_offset_timestamp(&ct, timezone);
	//printf("author %s <%s> %s\n", full, email, ts);
	printf("committer %s <%s> %s\n", full, email, ts);
	printf("data %zd\n%s\n", strlen(commit->log), commit->log);
	if (commit->parent)
	    printf("from :%d\n", markmap[commit->parent->serial].external);

	for (op2 = operations; op2 < op; op2++)
	{
	    assert(op2->op == 'M' || op2->op == 'D');
	    if (op2->op == 'M')
		printf("M 100%o :%d %s\n", 
		       op2->mode, 
		       markmap[op2->serial].external, 
		       op2->path);
	    if (op2->op == 'D')
		printf("D %s\n", op2->path);
	    /*
	     * If there's a .gitignore in the first commit, don't generate one.
	     * export_blob() will already have prepended them.
	     */
	    if (need_ignores && op2->path == s_gitignore)
		need_ignores = false;
	}
	if (need_ignores) {
	    need_ignores = false;
	    printf("M 100644 inline .gitignore\ndata %zd\n%s\n",
		   sizeof(CVS_IGNORES)-1, CVS_IGNORES);
	}
    }
    free(operations);

    if (revmap) {
	char *cp;
	for (cp = revpairs; *cp; cp++) {
	    if (*cp == '\n')
		fprintf(revmap, " :%d", here);
	    fputc(*cp, revmap);
	}
    }
    if (reposurgeon) 
    {
	if (report)
	    printf("property cvs-revision %zd %s", strlen(revpairs), revpairs);
    }
    if (reposurgeon || revmap != NULL)
	free(revpairs);

    if (report)
	printf("\n");
#undef OP_CHUNK
}

static int export_ncommit(rev_list *rl)
/* return a count of converted commits */
{
    rev_ref	*h;
    git_commit	*c;
    int		n = 0;
    
    for (h = rl->heads; h; h = h->next) {
	if (h->tail)
	    continue;
	/* PUNNING: see the big comment in cvs.h */ 
	for (c = (git_commit *)h->commit; c; c = c->parent) {
	    n++;
	    if (c->tail)
		break;
	}
    }
    return n;
}

struct commit_seq {
    git_commit *commit;
    rev_ref *head;
    bool realized;
};

static int sort_by_date(const void *ap, const void *bp)
{
    struct commit_seq *ac = (struct commit_seq *)ap;
    struct commit_seq *bc = (struct commit_seq *)bp;

    return ac->commit->date - bc->commit->date;
}

bool export_commits(rev_list *rl, 
		    const char *branch_prefix,
		    time_t fromtime,
		    const char *revision_map,
		    bool reposurgeon,
		    bool force_dates,
		    bool branchorder, 
		    bool progress)
/* export a revision list as a git fast-import stream in canonical order */
{
    rev_ref *h;
    Tag *t;
    git_commit *c;
    int n;
    size_t extent;
    FILE *revmap = NULL;

    export_total_commits = export_ncommit(rl);
    /* the +1 is because mark indices are 1-origin, slot 0 always empty */
    extent = sizeof(struct mark) * (seqno + export_total_commits + 1);
    markmap = (struct mark *)xmalloc(extent, "markmap allocation");
    memset(markmap, '\0', extent);
    if (revision_map != 0)
	revmap = fopen(revision_map, "w");

    progress_begin("Save: ", export_total_commits);

    if (branchorder) {
	/*
	 * Dump by branch order, not by commit date.  Slightly faster and
	 * less memory-intensive, but (a) incremental dump won't work, and
	 * (b) it's not git-fast-export  canonical form and cannot be 
	 * directly compared to the output of other tools.
	 */
	git_commit **history;
	int alloc, i;

	for (h = rl->heads; h; h = h->next) {
	    if (!h->tail) {
		// We need to export commits in reverse order; so
		// first of all, we convert the linked-list given by
		// h->commit into the array "history".
		history = NULL;
		alloc = 0;
		/* PUNNING: see the big comment in cvs.h */ 
		for (c=(git_commit *)h->commit, n=0; c; c=(c->tail ? NULL : c->parent), n++) {
		    if (n >= alloc) {
			alloc += 1024;
			history = (git_commit **)xrealloc(history, alloc *sizeof(git_commit *), "export");
		    }
		    history[n] = c;
		}

		/*
		 * Now walk the history array in reverse order and export the
		 * commits, along with any matching tags.
		 */
		for (i=n-1; i>=0; i--) {
		    export_commit(history[i], branch_prefix, h->ref_name, 
				  true, revmap, reposurgeon, force_dates);
		    progress_step();
		    for (t = all_tags; t; t = t->next)
			if (t->commit == history[i])
			    printf("reset refs/tags/%s\nfrom :%d\n\n", t->name, markmap[history[i]->serial].external);
		}

		free(history);
	    }
	}
    }
    else 
    {
	/*
	 * Dump in strict git-fast-export order.
	 *
	 * Commits are in reverse order on per-branch lists.  The branches
	 * have to ship in their current order, otherwise some marks may not 
	 * be resolved.
	 *
	 * Dump them all into a common array because (a) we're going to
	 * need to ship them back to front, and (b) we'd prefer to ship
	 * them in canonical order by commit date rather than ordered by
	 * branches.
	 *
	 * But there's a hitch; the branches themselves need to be dumped
	 * in forward order, otherwise not all ancestor marks will be defined.
	 * Since the branch commits need to be dumped in reverse, the easiest
	 * way to arrange this is to reverse the branches in the array, fill
	 * the array in forward order, and dump it forward order.
	 */
	struct commit_seq *history, *hp;
	bool sortable;
	int branchbase;

	history = (struct commit_seq *)xcalloc(export_total_commits, 
					       sizeof(struct commit_seq),
					       "export");
#ifdef ORDERDEBUG
	fputs("Export phase 1:\n", stderr);
#endif /* ORDERDEBUG */
	branchbase = 0;
	for (h = rl->heads; h; h = h->next) {
	    if (!h->tail) {
		int i = 0, branchlength = 0;
		/* PUNNING: see the big comment in cvs.h */ 
		for (c = (git_commit *)h->commit; c; c = (c->tail ? NULL : c->parent))
		    branchlength++;
		/* PUNNING: see the big comment in cvs.h */ 
		for (c = (git_commit *)h->commit; c; c = (c->tail ? NULL : c->parent)) {
		    /* copy commits in reverse order into this branch's span */
		    n = branchbase + branchlength - (i + 1);
		    history[n].commit = c;
		    history[n].head = h;
		    i++;
#ifdef ORDERDEBUG
		    fprintf(stderr, "At n = %d, i = %d\n", n, i);
		    dump_commit(c, stderr);
#endif /* ORDERDEBUG */
		}
		branchbase += branchlength;
	    }
	}
 
#ifdef ORDERDEBUG2
	fputs("Export phase 2:\n", stderr);
	for (hp = history; hp < history + export_total_commits; hp++)
	    dump_commit(hp->commit, stderr);
#endif /* ORDERDEBUG2 */

	/* 
	 * Check that the topo order is consistent with time order.
	 * If so, we can sort commits by date without worrying that
	 * we'll try to ship a mark before it's defined.
	 */
	sortable = true;
	for (hp = history; hp < history + export_total_commits; hp++) {
	    if (hp->commit->parent && hp->commit->parent->date > hp->commit->date) {
		sortable = false;
		announce("some parent commits are younger than children.\n");
		break;
	    }
	}
	if (sortable)
	    qsort((void *)history, 
		  export_total_commits, sizeof(struct commit_seq),
		  sort_by_date);

#ifdef ORDERDEBUG2
	fputs("Export phase 3:\n", stderr);
#endif /* ORDERDEBUG2 */
	for (hp = history; hp < history + export_total_commits; hp++) {
	    bool report = true;
	    if (fromtime > 0) {
		if (fromtime >= display_date(hp->commit, mark+1)) {
		    report = false;
		} else if (!hp->realized) {
		    struct commit_seq *lp;
		    if (hp->commit->parent != NULL && display_date(hp->commit->parent, markmap[hp->commit->parent->serial].external) < fromtime)
			(void)printf("from %s%s^0\n\n", branch_prefix, hp->head->ref_name);
		    for (lp = hp; lp < history + export_total_commits; lp++) {
			if (lp->head == hp->head) {
			    lp->realized = true;
			}
		    }
		}
	    }
	    progress_jump(hp - history);
	    export_commit(hp->commit, branch_prefix, hp->head->ref_name,
			  report, revmap, reposurgeon, force_dates);
	    for (t = all_tags; t; t = t->next)
		if (t->commit == hp->commit)
		    printf("reset refs/tags/%s\nfrom :%d\n\n", t->name, markmap[hp->commit->serial].external);
	}

	free(history);
    }

    for (h = rl->heads; h; h = h->next) {
	printf("reset %s%s\nfrom :%d\n\n", 
	       branch_prefix, 
	       h->ref_name, 
	       markmap[h->commit->serial].external);
    }
    free(markmap);

    if (revmap != NULL)
	fclose(revmap);

    return true;
}

/* end */
