/*****************************************************************************\
 *  burst_buffer_generic.c - Generic library for managing a burst_buffer
 *****************************************************************************
 *  Copyright (C) 2014 SchedMD LLC.
 *  Written by Morris Jette <jette@schedmd.com>
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#if     HAVE_CONFIG_H
#  include "config.h"
#endif

#define _GNU_SOURCE	/* For POLLRDHUP */
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

#include "slurm/slurm.h"

#include "src/common/list.h"
#include "src/common/pack.h"
#include "src/common/parse_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/slurmctld/slurmctld.h"

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *      <application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "burst_buffer" for SLURM burst_buffer) and <method> is a
 * description of how this plugin satisfies that application.  SLURM will only
 * load a burst_buffer plugin if the plugin_type string has a prefix of
 * "burst_buffer/".
 *
 * plugin_version - an unsigned 32-bit integer giving the version number
 * of the plugin.  If major and minor revisions are desired, the major
 * version number may be multiplied by a suitable magnitude constant such
 * as 100 or 1000.  Various SLURM versions will likely require a certain
 * minimum version for their plugins as this API matures.
 */
const char plugin_name[]        = "burst_buffer generic plugin";
const char plugin_type[]        = "burst_buffer/generic";
const uint32_t plugin_version   = 100;

/* Hash tables are used for both job burst buffer and user limit records */
#define BB_HASH_SIZE		100
typedef struct bb_alloc {
	uint32_t array_job_id;
	uint32_t array_task_id;
	uint32_t job_id;
	char *name;		/* For persistent burst buffers */
	struct bb_alloc *next;
	uint32_t size;
	uint16_t state;
	uint32_t user_id;
} bb_alloc_t;
bb_alloc_t **bb_hash = NULL;	/* Hash by job_id */

typedef struct bb_user {
	struct bb_user *next;
	uint32_t size;
	uint32_t user_id;
} bb_user_t;
bb_user_t **bb_uhash = NULL;	/* Hash by user_id */

static pthread_mutex_t bb_mutex = PTHREAD_MUTEX_INITIALIZER;
static uid_t   *allow_users = NULL;
static char    *allow_users_str = NULL;
static bool	debug_flag = false;
static uid_t   *deny_users = NULL;
static char    *deny_users_str = NULL;
static char    *get_sys_state = NULL;
static uint32_t job_size_limit = NO_VAL;
static uint32_t prio_boost = 0;
static char    *start_stage_in = NULL;
static char    *start_stage_out = NULL;
static char    *stop_stage_in = NULL;
static char    *stop_stage_out = NULL;
static uint32_t total_space = 0;
static uint32_t user_size_limit = NO_VAL;

/* Translate a burst buffer size specification in string form to numeric form,
 * recognizing various sufficies (MB, GB, TB, PB, and Nodes). */
static uint32_t _get_size_num(char *tok)
{
	char *end_ptr = NULL;
	int32_t bb_size_i;
	uint32_t bb_size_u = 0;

	bb_size_i = strtol(tok, &end_ptr, 10);
	if (bb_size_i > 0) {
		bb_size_u = (uint32_t) bb_size_i;
		if ((end_ptr[0] == 'm') || (end_ptr[0] == 'M')) {
			bb_size_u = (bb_size_u + 1023) / 1024;
		} else if ((end_ptr[0] == 'g') || (end_ptr[0] == 'G')) {
			;
		} else if ((end_ptr[0] == 't') || (end_ptr[0] == 'T')) {
			bb_size_u *= 1024;
		} else if ((end_ptr[0] == 'p') || (end_ptr[0] == 'P')) {
			bb_size_u *= (1024 * 1024);
		} else if ((end_ptr[0] == 'n') || (end_ptr[0] == 'N')) {
			bb_size_u |= BB_SIZE_IN_NODES;
		}
	}
	return bb_size_u;
}

/* Return the burst buffer size requested by a job */
static uint32_t _get_bb_size(struct job_record *job_ptr)
{
	char *tok;
	uint32_t bb_size_u = 0;

	if (job_ptr->burst_buffer) {
		tok = strstr(job_ptr->burst_buffer, "size=");
		if (tok)
			bb_size_u = _get_size_num(tok + 5);
	}

	return bb_size_u;
}

/* Allocate a per-job burst buffer record for a specific job.
 * Return a pointer to that record. */
static bb_alloc_t *_alloc_bb_job_rec(struct job_record *job_ptr)
{
	bb_alloc_t *bb_ptr = NULL;
	int i;

	xassert(bb_hash);
	xassert(job_ptr);
	bb_ptr = xmalloc(sizeof(bb_alloc_t));
	bb_ptr->array_job_id = job_ptr->array_job_id;
	bb_ptr->array_task_id = job_ptr->array_task_id;
	bb_ptr->job_id = job_ptr->job_id;
	i = job_ptr->user_id % BB_HASH_SIZE;
	bb_ptr->next = bb_hash[i];
	bb_hash[i] = bb_ptr;
	bb_ptr->size = _get_bb_size(job_ptr);
	bb_ptr->state = BB_STATE_ALLOCATED;
	bb_ptr->user_id = job_ptr->user_id;

	return bb_ptr;
}

/* Allocate a named burst buffer record for a specific user.
 * Return a pointer to that record. */
static bb_alloc_t *_alloc_bb_name_rec(char *name, uint32_t user_id)
{
	bb_alloc_t *bb_ptr = NULL;
	int i;

	xassert(bb_hash);
	bb_ptr = xmalloc(sizeof(bb_alloc_t));
	i = user_id % BB_HASH_SIZE;
	bb_ptr->next = bb_hash[i];
	bb_hash[i] = bb_ptr;
	bb_ptr->name = xstrdup(name);
	bb_ptr->state = BB_STATE_ALLOCATED;
	bb_ptr->user_id = user_id;

	return bb_ptr;
}

/* Find a per-job burst buffer record for a specific job.
 * If not found, return NULL. */
static bb_alloc_t *_find_bb_job_rec(struct job_record *job_ptr)
{
	bb_alloc_t *bb_ptr = NULL;

	xassert(bb_hash);
	xassert(job_ptr);
	bb_ptr = bb_hash[job_ptr->user_id % BB_HASH_SIZE];
	while (bb_ptr) {
		if (bb_ptr->job_id == job_ptr->job_id)
			return bb_ptr;
		bb_ptr = bb_ptr->next;
	}
	return bb_ptr;
}

/* Find a per-job burst buffer record with a specific name.
 * If not found, return NULL. */
static bb_alloc_t * _find_bb_name_rec(char *name, uint32_t user_id)
{
	bb_alloc_t *bb_ptr = NULL;

	xassert(bb_hash);
	bb_ptr = bb_hash[user_id % BB_HASH_SIZE];
	while (bb_ptr) {
		if (!xstrcmp(bb_ptr->name, name))
			return bb_ptr;
		bb_ptr = bb_ptr->next;
	}
	return bb_ptr;
}

/* Purge per-job burst buffer records when the stage-out has completed and
 * the job has been purged from Slurm */
static void _purge_bb_rec(void)
{
	static time_t time_last_purge = 0;
	time_t now = time(NULL);
	bb_alloc_t **bb_pptr, *bb_ptr = NULL;
	int i;

	if (difftime(now, time_last_purge) > 60) {	/* Once per minute */
		for (i = 0; i < BB_HASH_SIZE; i++) {
			bb_pptr = &bb_hash[i];
			bb_ptr = bb_hash[i];
			while (bb_ptr) {
				if ((bb_ptr->job_id != 0) &&
				    (bb_ptr->state >= BB_STATE_STAGED_OUT) &&
				    !find_job_record(bb_ptr->job_id)) {
					*bb_pptr = bb_ptr->next;
					xfree(bb_ptr);
					break;
				}
				bb_pptr = &bb_ptr->next;
				bb_ptr = bb_ptr->next;
			}
		}
	}
}

/* Find user table record for specific user ID, create a record as needed */
bb_user_t *_find_user_rec(uint32_t user_id)
{
	int inx = user_id % BB_HASH_SIZE;
	bb_user_t *user_ptr;

	xassert(bb_uhash);
	user_ptr = bb_uhash[inx];
	while (user_ptr) {
		if (user_ptr->user_id == user_id)
			return user_ptr;
		user_ptr = user_ptr->next;
	}
	user_ptr = xmalloc(sizeof(bb_user_t));
	user_ptr->next = bb_uhash[inx];
	/* user_ptr->size = 0;	initialized by xmalloc */
	user_ptr->user_id = user_id;
	bb_uhash[inx] = user_ptr;
	return user_ptr;
}

/* Add a burst buffer allocation to a user's load */
static void _add_user_load(bb_alloc_t *bb_ptr)
{
	bb_user_t *user_ptr;
	uint32_t tmp_u, tmp_j;

	user_ptr = _find_user_rec(bb_ptr->user_id);
	if ((user_ptr->size & BB_SIZE_IN_NODES) ||
	    (bb_ptr->size   & BB_SIZE_IN_NODES)) {
		tmp_u = user_ptr->size & (~BB_SIZE_IN_NODES);
		tmp_j = bb_ptr->size   & (~BB_SIZE_IN_NODES);
		user_ptr->size = tmp_u + tmp_j;
		user_ptr->size |= BB_SIZE_IN_NODES;
	} else {
		user_ptr->size += bb_ptr->size;
	}
}

/* Remove a burst buffer allocation from a user's load */
static void _remove_user_load(bb_alloc_t *bb_ptr)
{
	bb_user_t *user_ptr;
	uint32_t tmp_u, tmp_j;

	user_ptr = _find_user_rec(bb_ptr->user_id);
	if ((user_ptr->size & BB_SIZE_IN_NODES) ||
	    (bb_ptr->size   & BB_SIZE_IN_NODES)) {
		tmp_u = user_ptr->size & (~BB_SIZE_IN_NODES);
		tmp_j = bb_ptr->size   & (~BB_SIZE_IN_NODES);
		if (tmp_u > tmp_j) {
			user_ptr->size = tmp_u + tmp_j;
			user_ptr->size |= BB_SIZE_IN_NODES;
		} else {
			error("%s: user %u table underflow",
			      __func__, user_ptr->user_id);
			user_ptr->size = BB_SIZE_IN_NODES;
		}
	} else if (user_ptr->size >= bb_ptr->size) {
		user_ptr->size -= bb_ptr->size;
	} else {
		error("%s: user %u table underflow",
		      __func__, user_ptr->user_id);
		user_ptr->size = 0;
	}
}

/* Test if a user's space limit prevents adding
 * RET true if limit reached, false otherwise */
static bool _test_user_limit(uint32_t user_id, uint32_t add_space)
{
	bb_user_t *user_ptr;
	uint32_t tmp_u, tmp_j, lim_u;

	if (user_size_limit == NO_VAL)
		return false;
	user_ptr = _find_user_rec(user_id);
	tmp_u = user_ptr->size  & (~BB_SIZE_IN_NODES);
	tmp_j = add_space       & (~BB_SIZE_IN_NODES);
	lim_u = user_size_limit & (~BB_SIZE_IN_NODES);

	if ((tmp_u + tmp_j) > lim_u)
		return true;
	return false;
}

/* Translate colon delimitted list of users into a UID array,
 * Return value must be xfreed */
static uid_t *_parse_users(char *buf)
{
	char *delim, *tmp, *tok, *save_ptr = NULL;
	int inx = 0, array_size;
	uid_t *user_array = NULL;

	if (!buf)
		return user_array;
	tmp = xstrdup(buf);
	delim = strchr(tmp, ',');
	if (delim)
		delim[0] = '\0';
	array_size = 1;
	user_array = xmalloc(sizeof(uid_t) * array_size);
	tok = strtok_r(tmp, ":", &save_ptr);
	while (tok) {
		if ((uid_from_string(tok, user_array + inx) == -1) ||
		    (user_array[inx] == 0)) {
			error("%s: ignoring invalid user: %s", __func__, tok);
		} else {
			if (++inx >= array_size) {
				array_size *= 2;
				user_array = xrealloc(user_array,
						      sizeof(uid_t)*array_size);
			}
		}
		tok = strtok_r(NULL, ":", &save_ptr);
	}
	xfree(tmp);
	return user_array;
}

/* Translate an array of (zero terminated) UIDs into a string with colon
 * delimited UIDs
 * Return value must be xfreed */
static char *_print_users(uid_t *buf)
{
	char *user_elem, *user_str = NULL;
	int i;

	if (!buf)
		return user_str;
	for (i = 0; buf[i]; i++) {
		user_elem = uid_to_string(buf[i]);
		if (!user_elem)
			continue;
		if (user_str)
			xstrcat(user_str, ":");
		xstrcat(user_str, user_elem);
		xfree(user_elem);
	}
	return user_str;
}

/* Clear configuration parameters, free memory */
static void _clear_config(void)
{
	xfree(allow_users);
	xfree(allow_users_str);
	debug_flag = false;
	xfree(deny_users);
	xfree(deny_users_str);
	xfree(get_sys_state);
	job_size_limit = NO_VAL;
	prio_boost = 0;
	xfree(start_stage_in);
	xfree(start_stage_out);
	xfree(stop_stage_in);
	xfree(stop_stage_out);
	user_size_limit = NO_VAL;
}

/* Load and process BurstBufferParameters configuration parameter */
static void _load_config(void)
{
	s_p_hashtbl_t *bb_hashtbl = NULL;
	char *bb_conf, *tmp = NULL, *value;
	static s_p_options_t bb_options[] = {
		{"AllowUsers", S_P_STRING},
		{"DenyUsers", S_P_STRING},
		{"GetSysState", S_P_STRING},
		{"JobSizeLimit", S_P_STRING},
		{"StagedInPrioBoost", S_P_UINT32},
		{"StartStageIn", S_P_STRING},
		{"StartStageOut", S_P_STRING},
		{"StopStageIn", S_P_STRING},
		{"StopStageOut", S_P_STRING},
		{"UserSizeLimit", S_P_STRING},
		{NULL}
	};

	_clear_config();
	if (slurm_get_debug_flags() & DEBUG_FLAG_BURST_BUF)
		debug_flag = true;

	bb_conf = get_extra_conf_path("burst_buffer.conf");
	bb_hashtbl = s_p_hashtbl_create(bb_options);
	if (s_p_parse_file(bb_hashtbl, NULL, bb_conf, false) == SLURM_ERROR)
		fatal("something wrong with opening/reading %s: %m", bb_conf);
	if (s_p_get_string(&allow_users_str, "AllowUsers", bb_hashtbl))
		allow_users = _parse_users(allow_users_str);
	if (s_p_get_string(&deny_users_str, "DenyUsers", bb_hashtbl))
		deny_users = _parse_users(deny_users_str);
	s_p_get_string(&get_sys_state, "GetSysState", bb_hashtbl);
	if (s_p_get_string(&tmp, "JobSizeLimit", bb_hashtbl)) {
		job_size_limit = _get_size_num(tmp);
		xfree(tmp);
	}
	s_p_get_uint32(&prio_boost, "StagedInPrioBoost", bb_hashtbl);
	s_p_get_string(&start_stage_in, "StartStageIn", bb_hashtbl);
	s_p_get_string(&start_stage_out, "StartStageOut", bb_hashtbl);
	s_p_get_string(&stop_stage_in, "StopStageIn", bb_hashtbl);
	s_p_get_string(&stop_stage_out, "StopStageOut", bb_hashtbl);
	if (s_p_get_string(&tmp, "UserSizeLimit", bb_hashtbl)) {
		user_size_limit = _get_size_num(tmp);
		xfree(tmp);
	}

	s_p_hashtbl_destroy(bb_hashtbl);
	xfree(bb_conf);

	if (debug_flag) {
		value = _print_users(allow_users);
		info("%s: AllowUsers:%s",  __func__, value);
		xfree(value);

		value = _print_users(deny_users);
		info("%s: DenyUsers:%s",  __func__, value);
		xfree(value);

		info("%s: GetSysState:%s",  __func__, get_sys_state);
		info("%s: JobSizeLimit:%u",  __func__, job_size_limit);
		info("%s: StagedInPrioBoost:%u", __func__, prio_boost);
		info("%s: StartStageIn:%s",  __func__, start_stage_in);
		info("%s: StartStageOut:%s",  __func__, start_stage_out);
		info("%s: StopStageIn:%s",  __func__, stop_stage_in);
		info("%s: StopStageOut:%s",  __func__, stop_stage_out);
		info("%s: UserSizeLimit:%u",  __func__, user_size_limit);
	}
}

/* Clear all cached burst buffer records, freeing all memory. */
static void _clear_cache(void)
{
	bb_alloc_t *bb_current, *bb_next;
	bb_user_t  *user_current, *user_next;
	int i;

	if (bb_hash) {
		for (i = 0; i < BB_HASH_SIZE; i++) {
			bb_current = bb_hash[i];
			while (bb_current) {
				bb_next = bb_current->next;
				xfree(bb_current);
				bb_current = bb_next;
			}
		}
		xfree(bb_hash);
	}

	if (bb_uhash) {
		for (i = 0; i < BB_HASH_SIZE; i++) {
			user_current = bb_uhash[i];
			while (user_current) {
				user_next = user_current->next;
				xfree(user_current);
				user_current = user_next;
			}
		}
		xfree(bb_uhash);
	}
}

/* Restore all cached burst buffer records. */
static void _alloc_cache(void)
{
	bb_hash = xmalloc(sizeof(bb_alloc_t *) * BB_HASH_SIZE);
	bb_uhash = xmalloc(sizeof(bb_user_t *) * BB_HASH_SIZE);
}

/* Execute a script, wait for termination and return its stdout.
 * script_type IN - Type of program being run (e.g. "StartStageIn")
 * script_path IN - Fully qualified pathname of the program to execute
 * script_args IN - Arguments to the script
 * max_wait IN - maximum time to wait in seconds, -1 for no limit
 * Return stdout of spawned program, value must be xfreed. */
static char *_run_script(char *script_type, char *script_path,
			 char **script_argv, int max_wait)
{
	int i, status, new_wait, resp_size = 0, resp_offset = 0;
	pid_t cpid;
	char *resp = NULL;
	int pfd[2];

	if ((script_path == NULL) || (script_path[0] == '\0')) {
		error("%s: %s is not configured", plugin_type, script_type);
		return resp;
	}
	if (script_path[0] != '/') {
		error("%s: %s is not fully qualified pathname (%s)",
		      plugin_type, script_type, script_path);
		return resp;
	}
	if (access(script_path, R_OK | X_OK) < 0) {
		error("%s: %s can not be executed (%s)",
		      plugin_type, script_type, script_path);
		return resp;
	}
	if (pipe(pfd) != 0) {
		error("%s: pipe(): %m", plugin_type);
		return resp;
	}
	if ((cpid = fork()) == 0) {
		dup2(pfd[1], STDOUT_FILENO);
		for (i = 0; i < 127; i++) {
			if (i != STDOUT_FILENO)
				close(i);
		}
#ifdef SETPGRP_TWO_ARGS
		setpgrp(0, 0);
#else
		setpgrp();
#endif
		execv(script_path, script_argv);
		error("%s: execv(%s): %m", plugin_type, script_path);
		exit(127);
	} else if (cpid > 0) {
		struct pollfd fds;
		time_t start_time = time(NULL);
		resp_size = 1024;
		resp = xmalloc(resp_size);
		close(pfd[1]);
		while (1) {
			fds.fd = pfd[0];
			fds.events = POLLIN | POLLHUP | POLLRDHUP;
			fds.revents = 0;
			if (max_wait == -1) {
				new_wait = -1;
			} else {
				new_wait = time(NULL) - start_time + max_wait;
				if (new_wait <= 0)
					break;
			}
			status = poll(&fds, 1, new_wait);
			if (status < 1) {
				error("%s: %s timeout",
				      plugin_type, script_type);
				break;
			}
			if ((fds.revents & POLLIN) == 0)
				break;
			i = read(pfd[0], resp + resp_offset,
				 resp_size - resp_offset);
			if (i == 0) {
				break;
			} else if (i < 0) {
				if (errno == EAGAIN)
					continue;
				error("%s: read(%s): %m", plugin_type,
				      script_path);
				break;
			} else {
				resp_offset += i;
				if (resp_offset + 1024 >= resp_size) {
					resp_size *= 2;
					resp = xrealloc(resp, resp_size);
				}
			}
		}
		(void) killpg(cpid, SIGKILL);
		waitpid(cpid, &status, 0);
		close(pfd[0]);
	} else {
		close(pfd[0]);
		close(pfd[1]);
		error("%s: fork(): %m", plugin_type);
	}
	return resp;
}

static int _parse_job_info(void **dest, slurm_parser_enum_t type,
			   const char *key, const char *value,
			   const char *line, char **leftover)
{
	s_p_hashtbl_t *job_tbl;
	char *name = NULL, *tmp = NULL, tmp_name[64];
	uint32_t job_id = 0, size = 0, user_id = 0;
	uint16_t state = 0;
	bb_alloc_t *bb_ptr;
	struct job_record *job_ptr = NULL;
	static s_p_options_t _job_options[] = {
		{"JobID",S_P_STRING},
		{"Name", S_P_STRING},
		{"Size", S_P_STRING},
		{"State", S_P_STRING},
		{NULL}
	};

	*dest = NULL;
	user_id = atoi(value);
	job_tbl = s_p_hashtbl_create(_job_options);
	s_p_parse_line(job_tbl, *leftover, leftover);
	if (s_p_get_string(&tmp, "JobID", job_tbl))
		job_id = atoi(tmp);
	s_p_get_string(&name, "Name", job_tbl);
	if (s_p_get_string(&tmp, "Size", job_tbl))
		size =  _get_size_num(tmp);
	if (s_p_get_string(&tmp, "State", job_tbl))
		state = bb_state_num(tmp);

#if 0
	info("%s: JobID:%u Name:%s Size:%u State:%u UserID:%u",
	     __func__, job_id, name, size, state, user_id);
#endif
	if (job_id) {
		job_ptr = find_job_record(job_id);
		if (!job_ptr) {
			error("%s: Vestigial buffer for job ID %u. "
			      "Clear manually",
			      plugin_type, job_id);
		}
		snprintf(tmp_name, sizeof(tmp_name), "VestigialJob%u", job_id);
		job_id = 0;
		name = tmp_name;
	}
	if (job_ptr) {
		if ((bb_ptr = _find_bb_job_rec(job_ptr)) == NULL) {
			bb_ptr = _alloc_bb_job_rec(job_ptr);
			bb_ptr->state = state;
		}
	} else {
		if ((bb_ptr = _find_bb_name_rec(name, user_id)) == NULL) {
			bb_ptr = _alloc_bb_name_rec(name, user_id);
			bb_ptr->size = size;
			bb_ptr->state = state;
			return SLURM_SUCCESS;
		}
	}

	if (bb_ptr->user_id != user_id) {
		error("%s: User ID mismatch (%u != %u). "
		      "BB UserID=%u JobID=%u Name=%s",
		      plugin_type, bb_ptr->user_id, user_id,
		      bb_ptr->user_id, bb_ptr->job_id, bb_ptr->name);
	}
	if (bb_ptr->size != size) {
		error("%s: Size mismatch (%u != %u). "
		      "BB UserID=%u JobID=%u Name=%s",
		      plugin_type, bb_ptr->size, size,
		      bb_ptr->user_id, bb_ptr->job_id, bb_ptr->name);
		bb_ptr->size = MAX(bb_ptr->size, size);
	}
	if (bb_ptr->state != state) {
		/* State is subject to real-time changes */
		debug("%s: State mismatch (%s != %s). "
		      "BB UserID=%u JobID=%u Name=%s",
		      plugin_type, bb_state_string(bb_ptr->state),
		      bb_state_string(state),
		      bb_ptr->user_id, bb_ptr->job_id, bb_ptr->name);
	}

	return SLURM_SUCCESS;
}

/* Destroy any records created by _parse_job_info(), currently none */
static void _destroy_job_info(void *data)
{
}

/* Determine the current actual burst buffer state.
 * Run the program "get_sys_state" and parse stdout for details. */
static void _load_state(void)
{
	static uint32_t last_total_space = 0;
	char *save_ptr = NULL, *tok, *leftover = NULL, *resp, *tmp = NULL;
	char *script_args[3] = { NULL, "get_sys", NULL };
	s_p_hashtbl_t *state_hashtbl = NULL;
	static s_p_options_t state_options[] = {
		{"ENOENT", S_P_STRING},
		{"UserID", S_P_ARRAY, _parse_job_info, _destroy_job_info},
		{"TotalSize", S_P_STRING},
		{NULL}
	};

	tok = strrchr(get_sys_state, '/');
	if (tok)
		script_args[0] = tok + 1;
	else
		script_args[0] = get_sys_state;
	resp = _run_script("GetSysState", get_sys_state, script_args, 100);
	if (resp == NULL)
		return;
	state_hashtbl = s_p_hashtbl_create(state_options);
	tok = strtok_r(resp, "\n", &save_ptr);
	while (tok) {
		s_p_parse_line(state_hashtbl, tok, &leftover);
		tok = strtok_r(NULL, "\n", &save_ptr);
	}
	if (s_p_get_string(&tmp, "TotalSize", state_hashtbl)) {
		total_space = _get_size_num(tmp);
		xfree(tmp);
	} else {
		error("%s: GetSysState failed to respond with TotalSize",
		      plugin_type);
	}
	s_p_hashtbl_destroy(state_hashtbl);

	if (debug_flag && (total_space != last_total_space))
		info("%s: total_space:%u",  __func__, total_space);
	last_total_space = total_space;
}

/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init(void)
{
	pthread_mutex_lock(&bb_mutex);
	_load_config();
	if (debug_flag)
		info("%s: %s",  __func__, plugin_type);
	_alloc_cache();
	pthread_mutex_unlock(&bb_mutex);

	return SLURM_SUCCESS;
}

/*
 * fini() is called when the plugin is unloaded. Free all memory.
 */
extern int fini(void)
{
	pthread_mutex_lock(&bb_mutex);
	if (debug_flag)
		info("%s: %s",  __func__, plugin_type);
	_clear_config();
	_clear_cache();
	pthread_mutex_unlock(&bb_mutex);

	return SLURM_SUCCESS;
}

/*
 * Load the current burst buffer state (e.g. how much space is available now).
 * Run at the beginning of each scheduling cycle in order to recognize external
 * changes to the burst buffer state (e.g. capacity is added, removed, fails,
 * etc.)
 *
 * init_config IN - true if called as part of slurmctld initialization
 * Returns a SLURM errno.
 */
extern int bb_p_load_state(bool init_config)
{
	pthread_mutex_lock(&bb_mutex);
	if (debug_flag)
		info("%s: %s",  __func__, plugin_type);
	_load_state();
	_purge_bb_rec();
	pthread_mutex_unlock(&bb_mutex);

	return SLURM_SUCCESS;
}

/*
 * Note configuration may have changed. Handle changes in BurstBufferParameters.
 *
 * Returns a SLURM errno.
 */
extern int bb_p_reconfig(void)
{
	pthread_mutex_lock(&bb_mutex);
	if (debug_flag)
		info("%s: %s",  __func__, plugin_type);
	_load_config();
	pthread_mutex_unlock(&bb_mutex);

	return SLURM_SUCCESS;
}

/*
 * Pack current burst buffer state information for network transmission to
 * user (e.g. "scontrol show burst")
 *
 * Returns a SLURM errno.
 */
extern int bb_p_state_pack(Buf buffer, uint16_t protocol_version)
{
	struct bb_alloc *bb_next;
	uint32_t rec_count = 0;
	int i, eof, offset;

	pthread_mutex_lock(&bb_mutex);
	if (debug_flag)
		info("%s: %s",  __func__, plugin_type);
	packstr((char *)plugin_type, buffer);	/* Remove "const" qualifier */
	offset = get_buf_offset(buffer);
	pack32(rec_count,        buffer);
	packstr(allow_users_str, buffer);
	packstr(deny_users_str,  buffer);
	packstr(get_sys_state,   buffer);
	packstr(start_stage_in,  buffer);
	packstr(start_stage_out, buffer);
	packstr(stop_stage_in,   buffer);
	packstr(stop_stage_out,  buffer);
	pack32(job_size_limit,   buffer);
	pack32(prio_boost,       buffer);
	pack32(total_space,      buffer);
	pack32(user_size_limit,  buffer);
	if (bb_hash) {
		for (i = 0; i < BB_HASH_SIZE; i++) {
			bb_next = bb_hash[i];
			while (bb_next) {
				pack32(bb_next->array_job_id,  buffer);
				pack32(bb_next->array_task_id, buffer);
				pack32(bb_next->job_id,        buffer);
				packstr(bb_next->name,         buffer);
				pack32(bb_next->size,          buffer);
				pack16(bb_next->state,         buffer);
				pack32(bb_next->user_id,       buffer);
				rec_count++;
				bb_next = bb_next->next;
			}
		}
		if (rec_count != 0) {
			eof = get_buf_offset(buffer);
			set_buf_offset(buffer, offset);
			pack32(rec_count, buffer);
			set_buf_offset(buffer, eof);
		}
	}
	info("%s: record_count:%u",  __func__, rec_count);
	pthread_mutex_unlock(&bb_mutex);

	return SLURM_SUCCESS;
}

/*
 * Validate a job submit request with respect to burst buffer options.
 *
 * Returns a SLURM errno.
 */
extern int bb_p_job_validate(struct job_descriptor *job_desc,
			     uid_t submit_uid)
{
	int32_t bb_size = 0;
	char *key;
	int i;

	if (debug_flag) {
		info("%s: %s",  __func__, plugin_type);
		info("%s: job_user_id:%u, submit_uid:%d", __func__,
		     job_desc->user_id, submit_uid);
		info("%s: burst_buffer:%s", __func__, job_desc->burst_buffer);
		info("%s: script:%s", __func__, job_desc->script);
	}

	if (job_desc->burst_buffer) {
		key = strstr(job_desc->burst_buffer, "size=");
		if (key)
			bb_size = _get_size_num(key + 5);
	}
	if (bb_size == 0)
		return SLURM_SUCCESS;
	if (bb_size < 0)
		return ESLURM_BURST_BUFFER_LIMIT;

	pthread_mutex_lock(&bb_mutex);
	if (((job_size_limit  != NO_VAL) && (bb_size > job_size_limit)) ||
	    ((user_size_limit != NO_VAL) && (bb_size > user_size_limit))) {
		pthread_mutex_unlock(&bb_mutex);
		return ESLURM_BURST_BUFFER_LIMIT;
	}

	if (allow_users) {
		for (i = 0; allow_users[i]; i++) {
			if (job_desc->user_id == allow_users[i])
				break;
		}
		if (allow_users[i] == 0) {
			pthread_mutex_unlock(&bb_mutex);
			return ESLURM_BURST_BUFFER_PERMISSION;
		}
	}

	if (deny_users) {
		for (i = 0; deny_users[i]; i++) {
			if (job_desc->user_id == deny_users[i])
				break;
		}
		if (deny_users[i] != 0) {
			pthread_mutex_unlock(&bb_mutex);
			return ESLURM_BURST_BUFFER_PERMISSION;
		}
	}

	if (bb_size > total_space) {
		info("Job from user %u requested burst buffer size of %u, "
		     "but total space is only %u",
		     job_desc->user_id, bb_size, total_space);
	}
	pthread_mutex_unlock(&bb_mutex);

	return SLURM_SUCCESS;
}

/*
 * Validate a job submit request with respect to burst buffer options.
 *
 * Returns a SLURM errno.
 */
extern int bb_p_job_try_stage_in(List job_queue)
{
	ListIterator job_iter;
	struct job_record *job_ptr;
	bb_alloc_t *bb_ptr;
	uint32_t bb_size;

	if (debug_flag)
		info("%s: %s",  __func__, plugin_type);
	pthread_mutex_lock(&bb_mutex);
	job_iter = list_iterator_create(job_queue);
	while ((job_ptr = list_next(job_iter))) {
		if ((job_ptr->burst_buffer == NULL) ||
		    (job_ptr->burst_buffer[0] == '\0'))
			continue;
		bb_size = _get_bb_size(job_ptr);
		if (bb_size == 0)
			continue;
		if (_test_user_limit(job_ptr->user_id, bb_size))
			continue;
		if (_find_bb_job_rec(job_ptr))
			continue;
		bb_ptr = _alloc_bb_job_rec(job_ptr);
		bb_ptr->state = BB_STATE_ALLOCATED;
		_add_user_load(bb_ptr);
		if (debug_flag) {
			info("%s: start stage-in job_id:%u",
			     __func__, job_ptr->job_id);
		}
	}
	list_iterator_destroy(job_iter);
	pthread_mutex_unlock(&bb_mutex);

	return SLURM_SUCCESS;
}

/*
 * Determine if a job's burst buffer stage-in is complete
 *
 * RET: 0 - stage-in is underway
 *      1 - stage-in complete
 *     -1 - fatal error
 */
extern int bb_p_job_test_stage_in(struct job_record *job_ptr)
{
	bb_alloc_t *bb_ptr;

	if (debug_flag) {
		info("%s: %s",  __func__, plugin_type);
		info("%s: job_id:%u", __func__, job_ptr->job_id);
	}
	if ((job_ptr->burst_buffer == NULL) ||
	    (job_ptr->burst_buffer[0] == '\0') ||
	    (_get_bb_size(job_ptr) == 0))
		return 1;

	pthread_mutex_lock(&bb_mutex);
	bb_ptr = _find_bb_job_rec(job_ptr);
	if (!bb_ptr) {
		debug("%s: job_id:%u bb_rec not found",
		      __func__, job_ptr->job_id);
		pthread_mutex_unlock(&bb_mutex);
		return -1;
	}
	if (bb_ptr->state < BB_STATE_STAGED_IN) {
		bb_ptr->state++;
		pthread_mutex_unlock(&bb_mutex);
		return 0;
	} else if (bb_ptr->state == BB_STATE_STAGED_IN) {
		pthread_mutex_unlock(&bb_mutex);
		return 1;
	}

	error("%s: job_id:%u bb_state:%u",
	      __func__, job_ptr->job_id, bb_ptr->state);
	pthread_mutex_unlock(&bb_mutex);
	return -1;
}

/*
 * Trigger a job's burst buffer stage-out to begin
 *
 * Returns a SLURM errno.
 */
extern int bb_p_job_start_stage_out(struct job_record *job_ptr)
{
	bb_alloc_t *bb_ptr;

	if (debug_flag) {
		info("%s: %s",  __func__, plugin_type);
		info("%s: job_id:%u", __func__, job_ptr->job_id);
	}
	if ((job_ptr->burst_buffer == NULL) ||
	    (job_ptr->burst_buffer[0] == '\0') ||
	    (_get_bb_size(job_ptr) == 0))
		return SLURM_SUCCESS;

	pthread_mutex_lock(&bb_mutex);
	bb_ptr = _find_bb_job_rec(job_ptr);
	if (!bb_ptr) {
		error("%s: job_id:%u bb_rec not found",
		      __func__, job_ptr->job_id);
		pthread_mutex_unlock(&bb_mutex);
		return SLURM_ERROR;
	}
	bb_ptr->state = BB_STATE_STAGING_OUT;
	pthread_mutex_unlock(&bb_mutex);

	return SLURM_SUCCESS;
}

/*
 * Determine if a job's burst buffer stage-out is complete
 *
 * RET: 0 - stage-out is underway
 *      1 - stage-out complete
 *     -1 - fatal error
 */
extern int bb_p_job_test_stage_out(struct job_record *job_ptr)
{
	bb_alloc_t *bb_ptr;

	if (debug_flag) {
		info("%s: %s",  __func__, plugin_type);
		info("%s: job_id:%u", __func__, job_ptr->job_id);
	}
	if ((job_ptr->burst_buffer == NULL) ||
	    (job_ptr->burst_buffer[0] == '\0') ||
	    (_get_bb_size(job_ptr) == 0))
		return 1;

	pthread_mutex_lock(&bb_mutex);
	bb_ptr = _find_bb_job_rec(job_ptr);
	if (!bb_ptr) {
		error("%s: job_id:%u bb_rec not found",
		      __func__, job_ptr->job_id);
		pthread_mutex_unlock(&bb_mutex);
		return -1;
	} else if (bb_ptr->state == BB_STATE_STAGING_OUT) {
		if (++bb_ptr->state == BB_STATE_STAGED_OUT)
			_remove_user_load(bb_ptr);
		pthread_mutex_unlock(&bb_mutex);
		return 0;
	} else if (bb_ptr->state == BB_STATE_STAGED_OUT) {
		pthread_mutex_unlock(&bb_mutex);
		return 1;
	}
	error("%s: job_id:%u bb_state:%u",
	      __func__, job_ptr->job_id, bb_ptr->state);
	pthread_mutex_unlock(&bb_mutex);

	return -1;
}
