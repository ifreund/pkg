/*-
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * This software was developed by Isaac Freund <ifreund@freebsdfoundation.org>
 * under sponsorship from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <assert.h>

#include "pkg.h"
#include "pkg/vec.h"
#include "private/event.h"
#include "private/pkg.h"
#include "private/pkg_jobs.h"

#define dbg(x, ...) pkg_dbg(PKG_DBG_SCHEDULER, x, __VA_ARGS__)

extern struct pkg_ctx ctx;

static const char *
pkg_jobs_schedule_job_type_string(struct pkg_solved *job)
{
	switch (job->type) {
	case PKG_SOLVED_INSTALL:
		return "install";
	case PKG_SOLVED_DELETE:
		return "delete";
	case PKG_SOLVED_UPGRADE:
		return "upgrade";
	case PKG_SOLVED_UPGRADE_INSTALL:
		return "split upgrade install";
	case PKG_SOLVED_UPGRADE_REMOVE:
		return "split upgrade delete";
	default:
		assert(false);
	}
}

/*
 * Returns true if pkg a directly depends on pkg b.
 *
 * Checking only direct dependencies is sufficient to define the edges in a
 * graph that models indirect dependencies as well as long as all of the
 * intermediate dependencies are also nodes in the graph.
 */
static bool pkg_jobs_schedule_direct_depends(struct pkg *a, struct pkg *b)
{
	struct pkg_dep *dep = NULL;
	while (pkg_deps(a, &dep) == EPKG_OK) {
		if (STREQ(b->uid, dep->uid)) {
			return (true);
		}
	}
	return (false);
}

/* Enable debug logging in pkg_jobs_schedule_graph_edge() */
static bool debug_edges = false;

/*
 * Jobs are nodes in a directed graph. Edges represent job scheduling order
 * requirements. The existence of an edge from node A to node B indicates
 * that job A must be executed before job B.
 *
 * There is a directed edge from node A to node B if and only if
 * one of the following conditions holds:
 *
 * 1. B's new package depends on A's new package
 * 2. A's old package depends on B's old package
 * 3. A's old package conflicts with B's new package
 * 4. A and B are the two halves of a split upgrade job
 *    and A is the delete half.
 */
static bool
pkg_jobs_schedule_graph_edge(struct pkg_solved *a, struct pkg_solved *b)
{
	if (a == b) {
		return (false);
	}

	if (a->xlink == b || b->xlink == a) {
		assert(a->xlink == b && b->xlink == a);
		assert(a->type == PKG_SOLVED_UPGRADE_INSTALL ||
		       a->type == PKG_SOLVED_UPGRADE_REMOVE);
		assert(b->type == PKG_SOLVED_UPGRADE_INSTALL ||
		       b->type == PKG_SOLVED_UPGRADE_REMOVE);
		assert(a->type != b->type);

		bool edge = a->type == PKG_SOLVED_UPGRADE_REMOVE;
		if (edge && debug_edges) {
			dbg(4, "  edge to %s %s, split upgrade",
			    pkg_jobs_schedule_job_type_string(b),
			    b->items[0]->pkg->uid);
		}
		return (edge);
	}

	/* TODO: These switches would be unnecessary if delete jobs used
	 * items[1] rather than items[0]. I suspect other cleanups could
	 * be made as well. */
	struct pkg *a_new = NULL;
	struct pkg *a_old = NULL;
	switch (a->type) {
	case PKG_SOLVED_INSTALL:
	case PKG_SOLVED_UPGRADE_INSTALL:
		a_new = a->items[0]->pkg;
		break;
	case PKG_SOLVED_DELETE:
	case PKG_SOLVED_UPGRADE_REMOVE:
		a_old = a->items[0]->pkg;
		break;
	case PKG_SOLVED_UPGRADE:
		a_new = a->items[0]->pkg;
		a_old = a->items[1]->pkg;
		break;
	default:
		assert(false);
	}

	struct pkg *b_new = NULL;
	struct pkg *b_old = NULL;
	switch (b->type) {
	case PKG_SOLVED_INSTALL:
	case PKG_SOLVED_UPGRADE_INSTALL:
		b_new = b->items[0]->pkg;
		break;
	case PKG_SOLVED_DELETE:
	case PKG_SOLVED_UPGRADE_REMOVE:
		b_old = b->items[0]->pkg;
		break;
	case PKG_SOLVED_UPGRADE:
		b_new = b->items[0]->pkg;
		b_old = b->items[1]->pkg;
		break;
	default:
		assert(false);
	}

	if (a_new != NULL && b_new != NULL &&
	    pkg_jobs_schedule_direct_depends(b_new, a_new)) {
		if (debug_edges) {
			dbg(4, "  edge to %s %s, new depends on new",
			    pkg_jobs_schedule_job_type_string(b),
			    b->items[0]->pkg->uid);
		}
		return (true);
	} else if (a_old != NULL && b_old != NULL &&
		   pkg_jobs_schedule_direct_depends(a_old, b_old)) {
		if (debug_edges) {
			dbg(4, "  edge to %s %s, old depends on old",
			    pkg_jobs_schedule_job_type_string(b),
			    b->items[0]->pkg->uid);
		}
		return (true);
	} else if (a_old != NULL && b_new != NULL) {
		struct pkg_conflict *conflict = NULL;
		while (pkg_conflicts(a_old, &conflict) == EPKG_OK) {
			if (STREQ(b_new->uid, conflict->uid)) {
				if (debug_edges) {
					dbg(4, "  edge to %s %s, old conflicts with new",
					    pkg_jobs_schedule_job_type_string(b),
					    b->items[0]->pkg->uid);
				}
				return (true);
			}
		}
	}

	return (false);
}

static void
pkg_jobs_schedule_dbg_jobs(pkg_solved_list *jobs)
{
	if (ctx.debug_level < 4) {
		return;
	}

	debug_edges = true;
	vec_foreach(*jobs, i) {
		struct pkg_solved *job = jobs->d[i];

		dbg(4, "job: %s %s", pkg_jobs_schedule_job_type_string(job),
		    job->items[0]->pkg->uid);

		vec_foreach(*jobs, j) {
			pkg_jobs_schedule_graph_edge(job, jobs->d[j]);
		}
	}
	debug_edges = false;
}

static bool
pkg_jobs_schedule_has_incoming_edge(pkg_solved_list *nodes,
    struct pkg_solved *node, struct pkg_solved *ignore)
{
	vec_foreach(*nodes, i) {
		if (nodes->d[i] == ignore) {
			continue;
		}
		if (pkg_jobs_schedule_graph_edge(nodes->d[i], node)) {
			return (true);
		}
	}
	return (false);
}

/*
 * Prioritizing the install jobs and deprioritizing the delete jobs of split
 * upgrades reduces the distance between the two halves of the split job in the
 * final execution order.
 */
static int
pkg_jobs_schedule_priority(struct pkg_solved *node)
{
	switch (node->type) {
	case PKG_SOLVED_UPGRADE_INSTALL:
		return 1;
	case PKG_SOLVED_UPGRADE_REMOVE:
		return -1;
	default:
		return 0;
	}
}

/* This comparison function is used as a tiebreaker in the topological sort. */
static int
pkg_jobs_schedule_cmp_available(const void *va, const void *vb)
{
	struct pkg_solved *a = *(struct pkg_solved **)va;
	struct pkg_solved *b = *(struct pkg_solved **)vb;

	int ret = pkg_jobs_schedule_priority(b) - pkg_jobs_schedule_priority(a);
	if (ret == 0) {
		/* Falling back to lexicographical ordering ensures that job execution
		 * order is always consistent and makes testing easier. */
		return strcmp(b->items[0]->pkg->uid, a->items[0]->pkg->uid);
	} else {
		return ret;
	}
}


/* Move all job nodes with no incoming edges from unavailable to available.
 * If node is non-NULL, only checks jobs with an incoming edge from node as
 * an optimization. */
static void
pkg_jobs_schedule_update_available(pkg_solved_list *unavailable,
    pkg_solved_list *available, struct pkg_solved *node)
{
	for (size_t i = 0; i < unavailable->len;) {
		if ((node == NULL || pkg_jobs_schedule_graph_edge(node, unavailable->d[i])) &&
		    !pkg_jobs_schedule_has_incoming_edge(unavailable, unavailable->d[i], NULL) &&
		    !pkg_jobs_schedule_has_incoming_edge(available, unavailable->d[i], NULL)) {
			vec_push(available, unavailable->d[i]);
			vec_swap_remove(unavailable, i);
		} else {
			i++;
		}
	}
}

int pkg_jobs_schedule(struct pkg_jobs *j)
{
	pkg_solved_list unavailable = j->jobs;

	j->jobs = (pkg_solved_list)vec_init();

	/* First split all upgrade jobs. For the purposes of scheduling, the
	 * install/remove parts of upgrade jobs must be treated as two
	 * separate nodes in the graph. This avoids cycles in the graph caused
	 * by conflicts.
	 * During the topological sort, all upgrade jobs that are not required
	 * to remain split (due to a cycle) will be rejoined. No upgrade jobs
	 * can end up unnecessarily split after scheduling completes. */
	vec_foreach(unavailable, i) {
		struct pkg_solved *job = unavailable.d[i];
		if (job->type == PKG_SOLVED_UPGRADE) {
			struct pkg_solved *new = xcalloc(1, sizeof(struct pkg_solved));
			new->type = PKG_SOLVED_UPGRADE_REMOVE;
			new->items[0] = job->items[1];
			new->xlink = job;
			job->type = PKG_SOLVED_UPGRADE_INSTALL;
			job->items[1] = NULL;
			job->xlink = new;
			vec_push(&unavailable, new);
		}
	}

	pkg_jobs_schedule_dbg_jobs(&unavailable);

	/* Topological sort based on Kahn's algorithm with a special tiebreaker that
	 * rejoins upgrade jobs which don't need to remain split. */
	pkg_solved_list available = vec_init();
	pkg_jobs_schedule_update_available(&unavailable, &available, NULL);
	while (available.len > 0) {
		/* If the last job scheduled is a split upgrade remove job, rejoin
		 * it with the upgrade install half if possible. */
		bool found = false;
		if (j->jobs.len > 0 && vec_last(&j->jobs)->xlink != NULL) {
			struct pkg_solved *job = vec_last(&j->jobs);
			assert(job->type == PKG_SOLVED_UPGRADE_REMOVE);
			vec_foreach(available, i) {
				struct pkg_solved *other = available.d[i];
				if (other->xlink == job) {
					assert(other->type == PKG_SOLVED_UPGRADE_INSTALL);
					assert(job->xlink == other);
					job->type = PKG_SOLVED_UPGRADE;
					job->items[1] = job->items[0];
					job->items[0] = other->items[0];
					job->xlink = NULL;
					vec_swap_remove(&available, i);
					pkg_jobs_schedule_update_available(
					    &unavailable, &available, other);
					free(other);
					break;
				}
			}
		}

		vec_foreach(available, i) {
			struct pkg_solved *job = available.d[i];
			if (job->type != PKG_SOLVED_UPGRADE_REMOVE) {
				continue;
			}
			assert(job->xlink != NULL);
			assert(job->xlink->type == PKG_SOLVED_UPGRADE_INSTALL);
			/* Check if selecting the upgrade remove job would make the corresponding
			 * upgrade install half become available. */
			if (!pkg_jobs_schedule_has_incoming_edge(&available, job->xlink, job) &&
			    !pkg_jobs_schedule_has_incoming_edge(&unavailable, job->xlink, NULL)) {
			    	/* Aaaa this is still not sufficient since if there are multiple upgrade
			    	 * remove jobs to choose from and none of them make the corresponding
			    	 * upgrade install job become available we don't know if there is a specific
			    	 * order which would cause minimal splits. */
			}

		}

		if (!found) {
			/* Add the highest priority job from the set of available jobs
			 * to the sorted list */
			qsort(available.d, available.len, sizeof(available.d[0]),
			    pkg_jobs_schedule_cmp_available);
			struct pkg_solved *job = vec_pop(&available);
			vec_push(&j->jobs, job);
			pkg_jobs_schedule_update_available(&unavailable, &available, job);
		}
	}

	if (unavailable.len > 0) {
		pkg_emit_error("found cycle in job scheduling graph");
		return (EPKG_FATAL);
	}

	vec_free(&unavailable);
	vec_free(&available);

	dbg(3, "finished job scheduling");

	pkg_jobs_schedule_dbg_jobs(&j->jobs);

	return (EPKG_OK);
}
