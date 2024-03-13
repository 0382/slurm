/*****************************************************************************\
 *  parsers.c - Slurm data parsers
 *****************************************************************************
 *  Copyright (C) SchedMD LLC.
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
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
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>

#include "slurm/slurm.h"

#include "src/common/cpu_frequency.h"
#include "src/common/data.h"
#include "src/common/log.h"
#include "src/common/net.h"
#include "src/common/parse_time.h"
#include "src/common/proc_args.h"
#include "src/common/read_config.h"
#include "src/common/ref.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/slurmdbd_defs.h"
#include "src/common/uid.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/interfaces/data_parser.h"
#include "src/interfaces/select.h"

#include "alloc.h"
#include "api.h"
#include "events.h"
#include "openapi.h"
#include "parsers.h"
#include "parsing.h"
#include "slurmdb_helpers.h"

#include "src/sinfo/sinfo.h" /* provides sinfo_data_t */

#define CPU_FREQ_FLAGS_BUF_SIZE 64

#define MAGIC_FOREACH_CSV_STRING 0x889bbe2a
#define MAGIC_FOREACH_CSV_STRING_LIST 0x8391be0b
#define MAGIC_FOREACH_LIST 0xaefa2af3
#define MAGIC_FOREACH_LIST_FLAG 0xa1d4acd2
#define MAGIC_FOREACH_POPULATE_GLOBAL_TRES_LIST 0x31b8aad2
#define MAGIC_FOREACH_STEP 0x7e2eaef1
#define MAGIC_FOREACH_STRING_ID 0x2ea1be2b
#define MAGIC_FOREACH_STRING_ARRAY 0xaea1be2b
#define MAGIC_FOREACH_HOSTLIST 0xae71b92b
#define MAGIC_LIST_PER_TRES_TYPE_NCT 0xb1d8acd2
#define MAGIC_FOREACH_DUMP_ASSOC_SHARES 0xaccc222b

#define PARSER_ARRAY(type) _v41_parser_array_##type
#define PARSER_FLAG_ARRAY(type) _v41_parser_flag_array_##type
#define PARSE_FUNC(type) _v41_parse_##type
#define DUMP_FUNC(type) _v41_dump_##type
#define NEW_FUNC(type) _v41_openapi_new_##type
#define FREE_FUNC(type) _v41_openapi_free_##type
#define PARSE_DISABLED(type)                                                 \
	static int PARSE_FUNC(type)(const parser_t *const parser, void *src, \
				    data_t *dst, args_t *args,               \
				    data_t *parent_path)                     \
	{                                                                    \
		return PARSE_FUNC(disabled)(parser, src, dst, args,          \
					    parent_path);                    \
	}
#define DUMP_DISABLED(type)                                                  \
	static int DUMP_FUNC(type)(const parser_t *const parser, void *src,  \
				    data_t *dst, args_t *args)               \
	{                                                                    \
		return DUMP_FUNC(disabled)(parser, src, dst, args);	     \
	}

#define parse_error(parser, args, parent_path, error, fmt, ...)    \
	_parse_error_funcname(parser, args, parent_path, __func__, \
			      XSTRINGIFY(__LINE__), error, fmt, ##__VA_ARGS__)

static int PARSE_FUNC(disabled)(const parser_t *const parser, void *src,
				data_t *dst, args_t *args, data_t *parent_path)
{
	char *path = NULL;

	on_warn(PARSING, parser->type, args,
		set_source_path(&path, args, parent_path), __func__,
		"data_parser/v0.0.41 does not support parser %u for parsing. Output may be incomplete.",
		parser->type);

	xfree(path);
	return SLURM_SUCCESS;
}

static int DUMP_FUNC(disabled)(const parser_t *const parser, void *src,
			       data_t *dst, args_t *args)
{
	on_warn(DUMPING, parser->type, args, NULL, __func__,
		"data_parser/v0.0.41 does not support parser %u for dumping. Output may be incomplete.",
		parser->type);

	return SLURM_SUCCESS;
}

static int _parse_error_funcname(const parser_t *const parser, args_t *args,
				 data_t *parent_path, const char *funcname,
				 const char *line, int error, const char *fmt,
				 ...)
{
	char *path = NULL;
	va_list ap;
	char *str;
	char caller[128];

	snprintf(caller, sizeof(caller), "%s:%s", funcname, line);

	va_start(ap, fmt);
	str = vxstrfmt(fmt, ap);
	va_end(ap);

	(void) set_source_path(&path, args, parent_path);

	on_error(PARSING, parser->type, args, error, path, caller, "%s", str);

	xfree(path);
	xfree(str);
	return error;
}

/* based on slurmdb_tres_rec_t but includes node and task */
typedef struct {
	uint64_t count;
	char *node;
	uint64_t task;
	uint32_t id;
	char *name;
	char *type;
} slurmdb_tres_nct_rec_t;

typedef enum {
	TRES_EXPLODE_COUNT = 1,
	TRES_EXPLODE_NODE,
	TRES_EXPLODE_TASK,
} tres_explode_type_t;

typedef struct {
	int magic; /* MAGIC_LIST_PER_TRES_TYPE_NCT */
	tres_explode_type_t type;
	slurmdb_tres_nct_rec_t *tres_nct;
	int tres_nct_count;
	hostlist_t *host_list;
	args_t *args;
	const parser_t *const parser;
} foreach_list_per_tres_type_nct_t;

typedef struct {
	int magic; /* MAGIC_FOREACH_POPULATE_GLOBAL_TRES_LIST */
	slurmdb_tres_nct_rec_t *tres_nct;
	int tres_nct_count;
	int offset;
} foreach_populate_g_tres_list;

typedef struct {
	int magic; /* MAGIC_FOREACH_STRING_ID */
	const parser_t *const parser;
	data_t *ddst;
	data_t *parent_path;
	const char *caller;
	ssize_t index;
	List qos_list;
	args_t *args;
} foreach_qos_string_id_t;

typedef struct {
	int magic; /* MAGIC_FOREACH_STRING_ARRAY */
	char **array;
	int i;
	const parser_t *const parser;
	args_t *args;
	data_t *parent_path;
} foreach_string_array_t;

typedef struct {
	int magic; /* MAGIC_FOREACH_HOSTLIST */
	const parser_t *const parser;
	args_t *args;
	hostlist_t *host_list;
	data_t *parent_path;
} foreach_hostlist_parse_t;

typedef struct {
	int magic; /* MAGIC_FOREACH_CSV_STRING */
	int rc;
	char *dst;
	char *pos;
	const parser_t *const parser;
	args_t *args;
	data_t *parent_path;
} parse_foreach_CSV_STRING_t;

typedef struct {
	int magic; /* MAGIC_FOREACH_CSV_STRING_LIST */
	list_t *list;
	data_t *dst_list;
	const parser_t *const parser;
	args_t *args;
	data_t *parent_path;
} parse_foreach_CSV_STRING_LIST_t;

typedef enum {
	PROC_EXIT_CODE_INVALID = 0,
	PROC_EXIT_CODE_PENDING,
	PROC_EXIT_CODE_SUCCESS,
	PROC_EXIT_CODE_ERROR,
	PROC_EXIT_CODE_SIGNALED,
	PROC_EXIT_CODE_CORE_DUMPED,
	PROC_EXIT_CODE_INVALID_MAX
} proc_exit_code_status_t;

typedef struct {
	proc_exit_code_status_t status;
	uint32_t return_code;
	uint16_t signal;
} proc_exit_code_verbose_t;

typedef struct {
	char *name;
	long double value;
} SHARES_FLOAT128_TRES_t;

typedef struct {
	char *name;
	uint64_t value;
} SHARES_UINT64_TRES_t;

typedef struct {
	/*
	 * Special wrapper since assoc_shares_object_t references objects
	 * outside of its own structure.
	 */
	assoc_shares_object_t obj;
	uint64_t tot_shares;
	uint32_t tres_cnt;
	char **tres_names;
} assoc_shares_object_wrap_t;

typedef struct {
	int magic; /* MAGIC_FOREACH_DUMP_ASSOC_SHARES */
	int rc;
	args_t *args;
	assoc_shares_object_wrap_t wrap;
	data_t *dst;
	uint64_t tot_shares;
	uint32_t tres_cnt;
	char **tres_names;
} foreach_dump_ASSOC_SHARES_OBJ_LIST_t;

typedef struct {
	/*
	 * job_array_resp_msg_t is multiple arrays of values for each entry
	 * instead of an array of structs for each entry which doesn't work with
	 * parser arrays cleanly.
	 */
	slurm_selected_step_t step;
	int rc;
	char *msg;
} JOB_ARRAY_RESPONSE_MSG_entry_t;

typedef enum {
	WCKEY_TAG_FLAGS_ASSIGNED_DEFAULT = SLURM_BIT(0),
} WCKEY_TAG_FLAGS_t;

typedef struct {
	const char *wckey;
	WCKEY_TAG_FLAGS_t flags;
} WCKEY_TAG_STRUCT_t;

typedef struct {
	uint32_t end_job_queue;
	uint32_t bf_max_job_start;
	uint32_t bf_max_job_test;
	uint32_t bf_max_time;
	uint32_t bf_node_space_size;
	uint32_t state_changed;
} bf_exit_fields_t;

static const struct {
	bf_exit_t field;
	size_t offset;
} bf_exit_map[] = {
	{ BF_EXIT_END, offsetof(bf_exit_fields_t, end_job_queue) },
	{ BF_EXIT_MAX_JOB_START, offsetof(bf_exit_fields_t, bf_max_job_start) },
	{ BF_EXIT_MAX_JOB_TEST, offsetof(bf_exit_fields_t, bf_max_job_test) },
	{ BF_EXIT_STATE_CHANGED, offsetof(bf_exit_fields_t, state_changed) },
	{ BF_EXIT_TABLE_LIMIT, offsetof(bf_exit_fields_t, bf_node_space_size) },
	{ BF_EXIT_TIMEOUT, offsetof(bf_exit_fields_t, bf_max_time) }
};

typedef struct {
	uint32_t end_job_queue;
	uint32_t default_queue_depth;
	uint32_t max_job_start;
	uint32_t max_rpc_cnt;
	uint32_t max_sched_time;
	uint32_t licenses;
} schedule_exit_fields_t;

static const struct {
	schedule_exit_t field;
	size_t offset;
} schedule_exit_map[] = {
	{ SCHEDULE_EXIT_END, offsetof(schedule_exit_fields_t, end_job_queue) },
	{ SCHEDULE_EXIT_MAX_DEPTH, offsetof(schedule_exit_fields_t,
					    default_queue_depth) },
	{ SCHEDULE_EXIT_MAX_JOB_START, offsetof(schedule_exit_fields_t,
						max_job_start) },
	{ SCHEDULE_EXIT_LIC, offsetof(schedule_exit_fields_t, licenses) },
	{ SCHEDULE_EXIT_RPC_CNT, offsetof(schedule_exit_fields_t,
					  max_rpc_cnt) },
	{ SCHEDULE_EXIT_TIMEOUT, offsetof(schedule_exit_fields_t,
					  max_sched_time) }
};

typedef struct {
	bool set;
	bool infinite;
	double number;
} FLOAT64_NO_VAL_t;

typedef struct {
	bool set;
	bool infinite;
	uint64_t number;
} UINT64_NO_VAL_t;

typedef struct {
	bool set;
	bool infinite;
	uint32_t number;
} UINT32_NO_VAL_t;

typedef struct {
	bool set;
	bool infinite;
	uint16_t number;
} UINT16_NO_VAL_t;

typedef struct {
	bool set;
	bool infinite;
	int64_t number;
} INT64_NO_VAL_t;

typedef enum {
	JOB_RES_CORE_INVALID = 0,
	JOB_RES_CORE_UNALLOC = NO_VAL64,
	JOB_RES_CORE_ALLOC = SLURM_BIT(1),
	JOB_RES_CORE_IN_USE = SLURM_BIT(2),
} JOB_RES_CORE_status_t;

typedef struct {
	uint32_t index;
	JOB_RES_CORE_status_t status;
} JOB_RES_CORE_t;

typedef struct {
	uint32_t index;
	JOB_RES_CORE_t *cores;
} JOB_RES_SOCKET_t;

typedef struct {
	uint32_t index;
	const char *name;
	struct {
		uint16_t count;
		uint16_t used;
	} cpus;
	struct {
		uint64_t used;
		uint64_t allocated;
	} memory;
	JOB_RES_SOCKET_t *sockets;
} JOB_RES_NODE_t;

typedef struct {
	uint16_t id;
	uint32_t count;
	uint64_t time;
	uint64_t average_time;
} STATS_MSG_RPC_TYPE_t;

typedef struct {
	uint32_t id;
	uint32_t count;
	uint64_t time;
	uint64_t average_time;
} STATS_MSG_RPC_USER_t;

typedef struct {
	uint16_t id;
	uint32_t count;
} STATS_MSG_RPC_QUEUE_t;

typedef struct {
	uint16_t id;
	const char *hostlist;
} STATS_MSG_RPC_DUMP_t;

static int PARSE_FUNC(UINT64_NO_VAL)(const parser_t *const parser, void *obj,
				     data_t *str, args_t *args,
				     data_t *parent_path);
static int PARSE_FUNC(UINT64)(const parser_t *const parser, void *obj,
			      data_t *str, args_t *args, data_t *parent_path);
static int DUMP_FUNC(UINT64_NO_VAL)(const parser_t *const parser, void *obj,
				    data_t *dst, args_t *args);
static int PARSE_FUNC(INT64_NO_VAL)(const parser_t *const parser, void *obj,
				    data_t *str, args_t *args,
				    data_t *parent_path);
static int PARSE_FUNC(FLOAT64_NO_VAL)(const parser_t *const parser, void *obj,
				      data_t *str, args_t *args,
				      data_t *parent_path);
static int PARSE_FUNC(STRING)(const parser_t *const parser, void *obj,
			      data_t *str, args_t *args, data_t *parent_path);
static int DUMP_FUNC(STRING)(const parser_t *const parser, void *obj,
			     data_t *data, args_t *args);

#ifndef NDEBUG
static void _check_flag_bit(int8_t i, const flag_bit_t *bit, bool *found_bit,
			    ssize_t parser_size)
{
	xassert(bit->magic == MAGIC_FLAG_BIT);
	xassert(bit->type > FLAG_BIT_TYPE_INVALID);
	xassert(bit->type < FLAG_BIT_TYPE_MAX);
	xassert(bit->name && bit->name[0]);
	/* mask must be set */
	xassert(bit->mask);
	xassert(bit->flag_size <= sizeof(bit->value));
	xassert(bit->flag_size > 0);
	xassert(bit->flag_name && bit->flag_name[0]);
	xassert(bit->mask_size <= sizeof(bit->value));
	xassert(bit->mask_size > 0);
	xassert(bit->mask_name && bit->mask_name[0]);

	/* Bit values must fit in parser->size bits */
	switch (parser_size) {
	case sizeof(uint8_t):
		xassert((bit->value & UINT8_MAX) == bit->value);
		break;
	case sizeof(uint16_t):
		xassert((bit->value & UINT16_MAX) == bit->value);
		break;
	case sizeof(uint32_t):
		xassert((bit->value & UINT32_MAX) == bit->value);
		break;
	case sizeof(uint64_t):
		xassert((bit->value & UINT64_MAX) == bit->value);
		break;
	default:
		error("Parser->size (%ld) is invalid. This should never happen.",
		      parser_size);
		xassert(false);
	}

	if (bit->type == FLAG_BIT_TYPE_BIT) {
		/* atleast one bit must be set */
		xassert(bit->value);
		/* mask must include all value bits */
		xassert((bit->mask & bit->value) == bit->value);
		*found_bit = true;
	} else if (bit->type == FLAG_BIT_TYPE_EQUAL) {
		/*
		 * bit->mask must include all value bits
		 * (if there are any)
		 */
		xassert(!bit->value ||
			((bit->mask & bit->value) == bit->value));
		/*
		 * All equal type flags should come before any bit
		 * type flags to avoid issues with masks overlapping
		 */
		xassert(!*found_bit);
	}
}

extern void check_parser_funcname(const parser_t *const parser,
				  const char *func_name)
{
	xassert(parser->magic == MAGIC_PARSER);

	xassert(parser->model > PARSER_MODEL_INVALID);
	xassert(parser->model < PARSER_MODEL_MAX);
	xassert(parser->obj_type_string && parser->obj_type_string[0]);

	if (parser->model == PARSER_MODEL_ARRAY_REMOVED_FIELD) {
		xassert(!parser->size);
		xassert(!parser->field_name);
		xassert(parser->ptr_offset == NO_VAL);
		xassert(parser->key && parser->key[0]);
		xassert(parser->deprecated);
		xassert(!parser->flag_bit_array_count);
		xassert(parser->type_string && parser->type_string[0]);
		xassert(parser->list_type == DATA_PARSER_TYPE_INVALID);
		xassert(!parser->fields);
		xassert(!parser->field_count);
		xassert(!parser->parse);
		xassert(!parser->dump);
		xassert(!parser->pointer_type);
		xassert(!parser->array_type);
		xassert(parser->obj_openapi == OPENAPI_FORMAT_INVALID);
		return;
	}

	xassert(parser->size > 0);

	if (parser->model == PARSER_MODEL_ARRAY_SKIP_FIELD) {
		/* field is only a place holder so most assert()s dont apply */
		xassert(parser->field_name && parser->field_name[0]);
		xassert(parser->type == DATA_PARSER_TYPE_INVALID);
		xassert(!parser->flag_bit_array_count);
		xassert(parser->needs == NEED_NONE);
		xassert(!parser->field_name_overloads);
		xassert(!parser->key);
		xassert(!parser->type_string);
		xassert(!parser->required);
		xassert((parser->ptr_offset < NO_VAL) ||
			(parser->ptr_offset >= 0));
		return;
	}

	xassert(parser->type > DATA_PARSER_TYPE_INVALID);
	xassert(parser->type < DATA_PARSER_TYPE_MAX);
	xassert(parser->type_string && parser->type_string[0]);

	if (parser->model == PARSER_MODEL_FLAG_ARRAY) {
		bool found_bit_type = false;

		/* parser of a specific flag field list */
		xassert(parser->flag_bit_array);
		xassert(parser->flag_bit_array_count < NO_VAL8);

		for (int8_t i = 0; i < parser->flag_bit_array_count; i++) {
			_check_flag_bit(i, &parser->flag_bit_array[i],
					&found_bit_type, parser->size);

			/* check for duplicate flag names */
			for (int8_t j = 0; j < parser->flag_bit_array_count;
			     j++) {
				xassert((i == j) ||
					xstrcasecmp(parser->flag_bit_array[i]
							    .name,
						    parser->flag_bit_array[j]
							    .name));
			}
		}

		/* make sure this is not a list or array type */
		xassert(parser->list_type == DATA_PARSER_TYPE_INVALID);
		xassert(!parser->fields);
		xassert(!parser->field_count);
		xassert(!parser->parse);
		xassert(!parser->dump);
		xassert(parser->ptr_offset == NO_VAL);
		xassert(!parser->pointer_type);
		xassert(!parser->array_type);
		xassert(!parser->obj_openapi);
	} else if (parser->model == PARSER_MODEL_LIST) {
		/* parser of a List */
		xassert(parser->list_type > DATA_PARSER_TYPE_INVALID);
		xassert(parser->list_type < DATA_PARSER_TYPE_MAX);
		xassert(!parser->flag_bit_array_count);
		xassert(!parser->fields);
		xassert(!parser->field_count);
		xassert(!parser->parse);
		xassert(!parser->dump);
		xassert(parser->size == sizeof(list_t *));
		xassert(parser->ptr_offset == NO_VAL);
		xassert(!parser->pointer_type);
		xassert(!parser->array_type);
		xassert(!parser->obj_openapi);
	} else if (parser->model == PARSER_MODEL_ARRAY) {
		/* parser of a parser Array */
		xassert(parser->field_count > 0);

		xassert(parser->ptr_offset == NO_VAL);
		xassert(!parser->flag_bit_array_count);
		xassert(!parser->parse);
		xassert(!parser->dump);
		xassert(parser->list_type == DATA_PARSER_TYPE_INVALID);
		xassert(parser->fields);
		xassert(!parser->pointer_type);
		xassert(!parser->array_type);
		xassert(!parser->obj_openapi);

		for (int i = 0; i < parser->field_count; i++) {
			/* recursively check the child parsers */
			check_parser(&parser->fields[i]);

			/*
			 * Verify each field_name is unique while ignoring
			 * complex parsers.
			 */
			if (parser->fields[i].field_name) {
				int matches = 0;

				for (int j = 0; j < parser->field_count; j++) {
					if (i == j)
						continue;

					if (!xstrcasecmp(
						parser->fields[i].field_name,
						parser->fields[j].field_name))
						matches++;
				}

				xassert(matches ==
					parser->fields[i].field_name_overloads);
			}

			/*
			 * Verify each key path is unique while ignoring skipped
			 * parsers
			 */
			if (parser->fields[i].key)
				for (int j = 0; j < parser->field_count; j++)
					xassert((i == j) ||
						xstrcasecmp(parser->fields[i]
								    .key,
							    parser->fields[j]
								    .key));
		}
	} else if ((parser->model == PARSER_MODEL_ARRAY_LINKED_FIELD) ||
		   (parser->model ==
		    PARSER_MODEL_ARRAY_LINKED_EXPLODED_FLAG_ARRAY_FIELD)) {
		/* parser array link to a another parser */
		const parser_t *const linked =
			find_parser_by_type(parser->type);

		if (parser->model !=
		    PARSER_MODEL_ARRAY_LINKED_EXPLODED_FLAG_ARRAY_FIELD) {
			xassert(parser->key && parser->key[0]);
		}

		xassert(!parser->flag_bit_array_count);
		xassert(!parser->fields);
		xassert(!parser->field_count);
		xassert(!parser->parse);
		xassert(!parser->dump);
		xassert(parser->list_type == DATA_PARSER_TYPE_INVALID);
		xassert(!parser->pointer_type);
		xassert(!parser->array_type);
		xassert(!parser->obj_openapi);

		switch (linked->model) {
		case PARSER_MODEL_SIMPLE:
			xassert(parser->field_name && parser->field_name[0]);
			/* fall through */
		case PARSER_MODEL_ARRAY:
		case PARSER_MODEL_FLAG_ARRAY:
		case PARSER_MODEL_LIST:
		case PARSER_MODEL_PTR:
		case PARSER_MODEL_NT_ARRAY:
		case PARSER_MODEL_NT_PTR_ARRAY:
			/* linked parsers must always be the same size */
			xassert((parser->size == NO_VAL) ||
				(parser->size == linked->size));
			xassert((parser->ptr_offset < NO_VAL) ||
				(parser->ptr_offset >= 0));
			break;
		case PARSER_MODEL_COMPLEX:
			xassert(!parser->field_name);
			/*
			 * complex uses the size of the struct which we don't
			 * know here
			 */
			xassert(parser->size > 0);
			xassert(parser->size <= NO_VAL);
			xassert(parser->ptr_offset == NO_VAL);
			break;
		case PARSER_MODEL_ARRAY_LINKED_FIELD:
		case PARSER_MODEL_ARRAY_LINKED_EXPLODED_FLAG_ARRAY_FIELD:
			fatal_abort("linked parsers must not link to other linked parsers");
		case PARSER_MODEL_ARRAY_SKIP_FIELD:
			fatal_abort("linked parsers must not link to a skip parsers");
		case PARSER_MODEL_ARRAY_REMOVED_FIELD:
			fatal_abort("linked parsers must not link to a removed parser");
		case PARSER_MODEL_INVALID:
		case PARSER_MODEL_MAX:
			fatal_abort("invalid model");
		}
	} else if ((parser->model == PARSER_MODEL_SIMPLE) ||
		   (parser->model == PARSER_MODEL_COMPLEX)) {
		xassert(parser->ptr_offset == NO_VAL);
		xassert(!parser->key);
		xassert(!parser->field_name);
		xassert(!parser->flag_bit_array_count);
		xassert(!parser->fields);
		xassert(!parser->field_count);
		xassert(parser->parse);
		xassert(parser->dump);
		xassert(parser->list_type == DATA_PARSER_TYPE_INVALID);
		if ((parser->obj_openapi == OPENAPI_FORMAT_ARRAY) ||
		    (parser->obj_openapi == OPENAPI_FORMAT_OBJECT) ||
		    (parser->obj_openapi == OPENAPI_FORMAT_INVALID)) {
			/*
			 * Only one of the overrides is allowed but one must be
			 * set
			 */
			if (parser->array_type) {
				xassert(!parser->pointer_type);
			} else if (parser->pointer_type) {
				xassert(!parser->array_type);
			} else if (!parser->field_name) {
				/* field-less parser can can be any type */
			} else {
				fatal_abort("invalid openapi override");
			}
		} else {
			xassert(parser->obj_openapi > OPENAPI_FORMAT_INVALID);
			xassert(parser->obj_openapi < OPENAPI_FORMAT_MAX);
			xassert(!parser->pointer_type);
			xassert(!parser->array_type);
		}
	} else if (parser->model == PARSER_MODEL_PTR) {
		xassert(parser->pointer_type > DATA_PARSER_TYPE_INVALID);
		xassert(parser->pointer_type < DATA_PARSER_TYPE_MAX);
		xassert(parser->size == sizeof(void *));
		xassert(parser->ptr_offset == NO_VAL);
		xassert(!parser->field_name);
		xassert(!parser->key);
		xassert(!parser->field_name);
		xassert(!parser->flag_bit_array_count);
		xassert(!parser->fields);
		xassert(!parser->field_count);
		xassert(!parser->parse);
		xassert(!parser->dump);
		xassert(parser->list_type == DATA_PARSER_TYPE_INVALID);
		xassert(!parser->array_type);
		xassert(!parser->obj_openapi);
	} else if ((parser->model == PARSER_MODEL_NT_ARRAY) ||
		   (parser->model == PARSER_MODEL_NT_PTR_ARRAY)) {
		xassert(!parser->pointer_type);
		xassert(parser->array_type > DATA_PARSER_TYPE_INVALID);
		xassert(parser->array_type < DATA_PARSER_TYPE_MAX);
		xassert(parser->size == sizeof(void *));
		xassert(parser->ptr_offset == NO_VAL);
		xassert(!parser->field_name);
		xassert(!parser->key);
		xassert(!parser->field_name);
		xassert(!parser->flag_bit_array_count);
		xassert(!parser->fields);
		xassert(!parser->field_count);
		xassert(!parser->parse);
		xassert(!parser->dump);
		xassert(parser->list_type == DATA_PARSER_TYPE_INVALID);
		xassert(!parser->obj_openapi);
	} else {
		fatal_abort("invalid parser model %u", parser->model);
	}
}
#endif /* !NDEBUG */

static int PARSE_FUNC(QOS_ID)(const parser_t *const parser, void *obj,
			      data_t *src, args_t *args, data_t *parent_path)
{
	int rc;
	slurmdb_qos_rec_t *qos = NULL;
	uint32_t *qos_id = obj;

	if ((rc = resolve_qos(PARSING, parser, &qos, src, args, parent_path,
			      __func__, false)))
		return rc;

	if (qos)
		*qos_id = qos->id;
	else
		*qos_id = INFINITE;
	return rc;
}

static int PARSE_FUNC(QOS_NAME)(const parser_t *const parser, void *obj,
				data_t *src, args_t *args, data_t *parent_path)
{
	int rc;
	slurmdb_qos_rec_t *qos = NULL;
	char **qos_name = obj;

	if (!(rc = resolve_qos(PARSING, parser, &qos, src, args, parent_path,
			       __func__, true))) {
		*qos_name = xstrdup(qos->name);
		return rc;
	}

	/*
	 * QOS names may not always be resolvable to a known QOS such as in the
	 * case of creating a new QOS which references a new QOS in the same QOS
	 * list. To ignore this chicken and the egg problem, we just blindly
	 * send the QOS name to slurmdbd if we can stringifiy it.
	 */
	if (data_get_type(src) == DATA_TYPE_DICT) {
		data_t *n = data_key_get(src, "name");

		if (n && !data_get_string_converted(n, qos_name))
			return SLURM_SUCCESS;

		rc = ESLURM_INVALID_QOS;
	} else if (!data_get_string_converted(src, qos_name))
		return SLURM_SUCCESS;

	if (rc) {
		(void) data_convert_type(src, DATA_TYPE_STRING);
		parse_error(parser, args, parent_path, rc,
			    "Unable to resolve QOS %s of type %s",
			    ((data_get_type(src) == DATA_TYPE_STRING) ?
				     data_get_string(src) :
				     ""),
			    data_get_type_string(src));
	}

	return rc;
}

static int DUMP_FUNC(QOS_NAME)(const parser_t *const parser, void *obj,
			       data_t *dst, args_t *args)
{
	return DUMP_FUNC(STRING)(parser, obj, dst, args);
}

static int DUMP_FUNC(QOS_ID)(const parser_t *const parser, void *obj,
			     data_t *dst, args_t *args)
{
	uint32_t *qos_id = obj;
	slurmdb_qos_rec_t *qos = NULL;

	if (!(*qos_id) || (*qos_id) == INFINITE) {
		if (!is_complex_mode(args))
			(void) data_set_string(dst, "");

		return SLURM_SUCCESS;
	}

	/* find qos by id from global list */
	xassert(args->qos_list);
	qos = list_find_first(args->qos_list, slurmdb_find_qos_in_list, qos_id);

	if (qos && qos->name && qos->name[0])
		(void) data_set_string(dst, qos->name);
	else if (qos && qos->id)
		data_set_string_fmt(dst, "%u", qos->id);
	else if (!is_complex_mode(args)) {
		(void) data_set_string(dst, "Unknown");
		on_warn(DUMPING, parser->type, args, NULL, __func__,
			"Unknown QOS with id#%u. Unable to dump QOS.", *qos_id);
	}

	return SLURM_SUCCESS;
}

static int _foreach_dump_qos_string_id(void *x, void *arg)
{
	char *string_id = x;
	foreach_qos_string_id_t *argstruct = arg;
	const parser_t *const parser = argstruct->parser;
	data_t *dst = argstruct->ddst;
	args_t *args = argstruct->args;
	data_t *dstring_id = data_set_string(data_new(), string_id);
	data_t *parent_path = data_set_list(data_new());
	slurmdb_qos_rec_t *qos = NULL;

	data_set_string_fmt(data_list_append(parent_path), "QOS[%s]",
			    string_id);

	xassert(argstruct->magic == MAGIC_FOREACH_STRING_ID);
	xassert(args->magic == MAGIC_ARGS);
	xassert(data_get_type(dst) == DATA_TYPE_LIST);

	if (resolve_qos(DUMPING, parser, &qos, dstring_id, args, dstring_id,
			__func__, false)) {
		FREE_NULL_DATA(dstring_id);
		FREE_NULL_DATA(parent_path);
		return ESLURM_INVALID_QOS;
	}
	FREE_NULL_DATA(dstring_id);
	FREE_NULL_DATA(parent_path);

	xassert(qos);
	(void) data_set_string(data_list_append(dst), qos->name);

	return SLURM_SUCCESS;
}

static int DUMP_FUNC(QOS_STRING_ID_LIST)(const parser_t *const parser,
					 void *obj, data_t *dst, args_t *args)
{
	/*
	 * QOS string ID list is special because the contents have dynamic sizes
	 * which must be accounted for while dumping and parsing
	 */
	list_t **qos_list_ptr = obj;
	list_t *qos_list = *qos_list_ptr;
	foreach_qos_string_id_t argstruct = { .magic = MAGIC_FOREACH_STRING_ID,
					      .parser = parser,
					      .args = args,
					      .ddst = dst };

	if (!qos_list)
		return SLURM_SUCCESS;

	xassert(list_count(qos_list) >= 0);
	xassert(list_count(args->qos_list) >= 0);

	data_set_list(dst);

	if (list_for_each(qos_list, _foreach_dump_qos_string_id, &argstruct) <
	    0)
		return SLURM_ERROR;

	return SLURM_SUCCESS;
}

static data_for_each_cmd_t _foreach_parse_qos_string_id(data_t *src, void *arg)
{
	foreach_qos_string_id_t *argstruct = arg;
	const parser_t *const parser = argstruct->parser;
	list_t *qos_list = argstruct->qos_list;
	data_t *parent_path = argstruct->parent_path;
	args_t *args = argstruct->args;
	const char *caller = argstruct->caller;
	slurmdb_qos_rec_t *qos = NULL;
	data_t *ppath = data_copy(NULL, parent_path);
	data_t *ppath_last = data_get_list_last(ppath);
	int rc;

	if (argstruct->index < 0)
		argstruct->index = 0;

	/* Use jq style array zero based array notation */
	data_set_string_fmt(ppath_last, "%s[%zu]", data_get_string(ppath_last),
			    argstruct->index);

	if (!(rc = resolve_qos(PARSING, parser, &qos, src, args, parent_path,
			       caller, false))) {
		xassert(qos);
		list_append(qos_list, xstrdup_printf("%u", qos->id));
	}

	FREE_NULL_DATA(ppath);
	return (rc ? DATA_FOR_EACH_FAIL : DATA_FOR_EACH_CONT);
}

static int PARSE_FUNC(QOS_STRING_ID_LIST)(const parser_t *const parser,
					  void *obj, data_t *src, args_t *args,
					  data_t *parent_path)
{
	list_t **qos_list_ptr = obj;
	foreach_qos_string_id_t argstruct = {
		.magic = MAGIC_FOREACH_STRING_ID,
		.parser = parser,
		.args = args,
		.qos_list = list_create(xfree_ptr),
		.parent_path = parent_path,
		.caller = __func__,
		.index = -1,
	};

	if (data_list_for_each(src, _foreach_parse_qos_string_id, &argstruct) <
	    0) {
		FREE_NULL_LIST(argstruct.qos_list);
		return ESLURM_INVALID_QOS;
	}

	*qos_list_ptr = argstruct.qos_list;
	return SLURM_SUCCESS;
}

static int PARSE_FUNC(QOS_PREEMPT_LIST)(const parser_t *const parser, void *obj,
					data_t *src, args_t *args,
					data_t *parent_path)
{
	slurmdb_qos_rec_t *qos = obj;

	xassert(!qos->preempt_list);

	return PARSE(QOS_STRING_ID_LIST, qos->preempt_list, src, parent_path,
		     args);
}

static int DUMP_FUNC(QOS_PREEMPT_LIST)(const parser_t *const parser, void *obj,
				       data_t *dst, args_t *args)
{
	slurmdb_qos_rec_t *qos = obj;

	check_parser(parser);
	xassert(args->qos_list);
	xassert(!qos->preempt_list);

	data_set_list(dst);

	if (!args->qos_list)
		return ESLURM_NOT_SUPPORTED;

	/* skip empty lists */
	if (!qos->preempt_bitstr || (bit_ffs(qos->preempt_bitstr) == -1))
		return SLURM_SUCCESS;

	/* based on get_qos_complete_str_bitstr() */
	for (int i = 1; (i < bit_size(qos->preempt_bitstr)); i++) {
		slurmdb_qos_rec_t *ptr_qos;

		if (!bit_test(qos->preempt_bitstr, i))
			continue;

		if (!(ptr_qos =
			      list_find_first(args->qos_list,
					      slurmdb_find_qos_in_list, &i))) {
			int rc;
			char *bits = bit_fmt_full(qos->preempt_bitstr);

			/*
			 * There is a race condition here where the global
			 * QOS list could have changed betwen the query of the
			 * list and the bitstrs. Just error and have the user
			 * try again if they want.
			 */
			rc = on_error(
				DUMPING, parser->type, args, ESLURM_INVALID_QOS,
				"list_find_first()->slurmdb_find_qos_in_list()",
				__func__,
				"Unable to resolve Preempt QOS (bit %u/%" PRId64 "[%s]) in QOS %s(%u)",
				i, bit_size(qos->preempt_bitstr), bits,
				qos->name, qos->id);

			xfree(bits);

			if (rc)
				return rc;
		} else {
			data_set_string(data_list_append(dst), ptr_qos->name);
		}
	}

	return SLURM_SUCCESS;
}

static int _find_assoc(const parser_t *const parser, slurmdb_assoc_rec_t *dst,
		       data_t *src, slurmdb_assoc_rec_t *key, args_t *args,
		       data_t *parent_path)
{
	slurmdb_assoc_rec_t *match;

	if (!key->cluster)
		key->cluster = slurm_conf.cluster_name;

	match = list_find_first(args->assoc_list, (ListFindF) compare_assoc,
				key);

	if (key->cluster == slurm_conf.cluster_name)
		key->cluster = NULL;

	if (!match)
		return parse_error(parser, args, parent_path,
				 ESLURM_INVALID_ASSOC,
				 "Unable to find association: %pd", src);

	xassert(!dst->id || (dst->id == NO_VAL) || (dst->id == match->id));

	if (!(dst->id = match->id))
		return ESLURM_INVALID_ASSOC;

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(ASSOC_ID)(const parser_t *const parser, void *obj,
				data_t *src, args_t *args, data_t *parent_path)
{
	slurmdb_assoc_rec_t *assoc = obj;

	switch (data_get_type(src)) {
	case DATA_TYPE_STRING:
	{
		char *str = data_get_string(src);

		/* treat "" same as null */
		if (!str || !str[0])
			return SLURM_SUCCESS;

		/* fall through for non-empty strings */
	}
	case DATA_TYPE_FLOAT:
		if (data_convert_type(src, DATA_TYPE_INT_64) !=
		    DATA_TYPE_INT_64)
			return parse_error(parser, args, parent_path,
					   ESLURM_DATA_CONV_FAILED,
					   "Unable to convert %pd to integer for association id",
					   src);
		/* fall through */
	case DATA_TYPE_INT_64:
	{
		int rc;
		slurmdb_assoc_rec_t key = {
			.id = assoc->id,
			.cluster = assoc->cluster,
		};

		if ((rc = PARSE(UINT32, key.id, src, parent_path, args)))
			return rc;

		/* treat 0 same as null */
		if (!key.id)
			return SLURM_SUCCESS;

		return _find_assoc(parser, assoc, src, &key, args, parent_path);
	}
	case DATA_TYPE_NULL:
		return SLURM_SUCCESS;
	case DATA_TYPE_DICT:
	{
		int rc;
		slurmdb_assoc_rec_t key;

		if (!data_get_dict_length(src))
			return SLURM_SUCCESS;

		slurmdb_init_assoc_rec(&key, false);

		if (!(rc = PARSE(ASSOC_SHORT, key, src, parent_path, args)))
			rc = _find_assoc(parser, assoc, src, &key, args,
					 parent_path);

		slurmdb_free_assoc_rec_members(&key);
		return rc;
	}
	case DATA_TYPE_LIST:
	case DATA_TYPE_BOOL:
		return parse_error(parser, args, parent_path,
				   ESLURM_INVALID_ASSOC,
				   "Expected numeric Association ID but got %pd",
				   src);
	case DATA_TYPE_NONE:
		/* fall through */
	case DATA_TYPE_MAX:
		fatal_abort("invalid type");
	}

	fatal_abort("should never run");
}

static int DUMP_FUNC(ASSOC_ID)(const parser_t *const parser, void *obj,
			       data_t *dst, args_t *args)
{
	slurmdb_assoc_rec_t *assoc = obj;
	uint32_t id = 0;

	if (assoc->id && (assoc->id < NO_VAL)) {
		slurmdb_assoc_rec_t *match;

		if ((match = list_find_first(args->assoc_list,
					     (ListFindF) compare_assoc, assoc)))
			id = match->id;
	}

	if (is_complex_mode(args)) {
		data_set_null(dst);
		return SLURM_SUCCESS;
	}

	return DUMP(UINT32, id, dst, args);
}

static int PARSE_FUNC(JOB_ASSOC_ID)(const parser_t *const parser, void *obj,
				    data_t *src, args_t *args,
				    data_t *parent_path)
{
	int rc = SLURM_SUCCESS;
	slurmdb_job_rec_t *job = obj;
	slurmdb_assoc_rec_t *assoc = xmalloc(sizeof(*assoc));
	slurmdb_init_assoc_rec(assoc, false);

	check_parser(parser);

	rc = PARSE(ASSOC_SHORT, assoc, src, parent_path, args);

	if (!rc) {
		slurmdb_assoc_rec_t *match =
			list_find_first(args->assoc_list,
					(ListFindF) compare_assoc, assoc);

		if (match)
			job->associd = match->id;
		else
			rc = ESLURM_INVALID_ASSOC;
	}

	slurmdb_destroy_assoc_rec(assoc);

	return rc;
}

static int DUMP_FUNC(JOB_ASSOC_ID)(const parser_t *const parser, void *obj,
				   data_t *dst, args_t *args)
{
	slurmdb_job_rec_t *job = obj;
	slurmdb_assoc_rec_t *assoc = NULL;
	slurmdb_assoc_rec_t assoc_key = {
		.cluster = job->cluster, .id = job->associd
	};

	xassert(args->assoc_list);

	if (!job->associd || (job->associd == NO_VAL) ||
	    !(assoc = list_find_first(args->assoc_list,
				      (ListFindF) compare_assoc,
				      &assoc_key))) {
		/*
		 * The association is either invalid or unknown or deleted.
		 * Since this is coming from Slurm internally, issue a warning
		 * instead of erroring out to allow graceful dumping of the
		 * data.
		 */
		on_warn(DUMPING, parser->type, args, NULL, __func__,
			"Unknown association with id#%u. Unable to dump association.",
			job->associd);
		data_set_dict(dst);
		return SLURM_SUCCESS;
	} else {
		return DUMP(ASSOC_SHORT_PTR, assoc, dst, args);
	}
}

PARSE_DISABLED(JOB_PLANNED_TIME)

static int DUMP_FUNC(JOB_PLANNED_TIME)(const parser_t *const parser, void *obj,
				       data_t *dst, args_t *args)
{
	slurmdb_job_rec_t *job = obj;
	uint64_t diff = NO_VAL64;

	/* It's the same logic as used in sacct */
	if (!job->eligible || (job->eligible == INFINITE))
		diff = 0;
	else if ((job->start == NO_VAL) && job->end)
		diff = job->end - job->eligible;
	else if (job->start)
		diff = job->start - job->eligible;
	else
		diff = time(NULL) - job->eligible;

	return DUMP(UINT64_NO_VAL, diff, dst, args);
}

static int _foreach_resolve_tres_id(void *x, void *arg)
{
	slurmdb_tres_rec_t *tres = (slurmdb_tres_rec_t *) x;
	args_t *args = arg;
	slurmdb_tres_rec_t *ftres;

	xassert(args->magic == MAGIC_ARGS);

	if (!tres->type && tres->id) {
		/* resolve type/name if only id provided */
		slurmdb_tres_rec_t *c =
			list_find_first_ro(args->tres_list,
					   slurmdb_find_tres_in_list,
					   &tres->id);

		if (c) {
			tres->type = xstrdup(c->type);
			tres->name = xstrdup(c->name);
		}
	}

	/*
	 * This may be a new TRES being created so there won't be an
	 * existing TRES to compare against.
	 */
	if (!(ftres = list_find_first_ro(args->tres_list,
					 (ListFindF) fuzzy_match_tres, tres)))
		return SLURM_SUCCESS;

	/* verify ID if possible */

	if ((tres->id > 0) && (tres->id != ftres->id))
		return ESLURM_INVALID_TRES;

	if (!tres->id)
		tres->id = ftres->id;

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(TRES_STR)(const parser_t *const parser, void *obj,
				data_t *src, args_t *args, data_t *parent_path)
{
	char **tres = obj;
	int rc = SLURM_SUCCESS;
	list_t *tres_list = NULL;

	xassert(!*tres);

	if (!args->tres_list) {
		/* should not happen */
		xassert(args->tres_list);
		rc = ESLURM_NOT_SUPPORTED;
		goto cleanup;
	}

	if (data_get_type(src) != DATA_TYPE_LIST) {
		rc = parse_error(parser, args, parent_path,
				 ESLURM_DATA_EXPECTED_LIST,
				 "TRES should be LIST but is type %s",
				 data_get_type_string(src));
		goto cleanup;
	}

	if (!data_get_list_length(src)) {
		/* Ignore empty list used as workaround for OpenAPI clients */
		goto cleanup;
	}

	if ((rc = PARSE(TRES_LIST, tres_list, src, parent_path, args)))
		goto cleanup;

	list_for_each(tres_list, _foreach_resolve_tres_id, args);

	if ((*tres = slurmdb_make_tres_string(tres_list,
					      TRES_STR_FLAG_SIMPLE))) {
		rc = SLURM_SUCCESS;
	} else {
		rc = parse_error(parser, args, parent_path, ESLURM_INVALID_TRES,
				 "Unable to convert TRES to string");
		xassert(!rc); /* should not have failed */
	}

cleanup:
	FREE_NULL_LIST(tres_list);
	return rc;
}

static int DUMP_FUNC(TRES_STR)(const parser_t *const parser, void *obj,
			       data_t *dst, args_t *args)
{
	int rc;
	char **tres = obj;
	list_t *tres_list = NULL;

	xassert(args->tres_list && (list_count(args->tres_list) >= 0));

	if (!args->tres_list) {
		xassert(false);
		return on_error(DUMPING, parser->type, args,
				ESLURM_NOT_SUPPORTED, "TRES list not available",
				__func__, "TRES conversion requires TRES list");
	}

	if (!*tres || !*tres[0]) {
		/*
		 * Ignore empty TRES strings but set result as List for OpenAPI
		 * clients.
		 */
		data_set_list(dst);
		return SLURM_SUCCESS;
	}

	slurmdb_tres_list_from_string(&tres_list, *tres, TRES_STR_FLAG_BYTES);

	if (!tres_list) {
		rc = on_error(DUMPING, parser->type, args,
			      ESLURM_DATA_CONV_FAILED,
			      "slurmdb_tres_list_from_string", __func__,
			      "Unable to convert TRES from string");
	}

	if (list_is_empty(tres_list))
		goto cleanup;

	list_for_each(tres_list, _foreach_resolve_tres_id, args);

	if ((rc = DUMP(TRES_LIST, tres_list, dst, args)))
		return rc;
cleanup:
	FREE_NULL_LIST(tres_list);

	return SLURM_SUCCESS;
}

static int _foreach_list_per_tres_type_nct(void *x, void *arg)
{
	slurmdb_tres_rec_t *tres = (slurmdb_tres_rec_t *) x;
	foreach_list_per_tres_type_nct_t *args = arg;
	slurmdb_tres_nct_rec_t *tres_nct = NULL;

	xassert(args->magic == MAGIC_LIST_PER_TRES_TYPE_NCT);

	for (int i = 0; i < args->tres_nct_count; i++)
		if (args->tres_nct[i].id == tres->id)
			tres_nct = args->tres_nct + i;

	xassert(tres_nct);
	if (!tres_nct)
		/* out of sync?? */
		return -1;

	switch (args->type) {
	case TRES_EXPLODE_NODE:
		xassert(!tres_nct->node);
		free(tres_nct->node);
		/* based on find_hostname() */
		tres_nct->node = hostlist_nth(args->host_list, tres->count);
		return 1;
	case TRES_EXPLODE_TASK:
		xassert(!tres_nct->task);
		tres_nct->task = tres->count;
		return 1;
	case TRES_EXPLODE_COUNT:
		xassert(!tres_nct->count);
		tres_nct->count = tres->count;
		return 1;
	default:
		fatal("%s: unexpected type", __func__);
	}
}

static int _foreach_populate_g_tres_list(void *x, void *arg)
{
	slurmdb_tres_rec_t *tres = x;
	foreach_populate_g_tres_list *args = arg;
	slurmdb_tres_nct_rec_t *tres_nct = args->tres_nct + args->offset;

	xassert(args->magic == MAGIC_FOREACH_POPULATE_GLOBAL_TRES_LIST);

	tres_nct->id = tres->id;
	tres_nct->name = tres->name;
	tres_nct->type = tres->type;

	xassert(args->offset < args->tres_nct_count);
	args->offset += 1;
	return 0;
}

static int _dump_tres_nct(const parser_t *const parser, data_t *dst,
			  char *tres_count, char *tres_node, char *tres_task,
			  char *nodes, args_t *args)
{
	int rc = SLURM_SUCCESS;
	foreach_list_per_tres_type_nct_t fargs = {
		.magic = MAGIC_LIST_PER_TRES_TYPE_NCT,
		.args = args,
		.parser = parser,
	};
	foreach_populate_g_tres_list gtres_args = {
		.magic = MAGIC_FOREACH_POPULATE_GLOBAL_TRES_LIST,
	};
	slurmdb_tres_nct_rec_t *tres_nct = NULL;
	int tres_nct_count = 0;
	list_t *tres_count_list = NULL;
	list_t *tres_node_list = NULL;
	list_t *tres_task_list = NULL;

	xassert(args->magic == MAGIC_ARGS);
	xassert(data_get_type(dst) == DATA_TYPE_NULL);
	data_set_list(dst);

	xassert(args->tres_list);
	if (!args->tres_list)
		goto cleanup;

	if (!tres_count && !tres_node && !tres_task)
		/* ignore empty TRES strings */
		goto cleanup;

	fargs.tres_nct_count = gtres_args.tres_nct_count = tres_nct_count =
		list_count(args->tres_list);
	fargs.tres_nct = gtres_args.tres_nct = tres_nct =
		xcalloc(list_count(args->tres_list), sizeof(*tres_nct));
	if (list_for_each_ro(args->tres_list, _foreach_populate_g_tres_list,
			     &gtres_args) < 0)
		goto cleanup;

	fargs.host_list = hostlist_create(nodes);

	slurmdb_tres_list_from_string(&tres_count_list, tres_count,
				      TRES_STR_FLAG_BYTES);
	slurmdb_tres_list_from_string(&tres_node_list, tres_node,
				      TRES_STR_FLAG_BYTES);
	slurmdb_tres_list_from_string(&tres_task_list, tres_task,
				      TRES_STR_FLAG_BYTES);

	fargs.type = TRES_EXPLODE_COUNT;
	if (tres_count_list &&
	    (list_for_each(tres_count_list, _foreach_list_per_tres_type_nct,
			   &fargs) < 0))
		goto cleanup;
	fargs.type = TRES_EXPLODE_NODE;
	if (tres_node_list &&
	    (list_for_each(tres_node_list, _foreach_list_per_tres_type_nct,
			   &fargs) < 0))
		goto cleanup;
	fargs.type = TRES_EXPLODE_TASK;
	if (tres_task_list &&
	    (list_for_each(tres_task_list, _foreach_list_per_tres_type_nct,
			   &fargs) < 0))
		goto cleanup;
	/* clear type to catch unintended reuse */
	fargs.type = 0;

	for (int i = 0; !rc && (i < tres_nct_count); i++)
		if (tres_nct[i].count || tres_nct[i].node || tres_nct[i].task)
			rc = DUMP(TRES_NCT, tres_nct[i],
				  data_set_dict(data_list_append(dst)), args);

cleanup:
	FREE_NULL_LIST(tres_count_list);
	FREE_NULL_LIST(tres_node_list);
	FREE_NULL_LIST(tres_task_list);
	FREE_NULL_HOSTLIST(fargs.host_list);
	for (int i = 0; i < tres_nct_count; i++)
		/* hostlist_nth doesn't use xfree() */
		free(tres_nct[i].node);
	xfree(tres_nct);

	return rc;
}

PARSE_DISABLED(JOB_USER)

static int DUMP_FUNC(JOB_USER)(const parser_t *const parser, void *obj,
			       data_t *dst, args_t *args)
{
	char *user;
	slurmdb_job_rec_t *job = obj;

	/* job user may be set but fail back to resolving the uid */

	if (job->user && job->user[0]) {
		data_set_string(dst, job->user);
		return SLURM_SUCCESS;
	}

	user = uid_to_string_or_null(job->uid);

	if (user && user[0]) {
		data_set_string_own(dst, user);
		return SLURM_SUCCESS;
	}

	data_set_null(dst);
	xfree(user);
	return SLURM_SUCCESS;
}

PARSE_DISABLED(SLURMDB_RPC_ID)

static int DUMP_FUNC(SLURMDB_RPC_ID)(const parser_t *const parser, void *obj,
			     data_t *dst, args_t *args)
{
	slurmdbd_msg_type_t *id = obj;

	data_set_string(dst, slurmdbd_msg_type_2_str(*id, 1));

	return SLURM_SUCCESS;
}


PARSE_DISABLED(SELECT_PLUGIN_ID)
DUMP_DISABLED(SELECT_PLUGIN_ID)

PARSE_DISABLED(TASK_DISTRIBUTION)

static int DUMP_FUNC(TASK_DISTRIBUTION)(const parser_t *const parser, void *obj,
					data_t *dst, args_t *args)
{
	uint32_t *dist = obj;
	char *d = slurm_step_layout_type_name(*dist);

	data_set_string_own(dst, d);

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(STEP_ID)(const parser_t *const parser, void *obj,
			       data_t *src, args_t *args, data_t *parent_path)
{
	uint32_t *id = obj;

	(void) data_convert_type(src, DATA_TYPE_NONE);

	if (data_get_type(src) == DATA_TYPE_INT_64) {
		if (data_get_int(src) > SLURM_MAX_NORMAL_STEP_ID)
			return ESLURM_INVALID_STEP_ID_TOO_LARGE;
		if (data_get_int(src) < 0)
			return ESLURM_INVALID_STEP_ID_NEGATIVE;

		*id = data_get_int(src);
		return SLURM_SUCCESS;
	}

	if (data_convert_type(src, DATA_TYPE_STRING) == DATA_TYPE_STRING)
		return PARSE(STEP_NAMES, *id, src, parent_path, args);

	return ESLURM_DATA_CONV_FAILED;
}

static int DUMP_FUNC(STEP_ID)(const parser_t *const parser, void *obj,
			      data_t *dst, args_t *args)
{
	uint32_t *id = obj;

	if (*id > SLURM_MAX_NORMAL_STEP_ID) {
		int rc;
		data_t *name, *names = data_new();

		/*
		 * Use intermediary to convert flag dictionary response to
		 * string
		 */

		if ((rc = DUMP(STEP_NAMES, *id, names, args))) {
			FREE_NULL_DATA(names);
			return rc;
		}

		if (data_get_list_length(names) != 1) {
			FREE_NULL_DATA(names);
			return ESLURM_DATA_CONV_FAILED;
		}

		name = data_list_dequeue(names);
		FREE_NULL_DATA(names);

		data_move(dst, name);

		FREE_NULL_DATA(name);
		return SLURM_SUCCESS;
	}

	data_set_int(dst, *id);

	if (data_convert_type(dst, DATA_TYPE_STRING) != DATA_TYPE_STRING)
		return ESLURM_DATA_CONV_FAILED;
	else
		return SLURM_SUCCESS;
}

static int PARSE_FUNC(SLURM_STEP_ID_STRING)(const parser_t *const parser,
					    void *obj, data_t *src,
					    args_t *args, data_t *parent_path)
{
	slurm_step_id_t *id = obj;

	(void) data_convert_type(src, DATA_TYPE_NONE);

	if (data_get_type(src) == DATA_TYPE_STRING) {
		slurm_selected_step_t step = {0};
		int rc;

		if ((rc = PARSE(SELECTED_STEP, step, src, parent_path, args)))
			return rc;

		/* we must reject values that step_id can not store */
		if (step.array_task_id != NO_VAL)
			return ESLURM_DATA_CONV_FAILED;
		if (step.het_job_offset != NO_VAL)
			return ESLURM_DATA_CONV_FAILED;

		*id = step.step_id;
		return SLURM_SUCCESS;
	}

	/* default to step_id dictionary format */
	return PARSE(SLURM_STEP_ID, *id, src, parent_path, args);
}

static int DUMP_FUNC(SLURM_STEP_ID_STRING)(const parser_t *const parser,
					   void *obj, data_t *dst, args_t *args)
{
	slurm_step_id_t *id = obj;
	slurm_selected_step_t step = {
		.array_task_id = NO_VAL,
		.het_job_offset = NO_VAL,
		.step_id = *id,
	};

	return DUMP(SELECTED_STEP, step, dst, args);
}

PARSE_DISABLED(WCKEY_TAG)

static int DUMP_FUNC(WCKEY_TAG)(const parser_t *const parser, void *obj,
				data_t *dst, args_t *args)
{
	char **src = obj;
	WCKEY_TAG_STRUCT_t tag = {0};

	if (!*src) {
		if (is_complex_mode(args)) {
			return SLURM_SUCCESS;
		}
	} else if (*src[0] == '*') {
		tag.flags |= WCKEY_TAG_FLAGS_ASSIGNED_DEFAULT;
		tag.wckey = (*src + 1);
	} else {
		tag.wckey = *src;
	}

	return DUMP(WCKEY_TAG_STRUCT, tag, dst, args);
}

static int DUMP_FUNC(USER_ID)(const parser_t *const parser, void *obj,
			      data_t *dst, args_t *args)
{
	uid_t *uid = obj;
	char *u;

	if ((u = uid_to_string_or_null(*uid)))
		data_set_string_own(dst, u);
	else
		data_set_string(dst, "");

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(USER_ID)(const parser_t *const parser, void *obj,
			       data_t *src, args_t *args, data_t *parent_path)
{
	uid_t *uid_ptr = obj;
	uid_t uid;

	(void) data_convert_type(src, DATA_TYPE_NONE);

	switch (data_get_type(src)) {
	case DATA_TYPE_FLOAT:
		if (data_convert_type(src, DATA_TYPE_INT_64) !=
		    DATA_TYPE_INT_64)
			return parse_error(parser, args, parent_path,
					   ESLURM_DATA_CONV_FAILED, "Unable to convert %pd to integer to resolve user",
					   src);
		/* fall through */
	case DATA_TYPE_INT_64:
		uid = data_get_int(src);
		break;
	case DATA_TYPE_STRING:
	{
		int rc;
		char *str = data_get_string(src);

		if (!str || !str[0]) {
			*uid_ptr = SLURM_AUTH_NOBODY;
			return SLURM_SUCCESS;
		}

		if ((rc = uid_from_string(data_get_string(src), &uid)))
		{
			if (rc == SLURM_ERROR)
				rc = ESLURM_USER_ID_UNKNOWN;

			return parse_error(parser, args, parent_path,
					   ESLURM_USER_ID_UNKNOWN,
					   "Unable to resolve user: %s",
					   data_get_string(src));
		}

		break;
	}
	case DATA_TYPE_NULL:
		*uid_ptr = SLURM_AUTH_NOBODY;
		return SLURM_SUCCESS;
	case DATA_TYPE_DICT:
	case DATA_TYPE_LIST:
	case DATA_TYPE_BOOL:
		return parse_error(parser, args, parent_path,
				   ESLURM_DATA_CONV_FAILED,
				   "Invalid user field: %pd", src);
	case DATA_TYPE_NONE:
	case DATA_TYPE_MAX:
		fatal_abort("invalid type");
	}

	if (uid >= INT_MAX)
		return parse_error(parser, args, parent_path,
				   ESLURM_USER_ID_INVALID,
				   "Invalid user ID: %d", uid);

	*uid_ptr = uid;

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(GROUP_ID)(const parser_t *const parser, void *obj,
				data_t *src, args_t *args, data_t *parent_path)
{
	gid_t *gid_ptr = obj;
	gid_t gid;

	switch (data_convert_type(src, DATA_TYPE_NONE)) {
	case DATA_TYPE_FLOAT:
		if (data_convert_type(src, DATA_TYPE_INT_64) !=
		    DATA_TYPE_INT_64)
			return parse_error(parser, args, parent_path,
					   ESLURM_DATA_CONV_FAILED, "Unable to convert %pd to integer to resolve group",
					   src);
		/* fall through */
	case DATA_TYPE_INT_64:
		gid = data_get_int(src);
		break;
	case DATA_TYPE_STRING:
	{
		int rc;
		char *str = data_get_string(src);

		if (!str || !str[0]) {
			*gid_ptr = SLURM_AUTH_NOBODY;
			return SLURM_SUCCESS;
		}

		if ((rc = gid_from_string(data_get_string(src), &gid)))
		{
			if (rc == SLURM_ERROR)
				rc = ESLURM_GROUP_ID_UNKNOWN;

			return parse_error(parser, args, parent_path,
					   ESLURM_GROUP_ID_UNKNOWN,
					   "Unable to resolve group: %s",
					   data_get_string(src));
		}

		break;
	}
	case DATA_TYPE_NULL:
		*gid_ptr = SLURM_AUTH_NOBODY;
		return SLURM_SUCCESS;
	case DATA_TYPE_DICT:
	case DATA_TYPE_LIST:
	case DATA_TYPE_BOOL:
		return parse_error(parser, args, parent_path,
				   ESLURM_DATA_CONV_FAILED,
				   "Invalid group field: %pd", src);
	case DATA_TYPE_NONE:
	case DATA_TYPE_MAX:
		fatal_abort("invalid type");
	}

	if (gid >= INT_MAX)
		return parse_error(parser, args, parent_path,
				   ESLURM_GROUP_ID_INVALID,
				   "Invalid group ID: %d", gid);

	*gid_ptr = gid;

	return SLURM_SUCCESS;
}

static int DUMP_FUNC(GROUP_ID)(const parser_t *const parser, void *obj,
			       data_t *dst, args_t *args)
{
	gid_t *gid = obj;
	char *g;

	if ((g = gid_to_string_or_null(*gid)))
		data_set_string_own(dst, g);
	else if (is_complex_mode(args))
		data_set_null(dst);
	else
		data_set_string(dst, "");

	return SLURM_SUCCESS;
}

PARSE_DISABLED(JOB_REASON)

static int DUMP_FUNC(JOB_REASON)(const parser_t *const parser, void *obj,
				 data_t *dst, args_t *args)
{
	uint32_t *state = obj;

	data_set_string(dst, job_state_reason_string(*state));

	return SLURM_SUCCESS;
}

PARSE_DISABLED(OVERSUBSCRIBE_JOBS)

static int DUMP_FUNC(OVERSUBSCRIBE_JOBS)(const parser_t *const parser, void *obj,
				 data_t *dst, args_t *args)
{
	uint16_t *state = obj;
	uint16_t val = *state & (~SHARED_FORCE);

	data_set_int(dst, val);
	return SLURM_SUCCESS;
}

static int PARSE_FUNC(JOB_STATE_ID_STRING)(const parser_t *const parser,
					   void *obj, data_t *src, args_t *args,
					   data_t *parent_path)
{
	int rc = SLURM_SUCCESS;
	char **dst = obj;
	uint32_t state;

	if (data_get_type(src) == DATA_TYPE_INT_64)
		state = data_get_int(src);
	else if ((rc = PARSE(JOB_STATE, state, src, parent_path, args)))
		return rc;

	xfree(*dst);
	*dst = xstrdup_printf("%u", state);

	return rc;
}

static int DUMP_FUNC(JOB_STATE_ID_STRING)(const parser_t *const parser,
					  void *obj, data_t *dst, args_t *args)
{
	int rc;
	char **src = obj;
	uint32_t state;
	data_t *parent_path, *dsrc;

	parent_path = data_set_list(data_new());
	dsrc = data_set_string(data_list_append(parent_path), *src);

	rc = PARSE(JOB_STATE, state, dsrc, parent_path, args);

	FREE_NULL_DATA(parent_path);

	if (rc)
		return rc;
	else
		return DUMP(JOB_STATE, state, dst, args);
}

static int PARSE_FUNC(STRING)(const parser_t *const parser, void *obj,
			      data_t *str, args_t *args, data_t *parent_path)
{
	int rc = SLURM_SUCCESS;
	char **dst = obj;

	if (data_get_type(str) == DATA_TYPE_NULL) {
		xfree(*dst);
	} else if (data_convert_type(str, DATA_TYPE_STRING) ==
		   DATA_TYPE_STRING) {
		xfree(*dst);
		*dst = xstrdup(data_get_string(str));
	} else {
		rc = ESLURM_DATA_CONV_FAILED;
	}

	debug5("%s: string %s rc[%d]=%s", __func__, *dst, rc,
	       slurm_strerror(rc));

	return rc;
}

static int DUMP_FUNC(STRING)(const parser_t *const parser, void *obj,
			     data_t *data, args_t *args)
{
	int rc = SLURM_SUCCESS;
	char **src = obj;

	if (*src)
		data_set_string(data, *src);
	else if (is_complex_mode(args))
		data_set_null(data);
	else
		data_set_string(data, "");

	return rc;
}

static int PARSE_FUNC(FLOAT128)(const parser_t *const parser, void *obj,
				data_t *str, args_t *args, data_t *parent_path)
{
	long double *dst = obj;
	int rc = SLURM_SUCCESS;

	xassert(sizeof(long double) * 8 == 128);

	if (data_get_type(str) == DATA_TYPE_NULL)
		*dst = (double) NO_VAL;
	else if (data_convert_type(str, DATA_TYPE_FLOAT) == DATA_TYPE_FLOAT)
		*dst = data_get_float(str);
	else
		rc = ESLURM_DATA_CONV_FAILED;

	log_flag(DATA, "%s: string %Lf rc[%d]=%s", __func__, *dst, rc,
		 slurm_strerror(rc));

	return rc;
}

static int DUMP_FUNC(FLOAT128)(const parser_t *const parser, void *obj,
			       data_t *dst, args_t *args)
{
	long double *src = obj;

	/* see bug#9674 */
	if (((uint32_t) *src == INFINITE) || ((uint32_t) *src == NO_VAL))
		data_set_null(dst);
	else
		(void) data_set_float(dst, *src);

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(FLOAT64)(const parser_t *const parser, void *obj,
			       data_t *str, args_t *args, data_t *parent_path)
{
	double *dst = obj;
	int rc = SLURM_SUCCESS;

	xassert(sizeof(double) * 8 == 64);

	if (data_get_type(str) == DATA_TYPE_NULL)
		*dst = 0;
	else if (data_convert_type(str, DATA_TYPE_FLOAT) == DATA_TYPE_FLOAT)
		*dst = data_get_float(str);
	else
		rc = ESLURM_DATA_CONV_FAILED;

	log_flag(DATA, "%s: string %f rc[%d]=%s", __func__, *dst, rc,
		 slurm_strerror(rc));

	return rc;
}

static int DUMP_FUNC(FLOAT64)(const parser_t *const parser, void *obj,
			      data_t *dst, args_t *args)
{
	double *src = obj;

	(void) data_set_float(dst, *src);

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(FLOAT64_NO_VAL)(const parser_t *const parser, void *obj,
				      data_t *src, args_t *args,
				      data_t *parent_path)
{
	double *dst = obj;
	int rc = SLURM_ERROR;

	xassert(sizeof(double) * 8 == 64);

	switch (data_get_type(src)) {
	case DATA_TYPE_NULL:
		*dst = (double) NO_VAL;
		return SLURM_SUCCESS;
	case DATA_TYPE_INT_64:
	{
		int64_t value;

		if ((rc = PARSE_FUNC(INT64_NO_VAL)(parser, &value, src, args,
						   parent_path)))
			return rc;

		if (value == INFINITE64)
			*dst = (double) INFINITE;
		else if (value == NO_VAL64)
			*dst = (double) NO_VAL;
		else
			*dst = value;

		return rc;
	}
	case DATA_TYPE_DICT:
	{
		FLOAT64_NO_VAL_t fstruct = {0};

		if ((rc = PARSE(FLOAT64_NO_VAL_STRUCT, fstruct, src,
				parent_path, args)))
			return rc;

		if (fstruct.infinite)
			*dst = (double) INFINITE;
		else if (!fstruct.set)
			*dst = (double) NO_VAL;
		else if (fstruct.set)
			*dst = fstruct.number;
		else
			rc = parse_error(parser, args, parent_path,
					 ESLURM_DATA_CONV_FAILED,
					 "Expected \"number\" field when \"set\"=True but field not present");

		return rc;
	}
	case DATA_TYPE_STRING:
		if (data_convert_type(src, DATA_TYPE_FLOAT) != DATA_TYPE_FLOAT)
			return parse_error(parser, args, parent_path,
					   ESLURM_DATA_CONV_FAILED,
					   "Expected floating point number but got %pd",
					   src);
		/* fall through */
	case DATA_TYPE_FLOAT:
		*dst = data_get_float(src);
		return SLURM_SUCCESS;
	case DATA_TYPE_LIST:
	case DATA_TYPE_BOOL:
		return parse_error(parser, args, parent_path,
				   ESLURM_DATA_CONV_FAILED,
				   "Expected floating point number but got %pd",
				   src);
	case DATA_TYPE_NONE:
		/* fall through */
	case DATA_TYPE_MAX:
		fatal_abort("invalid type");
	}

	fatal_abort("should never run");
}

static int DUMP_FUNC(FLOAT64_NO_VAL)(const parser_t *const parser, void *obj,
				     data_t *dst, args_t *args)
{
	double *src = obj;
	FLOAT64_NO_VAL_t fstruct = {0};

	if (is_complex_mode(args)) {
		if (isinf(*src))
			data_set_string(dst, "Infinity");
		else if (isnan(*src))
			data_set_null(dst);
		else
			data_set_float(dst, *src);
		return SLURM_SUCCESS;
	}

	if ((uint32_t) *src == INFINITE) {
		fstruct.infinite = true;
	} else if ((uint32_t) *src == NO_VAL) {
		/* nothing to do */
	} else {
		fstruct.set = true;
		fstruct.number = *src;
	}

	return DUMP(FLOAT64_NO_VAL_STRUCT, fstruct, dst, args);
}

static int PARSE_FUNC(INT64)(const parser_t *const parser, void *obj,
			     data_t *str, args_t *args, data_t *parent_path)
{
	int64_t *dst = obj;
	int rc = SLURM_SUCCESS;

	if (data_get_type(str) == DATA_TYPE_NULL)
		*dst = 0;
	else if (data_convert_type(str, DATA_TYPE_INT_64) == DATA_TYPE_INT_64)
		*dst = data_get_int(str);
	else
		rc = parse_error(parser, args, parent_path,
				 ESLURM_DATA_CONV_FAILED,
				 "Expected integer but got %pd", str);

	return rc;
}

static int DUMP_FUNC(INT64)(const parser_t *const parser, void *obj,
			    data_t *dst, args_t *args)
{
	int64_t *src = obj;

	data_set_int(dst, *src);

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(INT64_NO_VAL)(const parser_t *const parser, void *obj,
				    data_t *src, args_t *args, data_t *parent_path)
{
	int64_t *dst = obj;

	switch (data_get_type(src)) {
	case DATA_TYPE_NULL:
		*dst = NO_VAL64;
		return SLURM_SUCCESS;
	case DATA_TYPE_FLOAT:
	{
		double value;
		int rc;

		if ((rc = PARSE_FUNC(FLOAT64_NO_VAL)(parser, &value, src, args,
						     parent_path)))
			return rc;

		if (isinf(value))
			*dst = INFINITE64;
		else if (isnan(value))
			*dst = NO_VAL64;
		else
			*dst = value;

		return rc;
	}
	case DATA_TYPE_DICT:
	{
		int rc;
		INT64_NO_VAL_t istruct = {0};

		if ((rc = PARSE(INT64_NO_VAL_STRUCT, istruct, src, parent_path,
				args)))
			return rc;

		if (istruct.infinite)
			*dst = INFINITE64;
		else if (!istruct.set)
			*dst = NO_VAL64;
		else if (istruct.set)
			*dst = istruct.number;
		else
			rc = parse_error(parser, args, parent_path,
					 ESLURM_DATA_CONV_FAILED,
					 "Expected \"number\" field when \"set\"=True but field not present");

		return rc;
	}
	case DATA_TYPE_STRING:
		if (data_convert_type(src, DATA_TYPE_INT_64) !=
		    DATA_TYPE_INT_64)
			return parse_error(parser, args, parent_path,
					   ESLURM_DATA_CONV_FAILED,
					   "Expected integer but got %pd", src);
		/* fall through */
	case DATA_TYPE_INT_64:
		return PARSE(INT64, *dst, src, parent_path, args);
	case DATA_TYPE_LIST:
	case DATA_TYPE_BOOL:
		return parse_error(parser, args, parent_path,
				   ESLURM_DATA_CONV_FAILED,
				   "Expected integer but got %pd", src);
	case DATA_TYPE_NONE:
		/* fall through */
	case DATA_TYPE_MAX:
		fatal_abort("invalid type");
	}

	fatal_abort("should never run");
}

static int DUMP_FUNC(INT64_NO_VAL)(const parser_t *const parser, void *obj,
				   data_t *dst, args_t *args)
{
	int64_t *src = obj;

	INT64_NO_VAL_t istruct = {0};

	if (is_complex_mode(args)) {
		if (*src == INFINITE64)
			data_set_string(dst, "Infinity");
		else if (*src == NO_VAL64)
			data_set_null(dst);
		else
			data_set_int(dst, *src);
		return SLURM_SUCCESS;
	}

	if (*src == INFINITE64) {
		istruct.infinite = true;
	} else if (*src == NO_VAL64) {
		/* nothing to do */
	} else {
		istruct.set = true;
		istruct.number = *src;
	}

	return DUMP(INT64_NO_VAL_STRUCT, istruct, dst, args);
}

static int PARSE_FUNC(INT32)(const parser_t *const parser, void *obj,
			     data_t *str, args_t *args, data_t *parent_path)
{
	int32_t *dst = obj;
	int rc;
	int64_t num;

	/* data_t already handles the parsing of signed */
	if ((rc = PARSE_FUNC(INT64)(parser, &num, str, args, parent_path)))
		return rc;

	if ((num > INT32_MAX) || (num < INT32_MIN))
		return EINVAL;

	*dst = num;
	return rc;
}

static int DUMP_FUNC(INT32)(const parser_t *const parser, void *obj,
				   data_t *dst, args_t *args)
{
	int32_t *src = obj;
	int64_t src64 = *src;

	return DUMP_FUNC(INT64)(parser, &src64, dst, args);
}

static int PARSE_FUNC(UINT16)(const parser_t *const parser, void *obj,
			      data_t *str, args_t *args, data_t *parent_path)
{
	uint16_t *dst = obj;
	int rc = SLURM_SUCCESS;

	if (data_get_type(str) == DATA_TYPE_NULL)
		*dst = 0;
	else if (data_convert_type(str, DATA_TYPE_INT_64) == DATA_TYPE_INT_64)
		*dst = data_get_int(str);
	else
		rc = ESLURM_DATA_CONV_FAILED;

	log_flag(DATA, "%s: string %hu rc[%d]=%s", __func__, *dst, rc,
		 slurm_strerror(rc));

	return rc;
}

static int DUMP_FUNC(UINT16)(const parser_t *const parser, void *obj,
			     data_t *dst, args_t *args)
{
	uint16_t *src = obj;

	(void) data_set_int(dst, *src);

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(UINT16_NO_VAL)(const parser_t *const parser, void *obj,
				     data_t *src, args_t *args,
				     data_t *parent_path)
{
	int rc;
	uint16_t *dst = obj;
	uint64_t num;

	if ((rc = PARSE(UINT64_NO_VAL, num, src, parent_path, args)))
		; /* do nothing on error */
	else if (num == NO_VAL64)
		*dst = NO_VAL16;
	else if (num >= NO_VAL)
		*dst = INFINITE16;
	else
		*dst = num;

	return rc;
}

static int DUMP_FUNC(UINT16_NO_VAL)(const parser_t *const parser, void *obj,
				    data_t *dst, args_t *args)
{
	uint16_t *src = obj;
	UINT16_NO_VAL_t istruct = {0};

	if (is_complex_mode(args)) {
		if (*src == INFINITE16)
			data_set_string(dst, "Infinity");
		else if (*src == NO_VAL16)
			data_set_null(dst);
		else
			data_set_int(dst, *src);
		return SLURM_SUCCESS;
	}

	if (*src == INFINITE16) {
		istruct.infinite = true;
	} else if (*src == NO_VAL16) {
		/* nothing to do */
	} else {
		istruct.set = true;
		istruct.number = *src;
	}

	return DUMP(UINT16_NO_VAL_STRUCT, istruct, dst, args);
}

static int PARSE_FUNC(UINT64_NO_VAL)(const parser_t *const parser, void *obj,
				     data_t *src, args_t *args,
				     data_t *parent_path)
{
	uint64_t *dst = obj;

	switch (data_get_type(src)) {
	case DATA_TYPE_NULL:
		*dst = NO_VAL64;
		return SLURM_SUCCESS;
	case DATA_TYPE_FLOAT:
	{
		double value;
		int rc;

		if ((rc = PARSE_FUNC(FLOAT64_NO_VAL)(parser, &value, src, args,
						     parent_path)))
			return rc;

		if (isinf(value))
			*dst = INFINITE64;
		else if (isnan(value))
			*dst = NO_VAL64;
		else
			*dst = value;

		return rc;
	}
	case DATA_TYPE_DICT:
	{
		int rc;
		UINT64_NO_VAL_t istruct = {0};

		if ((rc = PARSE(UINT64_NO_VAL_STRUCT, istruct, src, parent_path,
				args)))
			return rc;

		if (istruct.infinite)
			*dst = INFINITE64;
		else if (!istruct.set)
			*dst = NO_VAL64;
		else if (istruct.set)
			*dst = istruct.number;
		else
			rc = parse_error(parser, args, parent_path,
					 ESLURM_DATA_CONV_FAILED,
					 "Expected \"number\" field when \"set\"=True but field not present");

		return rc;
	}
	case DATA_TYPE_STRING:
		if (data_convert_type(src, DATA_TYPE_INT_64) !=
		    DATA_TYPE_INT_64)
			return parse_error(parser, args, parent_path,
					   ESLURM_DATA_CONV_FAILED,
					   "Expected integer but got %pd", src);
		/* fall through */
	case DATA_TYPE_INT_64:
		return PARSE(UINT64, *dst, src, parent_path, args);
	case DATA_TYPE_LIST:
		/* fall through */
	case DATA_TYPE_BOOL:
		return parse_error(parser, args, parent_path,
				   ESLURM_DATA_CONV_FAILED,
				   "Expected integer but got %pd", src);
	case DATA_TYPE_NONE:
		/* fall through */
	case DATA_TYPE_MAX:
		fatal_abort("invalid type: %d", data_get_type(src));
	}

	fatal_abort("should never run");
}

static int DUMP_FUNC(UINT64_NO_VAL)(const parser_t *const parser, void *obj,
				    data_t *dst, args_t *args)
{
	uint64_t *src = obj;
	UINT64_NO_VAL_t istruct = {0};

	if (is_complex_mode(args)) {
		if (*src == INFINITE64)
			data_set_string(dst, "Infinity");
		else if (*src == NO_VAL64)
			data_set_null(dst);
		else
			data_set_int(dst, *src);
		return SLURM_SUCCESS;
	}

	if (*src == INFINITE64) {
		istruct.infinite = true;
	} else if (*src == NO_VAL64) {
		/* nothing to do */
	} else {
		istruct.set = true;
		istruct.number = *src;
	}

	return DUMP(UINT64_NO_VAL_STRUCT, istruct, dst, args);
}

static int PARSE_FUNC(UINT64)(const parser_t *const parser, void *obj,
			      data_t *str, args_t *args, data_t *parent_path)
{
	uint64_t *dst = obj;
	int rc = SLURM_SUCCESS;

	if (data_get_type(str) == DATA_TYPE_NULL)
		*dst = 0;
	else if (data_convert_type(str, DATA_TYPE_INT_64) == DATA_TYPE_INT_64)
		*dst = data_get_int(str);
	else
		rc = ESLURM_DATA_CONV_FAILED;

	log_flag(DATA, "%s: string %" PRIu64 " rc[%d]=%s", __func__, *dst, rc,
		 slurm_strerror(rc));

	return rc;
}

static int DUMP_FUNC(UINT64)(const parser_t *const parser, void *obj,
			     data_t *dst, args_t *args)
{
	uint64_t *src = obj;

	/* Never set values of INF or NO_VAL */
	if ((*src == NO_VAL64) || (*src == INFINITE64))
		data_set_null(dst);
	else
		(void) data_set_int(dst, *src);

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(UINT32)(const parser_t *const parser, void *obj,
			      data_t *str, args_t *args, data_t *parent_path)
{
	uint32_t *dst = obj;
	int rc = SLURM_SUCCESS;

	if (data_get_type(str) == DATA_TYPE_NULL) {
		*dst = 0;
	} else if (data_convert_type(str, DATA_TYPE_INT_64) ==
		   DATA_TYPE_INT_64) {
		/* catch -1 and set to NO_VAL instead of rolling */
		if (0xFFFFFFFF00000000 & data_get_int(str))
			*dst = NO_VAL;
		else
			*dst = data_get_int(str);
	} else
		rc = ESLURM_DATA_CONV_FAILED;

	log_flag(DATA, "%s: string %u rc[%d]=%s", __func__, *dst, rc,
		 slurm_strerror(rc));

	return rc;
}

static int DUMP_FUNC(UINT32)(const parser_t *const parser, void *obj,
			     data_t *dst, args_t *args)
{
	uint32_t *src = obj;

	log_flag(DATA, "%s: uint32_t 0x%" PRIxPTR "=%u", __func__,
		 (uintptr_t) src, *src);
	(void) data_set_int(dst, *src);

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(UINT32_NO_VAL)(const parser_t *const parser, void *obj,
				     data_t *src, args_t *args,
				     data_t *parent_path)
{
	int rc;
	uint32_t *dst = obj;
	uint64_t num;

	if ((rc = PARSE(UINT64_NO_VAL, num, src, parent_path, args)))
		; /* do nothing on error */
	else if (num == NO_VAL64)
		*dst = NO_VAL;
	else if (num >= NO_VAL)
		*dst = INFINITE;
	else
		*dst = num;

	return rc;
}

static int DUMP_FUNC(UINT32_NO_VAL)(const parser_t *const parser, void *obj,
				    data_t *dst, args_t *args)
{
	uint32_t *src = obj;
	UINT32_NO_VAL_t istruct = {0};

	if (is_complex_mode(args)) {
		if (*src == INFINITE)
			data_set_string(dst, "Infinity");
		else if (*src == NO_VAL)
			data_set_null(dst);
		else
			data_set_int(dst, *src);
		return SLURM_SUCCESS;
	}

	if (*src == INFINITE) {
		istruct.infinite = true;
	} else if (*src == NO_VAL) {
		/* nothing to do */
	} else {
		istruct.set = true;
		istruct.number = *src;
	}

	return DUMP(UINT32_NO_VAL_STRUCT, istruct, dst, args);
}

PARSE_DISABLED(STEP_NODES)

static int DUMP_FUNC(STEP_NODES)(const parser_t *const parser, void *src,
				 data_t *dst, args_t *args)
{
	int rc;
	slurmdb_step_rec_t *step = src;
	hostlist_t *host_list;

	check_parser(parser);

	/* ignore empty node list */
	if (!step->nodes)
		return SLURM_SUCCESS;

	if (!(host_list = hostlist_create(step->nodes)))
		return errno;

	xassert(hostlist_count(host_list) == step->nnodes);

	rc = DUMP(HOSTLIST, host_list, dst, args);

	FREE_NULL_HOSTLIST(host_list);
	return rc;
}

PARSE_DISABLED(STEP_TRES_REQ_MAX)

static int DUMP_FUNC(STEP_TRES_REQ_MAX)(const parser_t *const parser, void *src,
					data_t *dst, args_t *args)
{
	slurmdb_step_rec_t *step = src;
	check_parser(parser);

	return _dump_tres_nct(parser, dst, step->stats.tres_usage_in_max,
			      step->stats.tres_usage_in_max_nodeid,
			      step->stats.tres_usage_in_max_taskid, step->nodes,
			      args);
}

PARSE_DISABLED(STEP_TRES_REQ_MIN)

static int DUMP_FUNC(STEP_TRES_REQ_MIN)(const parser_t *const parser, void *src,
					data_t *dst, args_t *args)
{
	slurmdb_step_rec_t *step = src;
	check_parser(parser);

	return _dump_tres_nct(parser, dst, step->stats.tres_usage_in_min,
			      step->stats.tres_usage_in_min_nodeid,
			      step->stats.tres_usage_in_min_taskid, step->nodes,
			      args);
}

PARSE_DISABLED(STEP_TRES_USAGE_MAX)

static int DUMP_FUNC(STEP_TRES_USAGE_MAX)(const parser_t *const parser,
					  void *src, data_t *dst, args_t *args)
{
	slurmdb_step_rec_t *step = src;
	check_parser(parser);

	return _dump_tres_nct(parser, dst, step->stats.tres_usage_out_max,
			      step->stats.tres_usage_out_max_nodeid,
			      step->stats.tres_usage_out_max_taskid,
			      step->nodes, args);
}

PARSE_DISABLED(STEP_TRES_USAGE_MIN)

static int DUMP_FUNC(STEP_TRES_USAGE_MIN)(const parser_t *const parser,
					  void *src, data_t *dst, args_t *args)
{
	slurmdb_step_rec_t *step = src;
	check_parser(parser);

	return _dump_tres_nct(parser, dst, step->stats.tres_usage_out_min,
			      step->stats.tres_usage_out_min_nodeid,
			      step->stats.tres_usage_out_min_taskid,
			      step->nodes, args);
}

static int PARSE_FUNC(BOOL)(const parser_t *const parser, void *obj,
			    data_t *src, args_t *args, data_t *parent_path)
{
	uint8_t *b = obj;

	if (data_convert_type(src, DATA_TYPE_BOOL) == DATA_TYPE_BOOL) {
		*b = data_get_bool(src);
		return SLURM_SUCCESS;
	}

	return ESLURM_DATA_CONV_FAILED;
}

static int DUMP_FUNC(BOOL)(const parser_t *const parser, void *obj, data_t *dst,
			   args_t *args)
{
	uint8_t *b = obj;

	data_set_bool(dst, *b);
	return SLURM_SUCCESS;
}

static int PARSE_FUNC(BOOL16)(const parser_t *const parser, void *obj,
			      data_t *src, args_t *args, data_t *parent_path)
{
	uint16_t *b = obj;

	if (data_convert_type(src, DATA_TYPE_BOOL) == DATA_TYPE_BOOL) {
		*b = data_get_bool(src);
		return SLURM_SUCCESS;
	}

	return ESLURM_DATA_CONV_FAILED;
}

static int DUMP_FUNC(BOOL16)(const parser_t *const parser, void *obj,
			     data_t *dst, args_t *args)
{
	uint16_t *b = obj;

	data_set_bool(dst, *b);
	return SLURM_SUCCESS;
}

static int PARSE_FUNC(BOOL16_NO_VAL)(const parser_t *const parser, void *obj,
				     data_t *src, args_t *args,
				     data_t *parent_path)
{
	uint16_t *b = obj;

	if (data_get_type(src) == DATA_TYPE_NULL) {
		*b = NO_VAL16;
		return SLURM_SUCCESS;
	}

	if ((data_get_type(src) == DATA_TYPE_INT_64) &&
	    (data_get_int(src) == -1)) {
		*b = NO_VAL16;
		return SLURM_SUCCESS;
	}

	return PARSE_FUNC(BOOL16)(parser, obj, src, args, parent_path);
}

static int DUMP_FUNC(BOOL16_NO_VAL)(const parser_t *const parser, void *obj,
				    data_t *dst, args_t *args)
{
	uint16_t *b = obj;

	if (is_complex_mode(args)) {
		if (*b == NO_VAL16)
			data_set_null(dst);
		else
			data_set_bool(dst, *b);
		return SLURM_SUCCESS;
	}

	if (*b == NO_VAL16)
		data_set_bool(dst, false);
	else
		data_set_bool(dst, *b);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(STATS_MSG_CYCLE_MEAN)

static int DUMP_FUNC(STATS_MSG_CYCLE_MEAN)(const parser_t *const parser,
					   void *obj, data_t *dst, args_t *args)
{
	stats_info_response_msg_t *stats = obj;

	if (stats->schedule_cycle_counter)
		data_set_int(dst, (stats->schedule_cycle_sum /
				   stats->schedule_cycle_counter));
	else
		data_set_int(dst, 0);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(STATS_MSG_CYCLE_MEAN_DEPTH)

static int DUMP_FUNC(STATS_MSG_CYCLE_MEAN_DEPTH)(const parser_t *const parser,
						 void *obj, data_t *dst,
						 args_t *args)
{
	stats_info_response_msg_t *stats = obj;

	if (stats->schedule_cycle_counter)
		data_set_int(dst, (stats->schedule_cycle_depth /
				   stats->schedule_cycle_counter));
	else
		data_set_int(dst, 0);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(STATS_MSG_CYCLE_PER_MIN)

static int DUMP_FUNC(STATS_MSG_CYCLE_PER_MIN)(const parser_t *const parser,
					      void *obj, data_t *dst,
					      args_t *args)
{
	stats_info_response_msg_t *stats = obj;

	if ((stats->req_time - stats->req_time_start) >= 60)
		data_set_int(dst, (stats->schedule_cycle_counter /
				   ((stats->req_time - stats->req_time_start) /
				    60)));
	else
		data_set_int(dst, 0);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(STATS_MSG_BF_EXIT)

static void _set_bf_exit_field(stats_info_response_msg_t *stats,
			       bf_exit_fields_t *dst, bf_exit_t field,
			       int value)
{
	for (int i = 0; i < ARRAY_SIZE(bf_exit_map); i++) {
		if (bf_exit_map[i].field == field) {
			int *ptr = ((void *) dst) + bf_exit_map[i].offset;
			*ptr = value;
			return;
		}
	}

	fatal_abort("unknown field %d", (int) field);
}

static int DUMP_FUNC(STATS_MSG_BF_EXIT)(const parser_t *const parser, void *obj,
					data_t *dst, args_t *args)
{
	stats_info_response_msg_t *stats = obj;
	bf_exit_fields_t fields = {0};

	/*
	 * The size of the response bf_exit array (bf_exit_cnt) should always
	 * be in sync with the number of fields in the bf_exit_map struct.
	 */
	xassert(stats->bf_exit_cnt == ARRAY_SIZE(bf_exit_map));

	for (int i = 0; i < stats->bf_exit_cnt; i++)
		_set_bf_exit_field(stats, &fields, i, stats->bf_exit[i]);

	return DUMP(BF_EXIT_FIELDS, fields, dst, args);
}

PARSE_DISABLED(STATS_MSG_SCHEDULE_EXIT)

static void _set_schedule_exit_field(stats_info_response_msg_t *stats,
				     schedule_exit_fields_t *dst,
				     schedule_exit_t field, int value)
{
	for (int i = 0; i < ARRAY_SIZE(schedule_exit_map); i++) {
		if (schedule_exit_map[i].field == field) {
			int *ptr = ((void *) dst) + schedule_exit_map[i].offset;
			*ptr = value;
			return;
		}
	}

	fatal_abort("unknown field %d", (int) field);
}

static int DUMP_FUNC(STATS_MSG_SCHEDULE_EXIT)(const parser_t *const parser,
					      void *obj, data_t *dst,
					      args_t *args)
{
	stats_info_response_msg_t *stats = obj;
	schedule_exit_fields_t fields = {0};

	/*
	 * The size of the response schedule_exit array (schedule_exit_cnt)
	 * should always be in sync with the number of fields in the
	 * schedule_exit_map struct.
	 */
	xassert(stats->schedule_exit_cnt == ARRAY_SIZE(schedule_exit_map));

	for (int i = 0; i < stats->schedule_exit_cnt; i++)
		_set_schedule_exit_field(stats, &fields, i,
					 stats->schedule_exit[i]);

	return DUMP(SCHEDULE_EXIT_FIELDS, fields, dst, args);
}

PARSE_DISABLED(STATS_MSG_BF_CYCLE_MEAN)

static int DUMP_FUNC(STATS_MSG_BF_CYCLE_MEAN)(const parser_t *const parser,
					      void *obj, data_t *dst,
					      args_t *args)
{
	stats_info_response_msg_t *stats = obj;

	if (stats->bf_cycle_counter)
		data_set_int(dst,
			     (stats->bf_cycle_sum / stats->bf_cycle_counter));
	else
		data_set_int(dst, 0);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(STATS_MSG_BF_DEPTH_MEAN)

static int DUMP_FUNC(STATS_MSG_BF_DEPTH_MEAN)(const parser_t *const parser,
					      void *obj, data_t *dst,
					      args_t *args)
{
	stats_info_response_msg_t *stats = obj;

	if (stats->bf_cycle_counter)
		data_set_int(dst,
			     (stats->bf_depth_sum / stats->bf_cycle_counter));
	else
		data_set_int(dst, 0);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(STATS_MSG_BF_DEPTH_MEAN_TRY)

static int DUMP_FUNC(STATS_MSG_BF_DEPTH_MEAN_TRY)(const parser_t *const parser,
						  void *obj, data_t *dst,
						  args_t *args)
{
	stats_info_response_msg_t *stats = obj;

	if (stats->bf_cycle_counter)
		data_set_int(dst, (stats->bf_depth_try_sum /
				   stats->bf_cycle_counter));
	else
		data_set_int(dst, 0);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(STATS_MSG_BF_QUEUE_LEN_MEAN)

static int DUMP_FUNC(STATS_MSG_BF_QUEUE_LEN_MEAN)(const parser_t *const parser,
						  void *obj, data_t *dst,
						  args_t *args)
{
	stats_info_response_msg_t *stats = obj;

	if (stats->bf_cycle_counter)
		data_set_int(dst, (stats->bf_queue_len_sum /
				   stats->bf_cycle_counter));
	else
		data_set_int(dst, 0);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(STATS_MSG_BF_TABLE_SIZE_MEAN)

static int DUMP_FUNC(STATS_MSG_BF_TABLE_SIZE_MEAN)(const parser_t *const parser,
						   void *obj, data_t *dst,
						   args_t *args)
{
	stats_info_response_msg_t *stats = obj;

	if (stats->bf_cycle_counter)
		data_set_int(dst, (stats->bf_table_size_sum /
				   stats->bf_cycle_counter));
	else
		data_set_int(dst, 0);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(STATS_MSG_BF_ACTIVE)

static int DUMP_FUNC(STATS_MSG_BF_ACTIVE)(const parser_t *const parser,
					  void *obj, data_t *dst, args_t *args)
{
	stats_info_response_msg_t *stats = obj;

	data_set_bool(dst, (stats->bf_active != 0));

	return SLURM_SUCCESS;
}

PARSE_DISABLED(STATS_MSG_RPCS_BY_TYPE)

static int DUMP_FUNC(STATS_MSG_RPCS_BY_TYPE)(const parser_t *const parser,
					     void *obj, data_t *dst,
					     args_t *args)
{
	stats_info_response_msg_t *stats = obj;
	int rc = SLURM_SUCCESS;

	data_set_list(dst);

	if (!stats->rpc_type_size)
		return SLURM_SUCCESS;

	for (int i = 0; !rc && (i < stats->rpc_type_size); i++) {
		STATS_MSG_RPC_TYPE_t rpc = {
			.id = stats->rpc_type_id[i],
			.count = stats->rpc_type_cnt[i],
			.time = stats->rpc_type_time[i],
			.average_time = NO_VAL64,
		};

		if ((stats->rpc_type_time[i] > 0) &&
		    (stats->rpc_type_cnt[i] > 0))
			rpc.average_time = stats->rpc_type_time[i] /
				stats->rpc_type_cnt[i];

		rc = DUMP(STATS_MSG_RPC_TYPE, rpc, data_list_append(dst), args);
	}

	return rc;
}

PARSE_DISABLED(STATS_MSG_RPCS_BY_USER)

static int DUMP_FUNC(STATS_MSG_RPCS_BY_USER)(const parser_t *const parser,
					     void *obj, data_t *dst,
					     args_t *args)
{
	stats_info_response_msg_t *stats = obj;
	int rc = SLURM_SUCCESS;

	data_set_list(dst);

	if (!stats->rpc_user_size)
		return SLURM_SUCCESS;

	for (int i = 0; !rc && (i < stats->rpc_user_size); i++) {
		STATS_MSG_RPC_USER_t rpc = {
			.id = stats->rpc_user_id[i],
			.count = stats->rpc_user_cnt[i],
			.time = stats->rpc_user_time[i],
			.average_time = NO_VAL64,
		};

		if ((stats->rpc_user_time[i] > 0) &&
		    (stats->rpc_user_cnt[i] > 0))
			rpc.average_time = stats->rpc_user_time[i] /
				stats->rpc_user_cnt[i];

		rc = DUMP(STATS_MSG_RPC_USER, rpc, data_list_append(dst), args);
	}

	return rc;
}

PARSE_DISABLED(STATS_MSG_RPCS_QUEUE)

static int DUMP_FUNC(STATS_MSG_RPCS_QUEUE)(const parser_t *const parser,
					   void *obj, data_t *dst, args_t *args)
{
	stats_info_response_msg_t *stats = obj;
	int rc = SLURM_SUCCESS;

	data_set_list(dst);

	if (!stats->rpc_queue_type_count)
		return SLURM_SUCCESS;

	for (int i = 0; !rc && (i < stats->rpc_queue_type_count); i++) {
		STATS_MSG_RPC_QUEUE_t rpc = {
			.id = stats->rpc_queue_type_id[i],
			.count = stats->rpc_queue_count[i],
		};

		rc = DUMP(STATS_MSG_RPC_QUEUE, rpc, data_list_append(dst),
			  args);
	}

	return rc;
}

PARSE_DISABLED(STATS_MSG_RPCS_DUMP)

static int DUMP_FUNC(STATS_MSG_RPCS_DUMP)(const parser_t *const parser,
					  void *obj, data_t *dst, args_t *args)
{
	stats_info_response_msg_t *stats = obj;
	int rc = SLURM_SUCCESS;

	data_set_list(dst);

	if (!stats->rpc_dump_count)
		return SLURM_SUCCESS;

	for (int i = 0; !rc && (i < stats->rpc_dump_count); i++) {
		STATS_MSG_RPC_DUMP_t rpc = {
			.id = stats->rpc_dump_types[i],
			.hostlist = stats->rpc_dump_hostlist[i],
		};

		rc = DUMP(STATS_MSG_RPC_DUMP, rpc, data_list_append(dst), args);
	}

	return rc;
}

static data_for_each_cmd_t _parse_foreach_CSV_STRING_list(data_t *data,
							  void *arg)
{
	parse_foreach_CSV_STRING_t *args = arg;

	if (data_convert_type(data, DATA_TYPE_STRING) != DATA_TYPE_STRING) {
		args->rc =
			parse_error(args->parser, args->args, args->parent_path,
				    ESLURM_DATA_CONV_FAILED,
				    "unable to convert csv entry %s to string",
				    data_get_type_string(data));
		return DATA_FOR_EACH_FAIL;
	}

	xstrfmtcatat(args->dst, &args->pos, "%s%s", (args->dst ? "," : ""),
		     data_get_string(data));

	return DATA_FOR_EACH_CONT;
}

static data_for_each_cmd_t _parse_foreach_CSV_STRING_dict(const char *key,
							  data_t *data,
							  void *arg)
{
	parse_foreach_CSV_STRING_t *args = arg;

	if (data_convert_type(data, DATA_TYPE_STRING) != DATA_TYPE_STRING) {
		args->rc =
			parse_error(args->parser, args->args, args->parent_path,
				    ESLURM_DATA_CONV_FAILED,
				    "unable to convert csv entry %s to string",
				    data_get_type_string(data));
		return DATA_FOR_EACH_FAIL;
	}

	xstrfmtcatat(args->dst, &args->pos, "%s%s=%s", (args->dst ? "," : ""),
		     key, data_get_string(data));

	return DATA_FOR_EACH_CONT;
}

static int PARSE_FUNC(CSV_STRING)(const parser_t *const parser, void *obj,
				  data_t *src, args_t *args,
				  data_t *parent_path)
{
	char **dst = obj;
	parse_foreach_CSV_STRING_t pargs = {
		.magic = MAGIC_FOREACH_CSV_STRING,
		.parser = parser,
		.args = args,
		.parent_path = parent_path,
	};

	xassert(!*dst);

	xfree(*dst);

	if (data_get_type(src) == DATA_TYPE_LIST) {
		(void) data_list_for_each(src, _parse_foreach_CSV_STRING_list,
					  &pargs);
	} else if (data_get_type(src) == DATA_TYPE_DICT) {
		(void) data_dict_for_each(src, _parse_foreach_CSV_STRING_dict,
					  &pargs);
	} else if (data_convert_type(src, DATA_TYPE_STRING) ==
		   DATA_TYPE_STRING) {
		*dst = xstrdup(data_get_string(src));
		return SLURM_SUCCESS;
	} else {
		return parse_error(parser, args, parent_path,
				   ESLURM_DATA_CONV_FAILED,
				   "Expected dictionary or list or string for comma delimited list but got %pd",
				   src);
	}

	if (!pargs.rc)
		*dst = pargs.dst;
	else
		xfree(pargs.dst);

	return pargs.rc;
}

static int DUMP_FUNC(CSV_STRING)(const parser_t *const parser, void *obj,
				 data_t *dst, args_t *args)
{
	char **src_ptr = obj;
	char *src = *src_ptr;
	char *save_ptr = NULL;
	char *token = NULL;
	char *str;

	data_set_list(dst);

	if (!src || (src[0] == '\0'))
		return SLURM_SUCCESS;

	str = xstrdup(src);
	token = strtok_r(str, ",", &save_ptr);
	while (token) {
		data_set_string(data_list_append(dst), token);
		token = strtok_r(NULL, ",", &save_ptr);
	}

	xfree(str);
	return SLURM_SUCCESS;
}

static data_for_each_cmd_t _parse_foreach_CSV_STRING_LIST_list(data_t *data,
							       void *arg)
{
	parse_foreach_CSV_STRING_LIST_t *args = arg;

	xassert(args->magic == MAGIC_FOREACH_CSV_STRING_LIST);

	if (data_convert_type(data, DATA_TYPE_STRING) != DATA_TYPE_STRING) {
		parse_error(args->parser, args->args, args->parent_path,
			    ESLURM_DATA_CONV_FAILED,
			    "unable to convert csv entry %s to string",
			    data_get_type_string(data));
		return DATA_FOR_EACH_FAIL;
	}

	list_append(args->list, xstrdup(data_get_string(data)));

	return DATA_FOR_EACH_CONT;
}

static data_for_each_cmd_t _parse_foreach_CSV_STRING_LIST_dict(const char *key,
							       data_t *data,
							       void *arg)
{
	parse_foreach_CSV_STRING_LIST_t *args = arg;

	xassert(args->magic == MAGIC_FOREACH_CSV_STRING_LIST);

	if (data_convert_type(data, DATA_TYPE_STRING) != DATA_TYPE_STRING) {
		parse_error(args->parser, args->args, args->parent_path,
			    ESLURM_DATA_CONV_FAILED,
			    "unable to convert csv entry %s to string",
			    data_get_type_string(data));
		return DATA_FOR_EACH_FAIL;
	}

	list_append(args->list,
		    xstrdup_printf("%s=%s", key, data_get_string(data)));

	return DATA_FOR_EACH_CONT;
}

static int PARSE_FUNC(CSV_STRING_LIST)(const parser_t *const parser, void *obj,
				       data_t *src, args_t *args,
				       data_t *parent_path)
{
	int rc = SLURM_SUCCESS;
	list_t **list_ptr = obj;
	list_t *list = list_create(xfree_ptr);

	if (data_get_type(src) == DATA_TYPE_LIST) {
		parse_foreach_CSV_STRING_LIST_t pargs = {
			.magic = MAGIC_FOREACH_CSV_STRING_LIST,
			.parser = parser,
			.args = args,
			.parent_path = parent_path,
			.list = list,
		};

		(void) data_list_for_each(src,
					  _parse_foreach_CSV_STRING_LIST_list,
					  &pargs);
	} else if (data_get_type(src) == DATA_TYPE_DICT) {
		parse_foreach_CSV_STRING_LIST_t pargs = {
			.magic = MAGIC_FOREACH_CSV_STRING_LIST,
			.parser = parser,
			.args = args,
			.parent_path = parent_path,
			.list = list,
		};

		(void) data_dict_for_each(src,
					  _parse_foreach_CSV_STRING_LIST_dict,
					  &pargs);
	} else if (data_convert_type(src, DATA_TYPE_STRING) ==
		   DATA_TYPE_STRING) {
		char *save_ptr = NULL;
		char *token = NULL;
		char *str = xstrdup(data_get_string(src));

		if (!str || (str[0] == '\0')) {
			xfree(str);
			goto cleanup;
		}

		token = strtok_r(str, ",", &save_ptr);
		while (token) {
			list_append(list, xstrdup(token));
			token = strtok_r(NULL, ",", &save_ptr);
		}

		xfree(str);
	} else {
		parse_error(parser, args, parent_path, ESLURM_DATA_CONV_FAILED,
			    "Expected dictionary or list or string for comma delimited list but got %pd",
			    src);
	}

cleanup:
	if (rc)
		FREE_NULL_LIST(list);
	else
		*list_ptr = list;

	return rc;
}

static int _dump_foreach_CSV_STRING_LIST(void *x, void *arg)
{
	char *str = x;
	parse_foreach_CSV_STRING_LIST_t *args = arg;

	xassert(args->magic == MAGIC_FOREACH_CSV_STRING_LIST);

	data_set_string(data_list_append(args->dst_list), str);

	return SLURM_SUCCESS;
}

static int DUMP_FUNC(CSV_STRING_LIST)(const parser_t *const parser, void *obj,
				      data_t *dst, args_t *args)
{
	list_t **list_ptr = obj;
	parse_foreach_CSV_STRING_LIST_t pargs = {
		.magic = MAGIC_FOREACH_CSV_STRING_LIST,
		.parser = parser,
		.args = args,
		.parent_path = NULL,
		.dst_list = dst,
	};

	data_set_list(dst);

	if (list_for_each_ro(*list_ptr, _dump_foreach_CSV_STRING_LIST,
			     &pargs) < 0)
		return ESLURM_DATA_CONV_FAILED;

	return SLURM_SUCCESS;
}

PARSE_DISABLED(NODE_SELECT_ALLOC_MEMORY)

static int DUMP_FUNC(NODE_SELECT_ALLOC_MEMORY)(const parser_t *const parser,
					       void *obj, data_t *dst,
					       args_t *args)
{
	int rc;
	node_info_t *node = obj;
	uint64_t alloc_memory = 0;

	if ((rc = slurm_get_select_nodeinfo(node->select_nodeinfo,
					    SELECT_NODEDATA_MEM_ALLOC,
					    NODE_STATE_ALLOCATED,
					    &alloc_memory))) {
		return on_error(
			DUMPING, parser->type, args, rc,
			"slurm_get_select_nodeinfo", __func__,
			"slurm_get_select_nodeinfo(%s, SELECT_NODEDATA_MEM_ALLOC) failed",
			node->name);
	}

	data_set_int(dst, alloc_memory);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(NODE_SELECT_ALLOC_CPUS)

static int DUMP_FUNC(NODE_SELECT_ALLOC_CPUS)(const parser_t *const parser,
					     void *obj, data_t *dst,
					     args_t *args)
{
	int rc;
	node_info_t *node = obj;
	uint16_t alloc_cpus = 0;

	if ((rc = slurm_get_select_nodeinfo(node->select_nodeinfo,
					    SELECT_NODEDATA_SUBCNT,
					    NODE_STATE_ALLOCATED,
					    &alloc_cpus))) {
		return on_error(DUMPING, parser->type, args, rc,
				"slurm_get_select_nodeinfo", __func__,
				"slurm_get_select_nodeinfo(%s, SELECT_NODEDATA_SUBCNT) failed",
				node->name);
	}

	data_set_int(dst, alloc_cpus);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(NODE_SELECT_ALLOC_IDLE_CPUS)

static int DUMP_FUNC(NODE_SELECT_ALLOC_IDLE_CPUS)(const parser_t *const parser,
						  void *obj, data_t *dst,
						  args_t *args)
{
	int rc;
	node_info_t *node = obj;
	uint16_t alloc_cpus = 0;

	if ((rc = slurm_get_select_nodeinfo(node->select_nodeinfo,
					    SELECT_NODEDATA_SUBCNT,
					    NODE_STATE_ALLOCATED,
					    &alloc_cpus))) {
		return on_error(DUMPING, parser->type, args, rc,
				"slurm_get_select_nodeinfo", __func__,
				"slurm_get_select_nodeinfo(%s, SELECT_NODEDATA_SUBCNT) failed",
				node->name);
	}

	data_set_int(dst, (node->cpus - alloc_cpus));

	return SLURM_SUCCESS;
}

PARSE_DISABLED(NODE_SELECT_TRES_USED)

static int DUMP_FUNC(NODE_SELECT_TRES_USED)(const parser_t *const parser,
					    void *obj, data_t *dst,
					    args_t *args)
{
	int rc;
	node_info_t *node = obj;
	char *node_alloc_tres = NULL;

	if ((rc = slurm_get_select_nodeinfo(node->select_nodeinfo,
					    SELECT_NODEDATA_TRES_ALLOC_FMT_STR,
					    NODE_STATE_ALLOCATED,
					    &node_alloc_tres))) {
		return on_error(DUMPING, parser->type, args, rc,
				"slurm_get_select_nodeinfo", __func__,
				"slurm_get_select_nodeinfo(%s, SELECT_NODEDATA_TRES_ALLOC_FMT_STR) failed",
				node->name);
	}

	if (node_alloc_tres)
		data_set_string_own(dst, node_alloc_tres);
	else
		data_set_string(dst, "");

	return SLURM_SUCCESS;
}

PARSE_DISABLED(NODE_SELECT_TRES_WEIGHTED)

static int DUMP_FUNC(NODE_SELECT_TRES_WEIGHTED)(const parser_t *const parser,
						void *obj, data_t *dst,
						args_t *args)
{
	int rc;
	node_info_t *node = obj;
	double node_tres_weighted = 0;

	if ((rc = slurm_get_select_nodeinfo(node->select_nodeinfo,
					    SELECT_NODEDATA_TRES_ALLOC_WEIGHTED,
					    NODE_STATE_ALLOCATED,
					    &node_tres_weighted))) {
		return on_error(DUMPING, parser->type, args, rc,
				"slurm_get_select_nodeinfo", __func__,
				"slurm_get_select_nodeinfo(%s, SELECT_NODEDATA_TRES_ALLOC_WEIGHTED) failed",
				node->name);
	}

	data_set_float(dst, node_tres_weighted);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(NODES)

static int DUMP_FUNC(NODES)(const parser_t *const parser, void *obj,
			    data_t *dst, args_t *args)
{
	node_info_msg_t *nodes = obj;
	int rc = SLURM_SUCCESS;

	data_set_list(dst);

	if (!nodes || !nodes->record_count) {
		on_warn(DUMPING, parser->type, args, NULL, __func__,
			"No nodes to dump");
		return SLURM_SUCCESS;
	}

	for (int i = 0; !rc && (i < nodes->record_count); i++) {
		/* filter unassigned dynamic nodes */
		if (nodes->node_array[i].name)
			rc = DUMP(NODE, nodes->node_array[i],
				  data_list_append(dst), args);
	}

	return SLURM_SUCCESS;
}

PARSE_DISABLED(LICENSES)

static int DUMP_FUNC(LICENSES)(const parser_t *const parser, void *obj,
			       data_t *dst, args_t *args)
{
	int rc = SLURM_SUCCESS;
	license_info_msg_t *msg = obj;

	data_set_list(dst);

	if (!msg->num_lic) {
		on_warn(DUMPING, parser->type, args, NULL, __func__,
			"Zero licenses to dump");
		return SLURM_SUCCESS;
	}

	for (size_t i = 0; !rc && (i < msg->num_lic); i++)
		rc = DUMP(LICENSE, msg->lic_array[i], data_list_append(dst),
			  args);

	return rc;
}

static int PARSE_FUNC(CORE_SPEC)(const parser_t *const parser, void *obj,
				 data_t *src, args_t *args, data_t *parent_path)
{
	uint16_t *spec = obj;

	if (data_convert_type(src, DATA_TYPE_INT_64) != DATA_TYPE_INT_64)
		return parse_error(parser, args, parent_path,
				   ESLURM_DATA_CONV_FAILED,
				   "Expected integer for core specification but got %pd",
				   src);

	if (data_get_int(src) >= CORE_SPEC_THREAD)
		return parse_error(parser, args, parent_path,
				   ESLURM_INVALID_CORE_CNT,
				   "Invalid core specification %" PRId64
				   " >= %d",
				   data_get_int(src), CORE_SPEC_THREAD);

	if (data_get_int(src) <= 0)
		return parse_error(parser, args, parent_path,
				   ESLURM_INVALID_CORE_CNT,
				   "Invalid core specification %" PRId64
				   " <= 0",
				   data_get_int(src));

	*spec = data_get_int(src);
	return SLURM_SUCCESS;
}

static int DUMP_FUNC(CORE_SPEC)(const parser_t *const parser, void *obj,
				data_t *dst, args_t *args)
{
	uint16_t *mem = obj;

	if (!(*mem & CORE_SPEC_THREAD))
		data_set_int(dst, *mem);
	else
		data_set_int(dst, 0);

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(THREAD_SPEC)(const parser_t *const parser, void *obj,
				   data_t *src, args_t *args,
				   data_t *parent_path)
{
	uint16_t *spec = obj;

	if (data_convert_type(src, DATA_TYPE_INT_64) != DATA_TYPE_INT_64)
		return parse_error(parser, args, parent_path,
				   ESLURM_DATA_CONV_FAILED,
				   "Expected integer for thread specification but got %pd",
				   src);

	if (data_get_int(src) >= CORE_SPEC_THREAD)
		return parse_error(parser, args, parent_path,
				   ESLURM_BAD_THREAD_PER_CORE,
				   "Invalid thread specification %" PRId64
				   " >= %d",
				   data_get_int(src), CORE_SPEC_THREAD);

	if (data_get_int(src) <= 0)
		return parse_error(parser, args, parent_path,
				   ESLURM_BAD_THREAD_PER_CORE,
				   "Invalid thread specification %" PRId64
				   "<= 0",
				   data_get_int(src));

	*spec = data_get_int(src);
	*spec |= CORE_SPEC_THREAD;
	return SLURM_SUCCESS;
}

static int DUMP_FUNC(THREAD_SPEC)(const parser_t *const parser, void *obj,
				  data_t *dst, args_t *args)
{
	uint16_t *mem = obj;

	if ((*mem & CORE_SPEC_THREAD))
		data_set_int(dst, (*mem & ~CORE_SPEC_THREAD));
	else
		data_set_int(dst, 0);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(JOB_INFO_GRES_DETAIL)

static int DUMP_FUNC(JOB_INFO_GRES_DETAIL)(const parser_t *const parser,
					   void *obj, data_t *dst, args_t *args)
{
	slurm_job_info_t *job = obj;
	xassert(job);

	data_set_list(dst);

	for (int i = 0; i < job->gres_detail_cnt; i++)
		data_set_string(data_list_append(dst), job->gres_detail_str[i]);

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(NICE)(const parser_t *const parser, void *obj,
			    data_t *src, args_t *args, data_t *parent_path)
{
	int32_t *nice_ptr = obj, nice;
	char *path = NULL;
	int rc;

	rc = PARSE(INT32, nice, src, parent_path, args);
	if (rc == EINVAL || (!rc && (llabs(nice) > (NICE_OFFSET - 3)))) {
		rc = on_error(PARSING, parser->type, args,
				ESLURM_INVALID_NICE,
				set_source_path(&path, args, parent_path),
				__func__,
				"Nice value not within +/- 2147483645");
	} else if (!rc) {
		*nice_ptr = nice + NICE_OFFSET;
	}

	xfree(path);
	return rc;
}

static int DUMP_FUNC(NICE)(const parser_t *const parser, void *obj, data_t *dst,
			   args_t *args)
{
	int64_t nice = *(uint32_t *) obj;

	if ((nice != NO_VAL) && (nice != NICE_OFFSET))
		data_set_int(dst, nice - NICE_OFFSET);
	else
		data_set_int(dst, 0);

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(MEM_PER_CPUS)(const parser_t *const parser, void *obj,
				    data_t *src, args_t *args,
				    data_t *parent_path)
{
	int rc;
	uint64_t *mem = obj;
	uint64_t cpu_mem = NO_VAL64;

	if (data_get_type(src) == DATA_TYPE_NULL) {
		*mem = NO_VAL64;
		return SLURM_SUCCESS;
	}

	if (data_get_type(src) == DATA_TYPE_STRING) {
		/* special handling for converting from string for units */
		if ((cpu_mem = str_to_mbytes(data_get_string(src))) ==
		    NO_VAL64) {
			return parse_error(parser, args, parent_path,
					   ESLURM_DATA_CONV_FAILED,
					   "Invalid formatted memory size: %s",
					   data_get_string(src));
		}
	} else if ((rc = PARSE(UINT64_NO_VAL, cpu_mem, src, parent_path,
			       args))) {
		/* error already logged */
		return rc;
	}

	if (cpu_mem == NO_VAL64) {
		*mem = NO_VAL64;
	} else if (cpu_mem == INFINITE64) {
		*mem = 0; /* 0 acts as infinity */
	} else if (cpu_mem >= MEM_PER_CPU) {
		/* memory size overflowed */
		return parse_error(parser, args, parent_path,
				   ESLURM_INVALID_TASK_MEMORY,
				   "Memory value %" PRIu64
				   " equal or larger than %" PRIu64,
				   cpu_mem, MEM_PER_CPU);
	} else {
		*mem = MEM_PER_CPU | cpu_mem;
	}

	return SLURM_SUCCESS;
}

static int DUMP_FUNC(MEM_PER_CPUS)(const parser_t *const parser, void *obj,
				   data_t *dst, args_t *args)
{
	uint64_t *mem = obj;
	uint64_t cpu_mem = NO_VAL64;

	if (*mem & MEM_PER_CPU)
		cpu_mem = *mem & ~MEM_PER_CPU;

	return DUMP(UINT64_NO_VAL, cpu_mem, dst, args);
}

static int PARSE_FUNC(MEM_PER_NODE)(const parser_t *const parser, void *obj,
				    data_t *src, args_t *args,
				    data_t *parent_path)
{
	int rc;
	uint64_t *mem = obj;
	uint64_t node_mem = NO_VAL64;

	if (data_get_type(src) == DATA_TYPE_NULL) {
		*mem = NO_VAL64;
		return SLURM_SUCCESS;
	}

	if (data_get_type(src) == DATA_TYPE_STRING) {
		/* special handling for converting from string for units */
		if ((node_mem = str_to_mbytes(data_get_string(src))) ==
		    NO_VAL64) {
			return parse_error(parser, args, parent_path,
					   ESLURM_DATA_CONV_FAILED,
					   "Invalid formatted memory size: %s",
					   data_get_string(src));
		}
	} else if ((rc = PARSE(UINT64_NO_VAL, node_mem, src, parent_path,
			       args))) {
		/* error already logged */
		return rc;
	}

	if (node_mem == NO_VAL64) {
		*mem = NO_VAL64;
	} else if (node_mem == INFINITE64) {
		*mem = 0; /* 0 acts as infinity */
	} else if (node_mem >= MEM_PER_CPU) {
		/* memory size overflowed */
		return parse_error(parser, args, parent_path,
				   ESLURM_INVALID_TASK_MEMORY,
				   "Memory value %" PRIu64
				   " equal or larger than %" PRIu64,
				   node_mem, MEM_PER_CPU);
	} else {
		*mem = node_mem;
	}

	return SLURM_SUCCESS;
}

static int DUMP_FUNC(MEM_PER_NODE)(const parser_t *const parser, void *obj,
				   data_t *dst, args_t *args)
{
	uint64_t *mem = obj;
	uint64_t node_mem = NO_VAL64;

	if (!(*mem & MEM_PER_CPU))
		node_mem = *mem;

	return DUMP(UINT64_NO_VAL, node_mem, dst, args);
}

static int _dump_node_res(data_t *dst, job_resources_t *j,
			  const size_t node_inx, const char *nodename,
			  const size_t sock_inx, size_t *bit_inx,
			  const size_t array_size, args_t *args)
{
	JOB_RES_NODE_t node = {
		.index = node_inx,
		.name = nodename,
		.cpus.count = j->cpus[node_inx],
		.cpus.used = j->cpus_used[node_inx],
		.memory.used = j->memory_used[node_inx],
		.memory.allocated = j->memory_allocated[node_inx],
	};
	const uint32_t bit_reps = (j->sockets_per_node[sock_inx] *
				 j->cores_per_socket[sock_inx]);
	int rc = SLURM_SUCCESS;

	node.sockets = xcalloc((j->sockets_per_node[sock_inx] + 1),
			       sizeof(*node.sockets));
	for (uint32_t i = 0; i < j->sockets_per_node[sock_inx]; i++)
		node.sockets[i].cores = xcalloc((j->cores_per_socket[i] + 1),
						sizeof(*node.sockets[i].cores));

	for (uint32_t i = 0; i < bit_reps; i++) {
		uint32_t socket_inx = i / j->cores_per_socket[sock_inx];
		uint32_t core_inx = i % j->cores_per_socket[sock_inx];
		JOB_RES_SOCKET_t *socket = &node.sockets[socket_inx];
		JOB_RES_CORE_t *core = &socket->cores[core_inx];

		xassert(*bit_inx < array_size);
		if (*bit_inx >= array_size) {
			rc = on_error(DUMPING, DATA_PARSER_JOB_RES_NODE, args,
				      ESLURM_BAD_TASK_COUNT, "job_resources_t",
				      __func__,
				      "unexpected invalid bit index: %zu/%zu",
				      *bit_inx, array_size);
			break;
		}

		socket->index = socket_inx;
		core->index = core_inx;

		if (bit_test(j->core_bitmap, *bit_inx)) {
			core->status |= JOB_RES_CORE_ALLOC;

			if (bit_test(j->core_bitmap_used, *bit_inx))
				core->status |= JOB_RES_CORE_IN_USE;
		} else {
			core->status = JOB_RES_CORE_UNALLOC;
		}

		(*bit_inx)++;
	}

	if (!rc)
		rc = DUMP(JOB_RES_NODE, node, dst, args);

	for (uint32_t i = 0; i < j->sockets_per_node[sock_inx]; i++)
		xfree(node.sockets[i].cores);
	xfree(node.sockets);

	return rc;
}

PARSE_DISABLED(JOB_RES_NODES)

static int DUMP_FUNC(JOB_RES_NODES)(const parser_t *const parser, void *obj,
				    data_t *dst, args_t *args)
{
	job_resources_t *j = obj;
	hostlist_t *hl = NULL;
	size_t bit_inx = 0;
	size_t array_size;
	size_t sock_inx = 0, sock_reps = 0;
	int rc = SLURM_SUCCESS;

	xassert(j);
	data_set_list(dst);

	/* log_job_resources() used as an example */

	if (!j->cores_per_socket || !j->nhosts) {
		/* not enough info present */
		return SLURM_SUCCESS;
	}

	hl = hostlist_create(j->nodes);
	array_size = bit_size(j->core_bitmap);

	for (size_t node_inx = 0; !rc && (node_inx < j->nhosts); node_inx++) {
		char *nodename = hostlist_nth(hl, node_inx);

		if (sock_reps >= j->sock_core_rep_count[sock_inx]) {
			sock_inx++;
			sock_reps = 0;
		}
		sock_reps++;

		rc = _dump_node_res(data_list_append(dst), j, node_inx,
				    nodename, sock_inx, &bit_inx, array_size,
				    args);

		free(nodename);
	}

	FREE_NULL_HOSTLIST(hl);
	return SLURM_SUCCESS;
}

PARSE_DISABLED(JOB_INFO_MSG)

static int DUMP_FUNC(JOB_INFO_MSG)(const parser_t *const parser, void *obj,
				   data_t *dst, args_t *args)
{
	int rc = SLURM_SUCCESS;
	job_info_msg_t *msg = obj;

	data_set_list(dst);

	if (!msg || !msg->record_count) {
		on_warn(DUMPING, parser->type, args, NULL, __func__,
			"Zero jobs to dump");
		return SLURM_SUCCESS;
	}

	for (size_t i = 0; !rc && (i < msg->record_count); ++i)
		rc = DUMP(JOB_INFO, msg->job_array[i], data_list_append(dst),
			  args);

	return rc;
}

PARSE_DISABLED(CONTROLLER_PING_MODE)

static int DUMP_FUNC(CONTROLLER_PING_MODE)(const parser_t *const parser,
					   void *obj, data_t *dst, args_t *args)
{
	int *mode_ptr = obj;
	int mode = *mode_ptr;

	if (mode == 0)
		data_set_string(dst, "primary");
	else if ((mode == 1) && (slurm_conf.control_cnt == 2))
		data_set_string(dst, "backup");
	else
		data_set_string_fmt(dst, "backup%u", mode);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(CONTROLLER_PING_RESULT)

static int DUMP_FUNC(CONTROLLER_PING_RESULT)(const parser_t *const parser,
					     void *obj, data_t *dst,
					     args_t *args)
{
	bool *ping_ptr = obj;
	int ping = *ping_ptr;

	if (ping)
		data_set_string(dst, "UP");
	else
		data_set_string(dst, "DOWN");

	return SLURM_SUCCESS;
}

PARSE_DISABLED(STEP_INFO_MSG)

static int DUMP_FUNC(STEP_INFO_MSG)(const parser_t *const parser, void *obj,
				    data_t *dst, args_t *args)
{
	int rc = SLURM_SUCCESS;
	job_step_info_response_msg_t *msg = obj;

	data_set_list(dst);

	if (!msg || !msg->job_step_count) {
		on_warn(DUMPING, parser->type, args, NULL, __func__,
			"Zero steps to dump");
		return SLURM_SUCCESS;
	}

	for (size_t i = 0; !rc && (i < msg->job_step_count); ++i)
		rc = DUMP(STEP_INFO, msg->job_steps[i], data_list_append(dst),
			  args);

	return rc;
}

static int PARSE_FUNC(HOLD)(const parser_t *const parser, void *obj,
			    data_t *src, args_t *args, data_t *parent_path)
{
	uint32_t *priority = obj;

	xassert(args->magic == MAGIC_ARGS);

	if (data_get_type(src) == DATA_TYPE_NULL) {
		/* ignore null as implied false */
		return SLURM_SUCCESS;
	}

	if (data_convert_type(src, DATA_TYPE_BOOL) != DATA_TYPE_BOOL)
		return ESLURM_DATA_CONV_FAILED;

	if (data_get_bool(src))
		*priority = 0;
	else
		*priority = INFINITE;

	return SLURM_SUCCESS;
}

static int DUMP_FUNC(HOLD)(const parser_t *const parser, void *obj, data_t *dst,
			   args_t *args)
{
	uint32_t *priority = obj;

	if (*priority == 0)
		data_set_bool(dst, true);
	else
		data_set_bool(dst, false);

	return SLURM_SUCCESS;
}

static data_for_each_cmd_t _foreach_hostlist_parse(data_t *data, void *arg)
{
	foreach_hostlist_parse_t *args = arg;

	xassert(args->magic == MAGIC_FOREACH_HOSTLIST);

	if (data_convert_type(data, DATA_TYPE_STRING) != DATA_TYPE_STRING) {
		parse_error(args->parser, args->args, args->parent_path,
			    ESLURM_DATA_CONV_FAILED,
			    "string expected but got %pd", data);
		return DATA_FOR_EACH_FAIL;
	}

	if (!hostlist_push(args->host_list, data_get_string(data))) {
		parse_error(args->parser, args->args, args->parent_path,
			    ESLURM_DATA_CONV_FAILED, "Invalid host string: %s",
			    data_get_string(data));
		return DATA_FOR_EACH_FAIL;
	}

	return DATA_FOR_EACH_CONT;
}

static int PARSE_FUNC(HOSTLIST)(const parser_t *const parser, void *obj,
				data_t *src, args_t *args, data_t *parent_path)
{
	int rc = SLURM_SUCCESS;
	hostlist_t **host_list_ptr = obj;
	hostlist_t *host_list = NULL;

	if (data_get_type(src) == DATA_TYPE_NULL)
		return SLURM_SUCCESS;

	if (data_get_type(src) == DATA_TYPE_STRING) {
		char *host_list_str = data_get_string(src);

		if (!host_list_str || !host_list_str[0]) {
			/* empty list -> no hostlist */
			return SLURM_SUCCESS;
		}

		if (!(host_list = hostlist_create(host_list_str))) {
			rc = parse_error(parser, args, parent_path,
					 ESLURM_DATA_CONV_FAILED,
					 "Invalid hostlist string: %s",
					 host_list_str);
			goto cleanup;
		}
	} else if (data_get_type(src) == DATA_TYPE_LIST) {
		foreach_hostlist_parse_t fargs = {
			.magic = MAGIC_FOREACH_HOSTLIST,
			.parser = parser,
			.args = args,
			.parent_path = parent_path,
		};

		fargs.host_list = host_list = hostlist_create(NULL);

		if (data_list_for_each(src, _foreach_hostlist_parse, &fargs) <
		    0)
			rc = ESLURM_DATA_CONV_FAILED;
	} else {
		rc = parse_error(parser, args, parent_path,
				 ESLURM_DATA_CONV_FAILED,
				 "string expected but got %pd", src);
		goto cleanup;
	}

	if (!rc)
		*host_list_ptr = host_list;
	else if (host_list)
		hostlist_destroy(host_list);

cleanup:
	return rc;
}

static int DUMP_FUNC(HOSTLIST)(const parser_t *const parser, void *obj,
			       data_t *dst, args_t *args)
{
	int rc = SLURM_SUCCESS;
	hostlist_t **host_list_ptr = obj;
	hostlist_t *host_list = *host_list_ptr;

	data_set_list(dst);

	if (hostlist_count(host_list)) {
		char *host;
		hostlist_iterator_t *itr = hostlist_iterator_create(host_list);

		while ((host = hostlist_next(itr))) {
			data_set_string(data_list_append(dst), host);
			free(host);
		}

		hostlist_iterator_destroy(itr);
	}

	return rc;
}

static int PARSE_FUNC(HOSTLIST_STRING)(const parser_t *const parser, void *obj,
				       data_t *src, args_t *args,
				       data_t *parent_path)
{
	int rc;
	char **host_list_str = obj;
	hostlist_t *host_list = NULL;


	if ((rc = PARSE_FUNC(HOSTLIST)(parser, &host_list, src, args,
				       parent_path)))
		return rc;

	if (host_list)
		*host_list_str = hostlist_ranged_string_xmalloc(host_list);

	hostlist_destroy(host_list);
	return rc;
}

static int DUMP_FUNC(HOSTLIST_STRING)(const parser_t *const parser, void *obj,
				      data_t *dst, args_t *args)
{
	int rc;
	char **host_list_ptr = obj;
	char *host_list_str = *host_list_ptr;
	hostlist_t *host_list;

	if (!host_list_str || !host_list_str[0]) {
		/* empty list */
		data_set_list(dst);
		return SLURM_SUCCESS;
	}

	if (!(host_list = hostlist_create(host_list_str))) {
		return on_error(DUMPING, parser->type, args,
				ESLURM_DATA_CONV_FAILED, "hostlist_create()",
				__func__, "Invalid hostlist string: %s",
				host_list_str);
	}

	rc = DUMP_FUNC(HOSTLIST)(parser, &host_list, dst, args);

	hostlist_destroy(host_list);
	return rc;
}

PARSE_DISABLED(CPU_FREQ_FLAGS)

static int DUMP_FUNC(CPU_FREQ_FLAGS)(const parser_t *const parser, void *obj,
				     data_t *dst, args_t *args)
{
	uint32_t *freq_ptr = obj;
	char buf[CPU_FREQ_FLAGS_BUF_SIZE];

	cpu_freq_to_string(buf, sizeof(buf), *freq_ptr);
	data_set_string(dst, buf);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(PARTITION_INFO_MSG)

static int DUMP_FUNC(PARTITION_INFO_MSG)(const parser_t *const parser,
					 void *obj, data_t *dst, args_t *args)
{
	int rc = SLURM_SUCCESS;
	partition_info_msg_t *msg = obj;

	data_set_list(dst);

	if (!msg || !msg->record_count) {
		on_warn(DUMPING, parser->type, args, NULL, __func__,
			"No partitions to dump");
		return SLURM_SUCCESS;
	}

	for (uint32_t i = 0; !rc && (i < msg->record_count); ++i)
		rc = DUMP(PARTITION_INFO, msg->partition_array[i],
			  data_list_append(dst), args);

	return rc;
}

PARSE_DISABLED(RESERVATION_INFO_MSG)

static int DUMP_FUNC(RESERVATION_INFO_MSG)(const parser_t *const parser,
					   void *obj, data_t *dst, args_t *args)
{
	int rc = SLURM_SUCCESS;
	reserve_info_msg_t *res = obj;

	data_set_list(dst);

	for (int i = 0; !rc && (i < res->record_count); i++)
		rc = DUMP(RESERVATION_INFO, res->reservation_array[i],
			  data_list_append(dst), args);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(RESERVATION_INFO_CORE_SPEC)

static int DUMP_FUNC(RESERVATION_INFO_CORE_SPEC)(const parser_t *const parser,
						 void *obj, data_t *dst,
						 args_t *args)
{
	int rc = SLURM_SUCCESS;
	reserve_info_t *res = obj;

	data_set_list(dst);

	for (int i = 0; !rc && (i < res->core_spec_cnt); i++)
		rc = DUMP(RESERVATION_CORE_SPEC, res->core_spec[i],
			  data_list_append(dst), args);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(JOB_ARRAY_RESPONSE_MSG)

static int DUMP_FUNC(JOB_ARRAY_RESPONSE_MSG)(const parser_t *const parser,
					     void *obj, data_t *dst,
					     args_t *args)
{
	int rc;
	job_array_resp_msg_t *msg = obj;
	JOB_ARRAY_RESPONSE_MSG_entry_t *array =
		xcalloc((msg->job_array_count + 1), sizeof(*array));

	for (int i = 0; i < msg->job_array_count; i++) {
		JOB_ARRAY_RESPONSE_MSG_entry_t *entry = &array[i];

		entry->rc = msg->error_code[i];
		entry->msg = msg->err_msg[i];

		if ((rc = unfmt_job_id_string(msg->job_array_id[i],
					      &entry->step))) {
			on_warn(DUMPING, parser->type, args,
				"unfmt_job_id_string()", __func__,
				"Unable to parse JobId=%s: %s",
				msg->job_array_id[i], slurm_strerror(rc));
		} else if (!entry->rc) {
			entry->rc = rc;
		}
	}

	rc = DUMP(JOB_ARRAY_RESPONSE_ARRAY, array, dst, args);
	xfree(array);
	return rc;
}

PARSE_DISABLED(ERROR)

static int DUMP_FUNC(ERROR)(const parser_t *const parser, void *obj,
			    data_t *dst, args_t *args)
{
	uint32_t *rc = obj;

	data_set_string(dst, slurm_strerror(*rc));

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(JOB_DESC_MSG_ARGV)(const parser_t *const parser,
					 void *obj, data_t *src, args_t *args,
					 data_t *parent_path)
{
	int rc;
	job_desc_msg_t *job = obj;

	if (data_get_type(src) == DATA_TYPE_NULL) {
		xassert(!job->argv);
		xassert(!job->argc);
		return SLURM_SUCCESS;
	}

	rc = PARSE(STRING_ARRAY, job->argv, src, parent_path, args);

	for (job->argc = 0; job->argv && job->argv[job->argc]; job->argc++)
		; /* no-op */

	return rc;
}

static int DUMP_FUNC(JOB_DESC_MSG_ARGV)(const parser_t *const parser, void *obj,
					data_t *dst, args_t *args)
{
	job_desc_msg_t *job = obj;

	if (!job || !job->argv)
		return SLURM_SUCCESS;

	return DUMP(STRING_ARRAY, job->argv, dst, args);
}

static int PARSE_FUNC(JOB_DESC_MSG_CPU_FREQ)(const parser_t *const parser,
					     void *obj, data_t *src,
					     args_t *args, data_t *parent_path)
{
	int rc;
	job_desc_msg_t *job = obj;
	char *str = NULL;

	if (data_get_type(src) == DATA_TYPE_NULL) {
		job->cpu_freq_min = NO_VAL;
		job->cpu_freq_max = NO_VAL;
		job->cpu_freq_gov = NO_VAL;
		return SLURM_SUCCESS;
	}

	if ((rc = data_get_string_converted(src, &str)))
		return parse_error(parser, args, parent_path, rc,
				   "string expected but got %pd",
				   src);

	if ((rc = cpu_freq_verify_cmdline(str, &job->cpu_freq_min,
					  &job->cpu_freq_max,
					  &job->cpu_freq_gov))) {
		xfree(str);
		return parse_error(parser, args, parent_path, rc,
				   "Invalid cpu_freuency");
	}

	xfree(str);
	return rc;
}

static int DUMP_FUNC(JOB_DESC_MSG_CPU_FREQ)(const parser_t *const parser,
					    void *obj, data_t *dst,
					    args_t *args)
{
	job_desc_msg_t *job = obj;

	if (job->cpu_freq_min || job->cpu_freq_max || job->cpu_freq_gov) {
		char *tmp = cpu_freq_to_cmdline(job->cpu_freq_min,
						job->cpu_freq_max,
						job->cpu_freq_gov);

		if (tmp)
			data_set_string_own(dst, tmp);
	}

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(JOB_DESC_MSG_ENV)(const parser_t *const parser, void *obj,
					data_t *src, args_t *args,
					data_t *parent_path)
{
	int rc;
	job_desc_msg_t *job = obj;

	if (data_get_type(src) == DATA_TYPE_NULL) {
		xassert(!job->environment);
		xassert(!job->env_size);
		return SLURM_SUCCESS;
	}

	rc = PARSE(STRING_ARRAY, job->environment, src, parent_path, args);
	job->env_size = envcount(job->environment);

	return rc;
}

static int DUMP_FUNC(JOB_DESC_MSG_ENV)(const parser_t *const parser, void *obj,
				       data_t *dst, args_t *args)
{
	job_desc_msg_t *job = obj;

	if (!job || !job->environment)
		return SLURM_SUCCESS;

	return DUMP(STRING_ARRAY, job->environment, dst, args);
}

static int PARSE_FUNC(JOB_DESC_MSG_SPANK_ENV)(const parser_t *const parser,
					      void *obj, data_t *src,
					      args_t *args, data_t *parent_path)
{
	int rc;
	job_desc_msg_t *job = obj;

	if (data_get_type(src) == DATA_TYPE_NULL) {
		xassert(!job->spank_job_env);
		xassert(!job->spank_job_env_size);
		return SLURM_SUCCESS;
	}

	rc = PARSE(STRING_ARRAY, job->spank_job_env, src, parent_path, args);
	job->spank_job_env_size = envcount(job->spank_job_env);

	return rc;
}

static int DUMP_FUNC(JOB_DESC_MSG_SPANK_ENV)(const parser_t *const parser,
					     void *obj, data_t *dst,
					     args_t *args)
{
	job_desc_msg_t *job = obj;

	if (!job || !job->spank_job_env)
		return SLURM_SUCCESS;

	return DUMP(STRING_ARRAY, job->spank_job_env, dst, args);
}

static data_for_each_cmd_t _foreach_string_array_list(const data_t *data,
						      void *arg)
{
	foreach_string_array_t *args = arg;
	char *str = NULL;
	int rc;

	xassert(args->magic == MAGIC_FOREACH_STRING_ARRAY);

	if ((rc = data_get_string_converted(data, &str))) {
		parse_error(args->parser, args->args, args->parent_path, rc,
			    "expected string but got %pd", data);
		return DATA_FOR_EACH_FAIL;
	}

	args->array[args->i] = str;
	args->i++;

	return DATA_FOR_EACH_CONT;
}

static data_for_each_cmd_t _foreach_string_array_dict(const char *key,
						      const data_t *data,
						      void *arg)
{
	foreach_string_array_t *args = arg;
	char *str = NULL, *keyvalue = NULL;
	int rc;

	xassert(args->magic == MAGIC_FOREACH_STRING_ARRAY);

	if ((rc = data_get_string_converted(data, &str))) {
		parse_error(args->parser, args->args, args->parent_path, rc,
			    "expected string but got %pd", data);
		return DATA_FOR_EACH_FAIL;
	}

	xstrfmtcat(keyvalue, "%s=%s", key, str);

	args->array[args->i] = keyvalue;
	args->i++;

	xfree(str);

	return DATA_FOR_EACH_CONT;
}

static int PARSE_FUNC(STRING_ARRAY)(const parser_t *const parser, void *obj,
				    data_t *src, args_t *args,
				    data_t *parent_path)
{
	char ***array_ptr = obj;
	foreach_string_array_t fargs = {
		.magic = MAGIC_FOREACH_STRING_ARRAY,
		.parser = parser,
		.args = args,
		.parent_path = parent_path,
	};


	if (data_get_type(src) == DATA_TYPE_LIST) {
		fargs.array = xcalloc(data_get_list_length(src) + 1,
				      sizeof(*fargs.array));

		if (data_list_for_each_const(src, _foreach_string_array_list,
					     &fargs) < 0)
			goto cleanup;
	} else if (data_get_type(src) == DATA_TYPE_DICT) {
		fargs.array = xcalloc(data_get_dict_length(src) + 1,
				      sizeof(*fargs.array));

		if (data_dict_for_each_const(src, _foreach_string_array_dict,
					     &fargs) < 0)
			goto cleanup;
	} else {
		parse_error(parser, args, parent_path,
			    ESLURM_DATA_EXPECTED_LIST,
			    "expected a list of strings but got %pd", src);
		goto cleanup;
	}

	xassert(!*array_ptr);
	*array_ptr = fargs.array;

	return SLURM_SUCCESS;
cleanup:
	if (fargs.array) {
		for (int i = 0; fargs.array[i]; i++)
			xfree(fargs.array[i]);
		xfree(fargs.array);
	}

	return ESLURM_DATA_CONV_FAILED;
}

static int DUMP_FUNC(STRING_ARRAY)(const parser_t *const parser, void *obj,
				   data_t *dst, args_t *args)
{
	char ***array_ptr = obj;
	char **array;

	data_set_list(dst);

	if (!(array = *array_ptr))
		return SLURM_SUCCESS;

	for (int i = 0; array[i]; i++)
		data_set_string(data_list_append(dst), array[i]);

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(SIGNAL)(const parser_t *const parser, void *obj,
			      data_t *src, args_t *args, data_t *parent_path)
{
	int rc;
	uint16_t *sig = obj;
	char *str = NULL;

	if (data_convert_type(src, DATA_TYPE_INT_64) == DATA_TYPE_INT_64) {
		*sig = data_get_int(src);
		return SLURM_SUCCESS;
	}

	if ((rc = data_get_string_converted(src, &str))) {
		return parse_error(parser, args, parent_path, rc,
				   "expected string but got %pd", src);
	}

	if (!str[0]) {
		*sig = NO_VAL16;
		xfree(str);
		return SLURM_SUCCESS;
	}

	if (!(*sig = sig_name2num(str))) {
		xfree(str);
		return parse_error(parser, args, parent_path, rc,
				   "Unknown signal %s", str);
	}

	if ((*sig < 1) || (*sig >= SIGRTMAX)) {
		on_warn(PARSING, parser->type, args, NULL, __func__,
			"Non-standard signal number: %u", *sig);
	}

	xfree(str);
	return rc;
}

static int DUMP_FUNC(SIGNAL)(const parser_t *const parser, void *obj,
			     data_t *dst, args_t *args)
{
	uint16_t *sig = obj;
	char *str;

	if (*sig == NO_VAL16) {
		if (is_complex_mode(args))
			data_set_null(dst);
		else
			data_set_string(dst, "");

		return SLURM_SUCCESS;
	}

	str = sig_num2name(*sig);
	data_set_string_own(dst, str);

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(BITSTR)(const parser_t *const parser, void *obj,
			      data_t *src, args_t *args, data_t *parent_path)
{
	int rc;
	bitstr_t *b = obj;

	xassert(*b);

	if (data_convert_type(src, DATA_TYPE_STRING) != DATA_TYPE_STRING)
		return parse_error(parser, args, parent_path,
				   ESLURM_DATA_CONV_FAILED,
				   "Expecting string but got %pd", src);

	rc = bit_unfmt(b, data_get_string(src));

	return rc;
}

static int DUMP_FUNC(BITSTR)(const parser_t *const parser, void *obj,
			     data_t *dst, args_t *args)
{
	bitstr_t *b = obj;
	char *str;

	if (!b)
		return SLURM_SUCCESS;

	str = bit_fmt_full(b);
	data_set_string_own(dst, str);

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(JOB_DESC_MSG_NODES)(const parser_t *const parser, void *obj,
			      data_t *src, args_t *args, data_t *parent_path)
{
	job_desc_msg_t *job = obj;

	if (data_get_type(src) == DATA_TYPE_LIST) {
		data_t *min, *max;

		if (!data_get_list_length(src) || (data_get_list_length(src) > 2)) {
			return parse_error(parser, args, parent_path,
					   ESLURM_DATA_CONV_FAILED,
					   "Node count in format of a list must have a cardinality of 2 or 1");
		}

		min = data_list_dequeue(src);
		max = data_list_dequeue(src);

		if (!max)
			SWAP(min, max);

		if (min && (data_convert_type(min, DATA_TYPE_INT_64) != DATA_TYPE_INT_64))
			return parse_error(parser, args, parent_path,
					   ESLURM_DATA_CONV_FAILED,
					   "Minimum nodes must be an integer instead of %s",
					   data_get_type_string(min));
		if (max && (data_convert_type(max, DATA_TYPE_INT_64) != DATA_TYPE_INT_64))
			return parse_error(parser, args, parent_path,
					   ESLURM_DATA_CONV_FAILED,
					   "Maximum nodes must be an integer instead of %s",
					   data_get_type_string(max));

		job->max_nodes = data_get_int(max);
		if (min)
			job->min_nodes = data_get_int(min);
	} else {
		int min, max;
		char *job_size_str = NULL;

		if (data_convert_type(src, DATA_TYPE_STRING) != DATA_TYPE_STRING)
			return parse_error(parser, args, parent_path,
					   ESLURM_DATA_CONV_FAILED,
					   "Expected string instead of %s for node counts",
					   data_get_type_string(src));

		if (!verify_node_count(data_get_string(src), &min, &max,
				       &job_size_str)) {
			xfree(job_size_str);
			return parse_error(parser, args, parent_path,
					   ESLURM_DATA_CONV_FAILED,
					   "Unknown format: %pd", src);
		}

		job->min_nodes = min;
		job->max_nodes = max;
		job->job_size_str = job_size_str;
	}

	if (job->min_nodes > job->max_nodes)
		SWAP(job->min_nodes, job->max_nodes);

	return SLURM_SUCCESS;
}

static int DUMP_FUNC(JOB_DESC_MSG_NODES)(const parser_t *const parser, void *obj,
			     data_t *dst, args_t *args)
{
	job_desc_msg_t *job = obj;

	if (job->job_size_str) {
		data_set_string(dst, job->job_size_str);
	} else if (job->min_nodes != job->max_nodes)
		data_set_string_fmt(dst, "%d-%d", job->min_nodes,
				    job->max_nodes);
	else
		data_set_string_fmt(dst, "%d", job->min_nodes);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(JOB_INFO_STDIN)

static int DUMP_FUNC(JOB_INFO_STDIN)(const parser_t *const parser, void *obj,
			    data_t *dst, args_t *args)
{
	slurm_job_info_t *job = obj;
	char *str = xmalloc(PATH_MAX);

	slurm_get_job_stdin(str, PATH_MAX, job);
	data_set_string_own(dst, str);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(JOB_INFO_STDOUT)

static int DUMP_FUNC(JOB_INFO_STDOUT)(const parser_t *const parser, void *obj,
			    data_t *dst, args_t *args)
{
	slurm_job_info_t *job = obj;
	char *str = xmalloc(PATH_MAX);

	slurm_get_job_stdout(str, PATH_MAX, job);
	data_set_string_own(dst, str);

	return SLURM_SUCCESS;
}

PARSE_DISABLED(JOB_INFO_STDERR)

static int DUMP_FUNC(JOB_INFO_STDERR)(const parser_t *const parser, void *obj,
			    data_t *dst, args_t *args)
{
	slurm_job_info_t *job = obj;
	char *str = xmalloc(PATH_MAX);

	slurm_get_job_stderr(str, PATH_MAX, job);
	data_set_string_own(dst, str);

	return SLURM_SUCCESS;
}

static int _parse_timestamp(const parser_t *const parser, time_t *time_ptr,
			    data_t *src, args_t *args, data_t *parent_path)
{
	int rc;
	time_t t;

	/* make sure NO_VAL64 is correct value here */
	xassert(sizeof(t) == sizeof(uint64_t));

	if (!src) {
		*time_ptr = NO_VAL64;
		return SLURM_SUCCESS;
	}

	switch (data_get_type(src)) {
	case DATA_TYPE_NULL:
		*time_ptr = NO_VAL64;
		return SLURM_SUCCESS;
	case DATA_TYPE_FLOAT:
		if (isnan(data_get_float(src)) || isinf(data_get_float(src))) {
			*time_ptr = NO_VAL64;
			return SLURM_SUCCESS;
		}

		if (data_convert_type(src, DATA_TYPE_INT_64) !=
		    DATA_TYPE_INT_64) {
			return parse_error(
				parser, args, parent_path,
				ESLURM_DATA_CONV_FAILED,
				"Conversion of %s to %s failed",
				data_type_to_string(DATA_TYPE_FLOAT),
				data_type_to_string(DATA_TYPE_INT_64));
		}
		/* fall-through */
	case DATA_TYPE_INT_64:
	{
		int64_t it = data_get_int(src);
		*time_ptr = it;
		return SLURM_SUCCESS;
	}
	case DATA_TYPE_STRING:
		if (!(t = parse_time(data_get_string(src), 0))) {
			return parse_error(parser, args, parent_path,
					   ESLURM_DATA_CONV_FAILED,
					   "Parsing of %s for timestamp failed",
					   data_get_string(src));
		}

		*time_ptr = t;
		return SLURM_SUCCESS;
	case DATA_TYPE_BOOL:
	case DATA_TYPE_LIST:
	case DATA_TYPE_DICT:
		/* no clear way to parse/convert */
		break;
	case DATA_TYPE_NONE:
	case DATA_TYPE_MAX:
		return ESLURM_DATA_CONV_FAILED;
	}

	/* see if UINT64_NO_VAL can parse it */
	if (!(rc = PARSE(UINT64_NO_VAL, t, src, parent_path, args)))
		*time_ptr = t;

	return rc;
}

static int PARSE_FUNC(TIMESTAMP)(const parser_t *const parser, void *obj,
				 data_t *src, args_t *args, data_t *parent_path)
{
	int rc;
	time_t t, *time_ptr = obj;

	if ((rc = _parse_timestamp(parser, &t, src, args, parent_path)))
		return rc;

	if (t == NO_VAL64) {
		return parse_error(parser, args, parent_path,
				   ESLURM_DATA_CONV_FAILED,
				   "Invalid or unset timestamp value");
	}

	*time_ptr = t;
	return SLURM_SUCCESS;
}

static int DUMP_FUNC(TIMESTAMP)(const parser_t *const parser, void *obj,
				data_t *dst, args_t *args)
{
	time_t *time_ptr = obj;
	uint64_t t = *time_ptr;

	return DUMP(UINT64, t, dst, args);
}

static int PARSE_FUNC(TIMESTAMP_NO_VAL)(const parser_t *const parser, void *obj,
					data_t *src, args_t *args,
					data_t *parent_path)
{
	int rc;
	time_t t, *time_ptr = obj;

	if ((rc = _parse_timestamp(parser, &t, src, args, parent_path)))
		return rc;

	*time_ptr = t;
	return SLURM_SUCCESS;
}

static int DUMP_FUNC(TIMESTAMP_NO_VAL)(const parser_t *const parser, void *obj,
				       data_t *dst, args_t *args)
{
	time_t *time_ptr = obj;
	uint64_t t = *time_ptr;

	return DUMP(UINT64_NO_VAL, t, dst, args);
}

static int PARSE_FUNC(JOB_CONDITION_SUBMIT_TIME)(const parser_t *const parser,
						 void *obj, data_t *src,
						 args_t *args,
						 data_t *parent_path)
{
	int rc;
	slurmdb_job_cond_t *cond = obj;
	time_t t = NO_VAL64;

	if (data_get_type(src) == DATA_TYPE_NULL)
		return SLURM_SUCCESS;

	rc = PARSE(TIMESTAMP_NO_VAL, t, src, parent_path, args);

	if (!rc && (t != NO_VAL64)) {
		cond->usage_start = t;
		cond->flags |= JOBCOND_FLAG_NO_DEFAULT_USAGE;
	}

	return rc;
}

static int DUMP_FUNC(JOB_CONDITION_SUBMIT_TIME)(const parser_t *const parser,
						void *obj, data_t *dst,
						args_t *args)
{
	slurmdb_job_cond_t *cond = obj;
	time_t t = NO_VAL64;

	if (cond->flags & JOBCOND_FLAG_NO_DEFAULT_USAGE)
		t = cond->usage_start;

	return DUMP(TIMESTAMP_NO_VAL, t, dst, args);
}

static int PARSE_FUNC(SELECTED_STEP)(const parser_t *const parser, void *obj,
				     data_t *src, args_t *args,
				     data_t *parent_path)
{
	slurm_selected_step_t *step = obj;

	if (data_convert_type(src, DATA_TYPE_STRING) != DATA_TYPE_STRING)
		return parse_error(parser, args, parent_path,
				   ESLURM_DATA_CONV_FAILED,
				   "Expecting string but got %pd", src);

	return unfmt_job_id_string(data_get_string(src), step);
}

static int DUMP_FUNC(SELECTED_STEP)(const parser_t *const parser, void *obj,
				    data_t *dst, args_t *args)
{
	int rc = SLURM_SUCCESS;
	char *str = NULL;
	slurm_selected_step_t *step = obj;

	if (!step)
		data_set_string(dst, "");
	else if (!(rc = fmt_job_id_string(step, &str)))
		data_set_string_own(dst, str);
	else
		xfree(str);

	return rc;
}

static int PARSE_FUNC(GROUP_ID_STRING)(const parser_t *const parser, void *obj,
				       data_t *src, args_t *args,
				       data_t *parent_path)
{
	int rc;
	char **str = obj;
	gid_t gid;

	if ((rc = PARSE(GROUP_ID, gid, src, parent_path, args)))
		return rc;

	xfree(*str);
	*str = xstrdup_printf("%u", gid);

	return SLURM_SUCCESS;
}

static int DUMP_FUNC(GROUP_ID_STRING)(const parser_t *const parser, void *obj,
				      data_t *dst, args_t *args)
{
	char **str = obj;
	int rc;
	gid_t gid;
	data_t *parent_path, *dsrc;
	char *gid_str;

	if (!*str || ((*str)[0] == '\0')) {
		data_set_string(dst, "");
		return SLURM_SUCCESS;
	}

	/* use gid id string as parent path and source */
	parent_path = data_set_list(data_new());
	dsrc = data_set_string(data_list_append(parent_path), *str);

	rc = PARSE(GROUP_ID, gid, dsrc, parent_path, args);

	FREE_NULL_DATA(parent_path);

	if (rc)
		return rc;

	if (!(gid_str = gid_to_string_or_null(gid))) {
		/* group id is unknown but ignore it as is internal */
		gid_str = xstrdup_printf("%u", gid);
	}

	data_set_string_own(dst, gid_str);

	return rc;
}

static int PARSE_FUNC(USER_ID_STRING)(const parser_t *const parser, void *obj,
				      data_t *src, args_t *args,
				      data_t *parent_path)
{
	char **str = obj;
	int rc;
	uid_t uid;

	if ((rc = PARSE(USER_ID, uid, src, parent_path, args)))
		return rc;

	xfree(*str);
	*str = xstrdup_printf("%u", uid);

	return SLURM_SUCCESS;
}

static int DUMP_FUNC(USER_ID_STRING)(const parser_t *const parser, void *obj,
				     data_t *dst, args_t *args)
{
	char **str = obj;
	int rc;
	uid_t uid;
	data_t *parent_path, *dsrc;
	char *uid_str;

	if (!*str || ((*str)[0] == '\0')) {
		data_set_string(dst, "");
		return SLURM_SUCCESS;
	}

	/* use uid id string as parent path and source */
	parent_path = data_set_list(data_new());
	dsrc = data_set_string(data_list_append(parent_path), *str);

	rc = PARSE(USER_ID, uid, dsrc, parent_path, args);

	FREE_NULL_DATA(parent_path);

	if (rc)
		return rc;

	if (!(uid_str = uid_to_string_or_null(uid))) {
		/* group id is unknown but ignore it as is internal */
		uid_str = xstrdup_printf("%u", uid);
	}

	data_set_string_own(dst, uid_str);

	return rc;
}

static int PARSE_FUNC(QOS_NAME_CSV_LIST)(const parser_t *const parser,
					 void *obj, data_t *src, args_t *args,
					 data_t *parent_path)
{
	int rc;
	list_t **dst = obj;
	list_t *str_list = list_create(xfree_ptr);
	data_t *d = data_new();
	char *str = NULL;

	if ((rc = PARSE(CSV_STRING_LIST, str_list, src, parent_path, args)))
		goto cleanup;

	FREE_NULL_LIST(*dst);
	*dst = list_create(xfree_ptr);

	while ((str = list_pop(str_list))) {
		char *out = NULL;

		data_set_string_own(d, str);

		if ((rc = PARSE(QOS_NAME, out, d, parent_path, args)))
			goto cleanup;

		list_append(*dst, out);
	}

cleanup:
	FREE_NULL_LIST(str_list);
	FREE_NULL_DATA(d);
	return rc;
}

static int DUMP_FUNC(QOS_NAME_CSV_LIST)(const parser_t *const parser, void *obj,
					data_t *dst, args_t *args)
{
	list_t **src = obj;

	return DUMP(CSV_STRING_LIST, *src, dst, args);
}

static int PARSE_FUNC(QOS_ID_STRING)(const parser_t *const parser, void *obj,
				     data_t *src, args_t *args,
				     data_t *parent_path)
{
	int rc;
	slurmdb_qos_rec_t *qos = NULL;
	char **id = obj;

	if (!(rc = resolve_qos(PARSING, parser, &qos, src, args, parent_path,
			       __func__, true))) {
		xfree(*id);
		xassert(qos);
		xstrfmtcat(*id, "%u", qos->id);
		return rc;
	}

	/*
	 * QOS id may not always be resolvable to a known QOS such as in the
	 * case of creating a new QOS which references a new QOS in the same QOS
	 * list. To ignore this chicken and the egg problem, we just blindly
	 * send the QOS id to slurmdbd if we can stringifiy it.
	 */
	if (data_get_type(src) == DATA_TYPE_DICT) {
		data_t *n = data_key_get(src, "id");

		if (n && !data_get_string_converted(n, id))
			return SLURM_SUCCESS;

		return ESLURM_DATA_CONV_FAILED;
	}

	if (data_convert_type(src, DATA_TYPE_INT_64) != DATA_TYPE_INT_64)
		return ESLURM_DATA_CONV_FAILED;

	if (!data_get_string_converted(src, id))
		return SLURM_SUCCESS;

	return ESLURM_DATA_CONV_FAILED;
}

static int DUMP_FUNC(QOS_ID_STRING)(const parser_t *const parser, void *obj,
				    data_t *dst, args_t *args)
{
	char **id = obj;

	data_set_string(dst, *id);

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(QOS_ID_STRING_CSV_LIST)(const parser_t *const parser,
					      void *obj, data_t *src,
					      args_t *args, data_t *parent_path)
{
	int rc;
	list_t **dst = obj;
	list_t *str_list = list_create(xfree_ptr);
	data_t *d = data_new();
	char *str = NULL;

	if ((rc = PARSE(CSV_STRING_LIST, str_list, src, parent_path, args)))
		goto cleanup;

	*dst = list_create(xfree_ptr);

	while ((str = list_pop(str_list))) {
		char *out = NULL;

		data_set_string_own(d, str);

		if ((rc = PARSE(QOS_ID_STRING, out, d, parent_path, args)))
			goto cleanup;

		list_append(*dst, out);
	}

cleanup:
	FREE_NULL_LIST(str_list);
	FREE_NULL_DATA(d);
	return rc;
}

static int DUMP_FUNC(QOS_ID_STRING_CSV_LIST)(const parser_t *const parser,
					     void *obj, data_t *dst,
					     args_t *args)
{
	list_t **src = obj;

	return DUMP(CSV_STRING_LIST, src, dst, args);
}

static int PARSE_FUNC(ASSOC_ID_STRING)(const parser_t *const parser, void *obj,
				       data_t *src, args_t *args,
				       data_t *parent_path)
{
	char **id = obj;

	if (data_convert_type(src, DATA_TYPE_INT_64) != DATA_TYPE_INT_64)
		return ESLURM_DATA_CONV_FAILED;

	if (!data_get_string_converted(src, id))
		return SLURM_SUCCESS;

	return ESLURM_DATA_CONV_FAILED;
}

static int DUMP_FUNC(ASSOC_ID_STRING)(const parser_t *const parser, void *obj,
				      data_t *dst, args_t *args)
{
	char **id = obj;

	data_set_string(dst, *id);

	return SLURM_SUCCESS;
}

static int PARSE_FUNC(ASSOC_ID_STRING_CSV_LIST)(const parser_t *const parser,
						void *obj, data_t *src,
						args_t *args,
						data_t *parent_path)
{
	int rc;
	list_t **dst = obj;
	list_t *str_list = list_create(xfree_ptr);
	data_t *d = data_new();
	char *str = NULL;

	if ((rc = PARSE(CSV_STRING_LIST, str_list, src, parent_path, args)))
		goto cleanup;

	*dst = list_create(xfree_ptr);

	while ((str = list_pop(str_list))) {
		char *out = NULL;

		data_set_string_own(d, str);

		if ((rc = PARSE(ASSOC_ID_STRING, out, d, parent_path, args)))
			goto cleanup;

		list_append(*dst, out);
	}

cleanup:
	FREE_NULL_LIST(str_list);
	FREE_NULL_DATA(d);
	return rc;
}

static int DUMP_FUNC(ASSOC_ID_STRING_CSV_LIST)(const parser_t *const parser,
					       void *obj, data_t *dst,
					       args_t *args)
{
	list_t **src = obj;

	return DUMP(CSV_STRING_LIST, src, dst, args);
}

static int PARSE_FUNC(PROCESS_EXIT_CODE)(const parser_t *const parser,
					 void *obj, data_t *src, args_t *args,
					 data_t *parent_path)
{
	int rc;
	uint32_t *return_code = obj;
	proc_exit_code_verbose_t rcv;

	/* parse numeric return code directly first */
	if (data_convert_type(src, DATA_TYPE_INT_64) == DATA_TYPE_INT_64)
		return PARSE(UINT32_NO_VAL, *return_code, src, parent_path,
			     args);

	if ((rc = PARSE(PROCESS_EXIT_CODE_VERBOSE, rcv, src, parent_path,
			args)))
		return rc;

	*return_code = rcv.return_code;
	return rc;
}

static int DUMP_FUNC(PROCESS_EXIT_CODE)(const parser_t *const parser, void *obj,
					data_t *dst, args_t *args)
{
	uint32_t *return_code = obj;
	proc_exit_code_verbose_t rcv = {
		.status = PROC_EXIT_CODE_INVALID,
		.return_code = NO_VAL,
		.signal = NO_VAL16,
	};

	if (*return_code == NO_VAL)
		rcv.status = PROC_EXIT_CODE_PENDING;
	else if (WIFEXITED(*return_code)) {
		rcv.return_code = WEXITSTATUS(*return_code);

		if (rcv.return_code)
			rcv.status = PROC_EXIT_CODE_ERROR;
		else
			rcv.status = PROC_EXIT_CODE_SUCCESS;
	} else if (WIFSIGNALED(*return_code)) {
		rcv.status = PROC_EXIT_CODE_SIGNALED;
		rcv.signal = WTERMSIG(*return_code);
	} else if (WCOREDUMP(*return_code)) {
		rcv.status = PROC_EXIT_CODE_CORE_DUMPED;
	} else {
		rcv.status = PROC_EXIT_CODE_INVALID;
		rcv.return_code = *return_code;
	}

	return DUMP(PROCESS_EXIT_CODE_VERBOSE, rcv, dst, args);
}

static void *NEW_FUNC(ASSOC)(void)
{
	slurmdb_assoc_rec_t *assoc = xmalloc(sizeof(*assoc));
	slurmdb_init_assoc_rec(assoc, false);
	return assoc;
}

static void *NEW_FUNC(USER)(void)
{
	slurmdb_user_rec_t *user = xmalloc(sizeof(*user));
	user->assoc_list = list_create(slurmdb_destroy_assoc_rec);
	user->coord_accts = list_create(slurmdb_destroy_coord_rec);
	return user;
}

static void *NEW_FUNC(ACCOUNT)(void)
{
	slurmdb_account_rec_t *acct = xmalloc(sizeof(*acct));
	acct->assoc_list = list_create(slurmdb_destroy_assoc_rec);
	acct->coordinators = list_create(slurmdb_destroy_coord_rec);
	return acct;
}

static void *NEW_FUNC(ACCOUNTS_ADD_COND)(void)
{
	slurmdb_add_assoc_cond_t *add_assoc_cond =
		xmalloc(sizeof(*add_assoc_cond));
	slurmdb_init_add_assoc_cond(add_assoc_cond, false);

	return add_assoc_cond;
}

static void *NEW_FUNC(WCKEY)(void)
{
	slurmdb_wckey_rec_t *wckey = xmalloc(sizeof(*wckey));
	slurmdb_init_wckey_rec(wckey, false);
	wckey->accounting_list = list_create(slurmdb_destroy_account_rec);
	return wckey;
}

static void *NEW_FUNC(QOS)(void)
{
	slurmdb_qos_rec_t *qos = xmalloc(sizeof(*qos));

	slurmdb_init_qos_rec(qos, false, NO_VAL);

	/*
	 * Clear the QOS_FLAG_NOTSET by slurmdb_init_qos_rec() so that
	 * flag updates won't be ignored.
	 */
	qos->flags = 0;

	/* force to off instead of NO_VAL */
	qos->preempt_mode = PREEMPT_MODE_OFF;

	return qos;
}

static void FREE_FUNC(TRES_NCT)(void *ptr)
{
	slurmdb_tres_nct_rec_t *tres = ptr;

	if (!tres)
		return;

	xfree(tres->node);
	xfree(tres->name);
	xfree(tres->type);
	xfree(tres);
}

static void *NEW_FUNC(CLUSTER_REC)(void)
{
	slurmdb_cluster_rec_t *cluster = xmalloc(sizeof(*cluster));
	slurmdb_init_cluster_rec(cluster, false);
	return cluster;
}

static void *NEW_FUNC(JOB_DESC_MSG)(void)
{
	job_desc_msg_t *job = xmalloc(sizeof(*job));
	slurm_init_job_desc_msg(job);
	return job;
}

static void *NEW_FUNC(CLUSTER_CONDITION)(void)
{
	slurmdb_cluster_cond_t *cond = xmalloc(sizeof(*cond));
	cond->flags = NO_VAL;
	return cond;
}

static void *NEW_FUNC(INSTANCE)(void)
{
	slurmdb_instance_rec_t *instance = xmalloc(sizeof(*instance));
	slurmdb_init_instance_rec(instance);
	return instance;
}

static int PARSE_FUNC(JOB_EXCLUSIVE)(const parser_t *const parser, void *obj,
				     data_t *src, args_t *args,
				     data_t *parent_path)
{
	uint16_t *flag = obj;

	if (data_get_type(src) == DATA_TYPE_NULL) {
		*flag = JOB_SHARED_OK;
		return SLURM_SUCCESS;
	}

	if (data_get_type(src) == DATA_TYPE_BOOL) {
		if (data_get_bool(src)) {
			*flag = JOB_SHARED_NONE;
		} else {
			*flag = JOB_SHARED_OK;
		}

		return SLURM_SUCCESS;
	}

	return PARSE(JOB_EXCLUSIVE_FLAGS, *flag, src, parent_path, args);
}

static int DUMP_FUNC(JOB_EXCLUSIVE)(const parser_t *const parser, void *obj,
				    data_t *dst, args_t *args)
{
	uint16_t *flag = obj;

	return DUMP(JOB_EXCLUSIVE_FLAGS, *flag, dst, args);
}

static int _parse_job_rlimit(const parser_t *const parser, void *obj,
			     data_t *src, args_t *args, data_t *parent_path,
			     const char *name)
{
	int rc;
	job_desc_msg_t *job = obj;
	uint64_t limit = NO_VAL64;

	if ((rc = PARSE(UINT64_NO_VAL, limit, src, parent_path, args))) {
		/* error already logged */
		return rc;
	}

	if (limit != NO_VAL64) {
		if ((rc = setenvf(&job->environment, name, "%" PRIu64, limit)))
			return rc;

		job->env_size = envcount(job->environment);
		return SLURM_SUCCESS;
	}

	return SLURM_SUCCESS;
}

static int _dump_job_rlimit(const parser_t *const parser, void *obj,
			    data_t *dst, args_t *args, const char *name)
{
	job_desc_msg_t *job = obj;
	uint64_t limit = NO_VAL64;
	const char *str_limit = getenvp(job->environment, "SLURM_RLIMIT_CPU");
	int rc = SLURM_SUCCESS;

	if (str_limit) {
		data_t *parent_path = data_set_list(data_new());
		data_t *d = data_set_string(data_new(), str_limit);

		/* convert env string value to integer */
		rc = PARSE(UINT64_NO_VAL, limit, d, parent_path, args);

		FREE_NULL_DATA(d);
		FREE_NULL_DATA(parent_path);
	}

	if (rc)
		return rc;

	return DUMP(UINT64_NO_VAL, limit, dst, args);
}

static int PARSE_FUNC(JOB_DESC_MSG_RLIMIT_CPU)(const parser_t *const parser,
					       void *obj, data_t *src,
					       args_t *args,
					       data_t *parent_path)
{
	return _parse_job_rlimit(parser, obj, src, args, parent_path,
				 "SLURM_RLIMIT_CPU");
}

static int DUMP_FUNC(JOB_DESC_MSG_RLIMIT_CPU)(const parser_t *const parser,
					      void *obj, data_t *dst,
					      args_t *args)
{
	return _dump_job_rlimit(parser, obj, dst, args, "SLURM_RLIMIT_CPU");
}

static int PARSE_FUNC(JOB_DESC_MSG_RLIMIT_FSIZE)(const parser_t *const parser,
						 void *obj, data_t *src,
						 args_t *args,
						 data_t *parent_path)
{
	return _parse_job_rlimit(parser, obj, src, args, parent_path,
				 "SLURM_RLIMIT_FSIZE");
}

static int DUMP_FUNC(JOB_DESC_MSG_RLIMIT_FSIZE)(const parser_t *const parser,
						void *obj, data_t *dst,
						args_t *args)
{
	return _dump_job_rlimit(parser, obj, dst, args, "SLURM_RLIMIT_FSIZE");
}

static int PARSE_FUNC(JOB_DESC_MSG_RLIMIT_DATA)(const parser_t *const parser,
						void *obj, data_t *src,
						args_t *args,
						data_t *parent_path)
{
	return _parse_job_rlimit(parser, obj, src, args, parent_path,
				 "SLURM_RLIMIT_DATA");
}

static int DUMP_FUNC(JOB_DESC_MSG_RLIMIT_DATA)(const parser_t *const parser,
					       void *obj, data_t *dst,
					       args_t *args)
{
	return _dump_job_rlimit(parser, obj, dst, args, "SLURM_RLIMIT_DATA");
}

static int PARSE_FUNC(JOB_DESC_MSG_RLIMIT_STACK)(const parser_t *const parser,
						 void *obj, data_t *src,
						 args_t *args,
						 data_t *parent_path)
{
	return _parse_job_rlimit(parser, obj, src, args, parent_path,
				 "SLURM_RLIMIT_STACK");
}

static int DUMP_FUNC(JOB_DESC_MSG_RLIMIT_STACK)(const parser_t *const parser,
						void *obj, data_t *dst,
						args_t *args)
{
	return _dump_job_rlimit(parser, obj, dst, args, "SLURM_RLIMIT_STACK");
}

static int PARSE_FUNC(JOB_DESC_MSG_RLIMIT_CORE)(const parser_t *const parser,
						void *obj, data_t *src,
						args_t *args,
						data_t *parent_path)
{
	return _parse_job_rlimit(parser, obj, src, args, parent_path,
				 "SLURM_RLIMIT_CORE");
}

static int DUMP_FUNC(JOB_DESC_MSG_RLIMIT_CORE)(const parser_t *const parser,
					       void *obj, data_t *dst,
					       args_t *args)
{
	return _dump_job_rlimit(parser, obj, dst, args, "SLURM_RLIMIT_CORE");
}

static int PARSE_FUNC(JOB_DESC_MSG_RLIMIT_RSS)(const parser_t *const parser,
					       void *obj, data_t *src,
					       args_t *args,
					       data_t *parent_path)
{
	return _parse_job_rlimit(parser, obj, src, args, parent_path,
				 "SLURM_RLIMIT_RSS");
}

static int DUMP_FUNC(JOB_DESC_MSG_RLIMIT_RSS)(const parser_t *const parser,
					      void *obj, data_t *dst,
					      args_t *args)
{
	return _dump_job_rlimit(parser, obj, dst, args, "SLURM_RLIMIT_RSS");
}

static int PARSE_FUNC(JOB_DESC_MSG_RLIMIT_NPROC)(const parser_t *const parser,
						 void *obj, data_t *src,
						 args_t *args,
						 data_t *parent_path)
{
	return _parse_job_rlimit(parser, obj, src, args, parent_path,
				 "SLURM_RLIMIT_NPROC");
}

static int DUMP_FUNC(JOB_DESC_MSG_RLIMIT_NPROC)(const parser_t *const parser,
						void *obj, data_t *dst,
						args_t *args)
{
	return _dump_job_rlimit(parser, obj, dst, args, "SLURM_RLIMIT_NPROC");
}

static int PARSE_FUNC(JOB_DESC_MSG_RLIMIT_NOFILE)(const parser_t *const parser,
						  void *obj, data_t *src,
						  args_t *args,
						  data_t *parent_path)
{
	return _parse_job_rlimit(parser, obj, src, args, parent_path,
				 "SLURM_RLIMIT_NOFILE");
}

static int DUMP_FUNC(JOB_DESC_MSG_RLIMIT_NOFILE)(const parser_t *const parser,
						 void *obj, data_t *dst,
						 args_t *args)
{
	return _dump_job_rlimit(parser, obj, dst, args, "SLURM_RLIMIT_NOFILE");
}

static int PARSE_FUNC(JOB_DESC_MSG_RLIMIT_MEMLOCK)(const parser_t *const parser,
						   void *obj, data_t *src,
						   args_t *args,
						   data_t *parent_path)
{
	return _parse_job_rlimit(parser, obj, src, args, parent_path,
				 "SLURM_RLIMIT_MEMLOCK");
}

static int DUMP_FUNC(JOB_DESC_MSG_RLIMIT_MEMLOCK)(const parser_t *const parser,
						  void *obj, data_t *dst,
						  args_t *args)
{
	return _dump_job_rlimit(parser, obj, dst, args, "SLURM_RLIMIT_MEMLOCK");
}

static int PARSE_FUNC(JOB_DESC_MSG_RLIMIT_AS)(const parser_t *const parser,
					      void *obj, data_t *src,
					      args_t *args, data_t *parent_path)
{
	return _parse_job_rlimit(parser, obj, src, args, parent_path,
				 "SLURM_RLIMIT_AS");
}

static int DUMP_FUNC(JOB_DESC_MSG_RLIMIT_AS)(const parser_t *const parser,
					     void *obj, data_t *dst,
					     args_t *args)
{
	return _dump_job_rlimit(parser, obj, dst, args, "SLURM_RLIMIT_AS");
}

PARSE_DISABLED(ASSOC_SHARES_OBJ_LIST)

static int _foreach_dump_ASSOC_SHARES_OBJ_LIST(void *x, void *arg)
{
	assoc_shares_object_t *obj = x;
	foreach_dump_ASSOC_SHARES_OBJ_LIST_t *args = arg;
	data_t *e = data_list_append(args->dst);
	assoc_shares_object_wrap_t wrap = {
		.obj = *obj,
		.tot_shares = args->tot_shares,
		.tres_cnt = args->tres_cnt,
		.tres_names = args->tres_names,
	};

	xassert(args->magic == MAGIC_FOREACH_DUMP_ASSOC_SHARES);

	if ((args->rc = DUMP(ASSOC_SHARES_OBJ_WRAP, wrap, e, args->args)))
		return SLURM_ERROR;

	return SLURM_SUCCESS;
}
static int DUMP_FUNC(ASSOC_SHARES_OBJ_LIST)(const parser_t *const parser,
					    void *obj, data_t *dst,
					    args_t *args)
{
	shares_response_msg_t *resp = obj;
	foreach_dump_ASSOC_SHARES_OBJ_LIST_t fargs = {
		.magic = MAGIC_FOREACH_DUMP_ASSOC_SHARES,
		.rc = SLURM_SUCCESS,
		.args = args,
		.tot_shares = resp->tot_shares,
		.tres_cnt = resp->tres_cnt,
		.tres_names = resp->tres_names,
		.dst = dst,
	};

	data_set_list(dst);

	if (!resp->assoc_shares_list) {
		if (!slurm_conf.accounting_storage_type) {
			on_warn(DUMPING, parser->type, args, NULL, __func__,
				"Shares list is empty because slurm accounting storage is disabled.");
		}
		return SLURM_SUCCESS;
	}

	if (list_for_each(resp->assoc_shares_list,
			  _foreach_dump_ASSOC_SHARES_OBJ_LIST, &fargs) < 0)
		xassert(fargs.rc);

	return fargs.rc;
}

static int _dump_uint64_shares_tres_list(const assoc_shares_object_wrap_t *wrap,
					 const uint64_t *array, data_t *dst,
					 args_t *args)
{
	int rc;
	list_t *list = list_create(xfree_ptr);

	for (uint32_t i = 0; i < wrap->tres_cnt; i++) {
		SHARES_UINT64_TRES_t *tres = xmalloc(sizeof(*tres));
		list_append(list, tres);

		tres->name = wrap->tres_names[i];
		tres->value = array[i];
	}

	rc = DUMP(SHARES_UINT64_TRES_LIST, list, dst, args);

	FREE_NULL_LIST(list);
	return rc;
}

static int _dump_float128_shares_tres_list(
	const assoc_shares_object_wrap_t *wrap,
	const long double *array,
	data_t *dst, args_t *args)
{
	int rc;
	list_t *list = list_create(xfree_ptr);

	for (uint32_t i = 0; i < wrap->tres_cnt; i++) {
		SHARES_FLOAT128_TRES_t *tres = xmalloc(sizeof(*tres));
		list_append(list, tres);

		tres->name = wrap->tres_names[i];
		tres->value = array[i];
	}

	rc = DUMP(SHARES_FLOAT128_TRES_LIST, list, dst, args);

	FREE_NULL_LIST(list);
	return rc;
}

PARSE_DISABLED(ASSOC_SHARES_OBJ_WRAP_TRES_RUN_SECS)

static int DUMP_FUNC(ASSOC_SHARES_OBJ_WRAP_TRES_RUN_SECS)(
	const parser_t *const parser,
	void *obj,
	data_t *dst,
	args_t *args)
{
	assoc_shares_object_wrap_t *wrap = obj;
	return _dump_uint64_shares_tres_list(wrap, wrap->obj.tres_run_secs,
					     dst, args);
}

PARSE_DISABLED(ASSOC_SHARES_OBJ_WRAP_TRES_GRP_MINS)

static int DUMP_FUNC(ASSOC_SHARES_OBJ_WRAP_TRES_GRP_MINS)(
	const parser_t *const parser,
	void *obj,
	data_t *dst,
	args_t *args)
{
	assoc_shares_object_wrap_t *wrap = obj;
	return _dump_uint64_shares_tres_list(wrap, wrap->obj.tres_grp_mins,
					     dst, args);
}

PARSE_DISABLED(ASSOC_SHARES_OBJ_WRAP_TRES_USAGE_RAW)

static int DUMP_FUNC(ASSOC_SHARES_OBJ_WRAP_TRES_USAGE_RAW)(
	const parser_t *const parser,
	void *obj,
	data_t *dst,
	args_t *args)
{
	assoc_shares_object_wrap_t *wrap = obj;
	return _dump_float128_shares_tres_list(wrap, wrap->obj.usage_tres_raw,
					       dst, args);
}

static void *NEW_FUNC(SHARES_REQ_MSG)(void)
{
	shares_request_msg_t *req = xmalloc(sizeof(*req));
	req->acct_list = list_create(xfree_ptr);
	req->user_list = list_create(xfree_ptr);
	return req;
}

static void FREE_FUNC(SHARES_REQ_MSG)(void *ptr)
{
	shares_request_msg_t *msg = ptr;
	slurm_free_shares_request_msg(msg);
}

PARSE_DISABLED(RPC_ID)

static int DUMP_FUNC(RPC_ID)(const parser_t *const parser, void *obj,
			       data_t *dst, args_t *args)
{
	uint16_t *id = obj;

	data_set_string(dst, rpc_num2string(*id));

	return SLURM_SUCCESS;
}

PARSE_DISABLED(JOB_STATE_RESP_MSG)

static int DUMP_FUNC(JOB_STATE_RESP_MSG)(const parser_t *const parser,
					 void *obj, data_t *dst, args_t *args)
{
	int rc = SLURM_SUCCESS;
	job_state_response_msg_t *msg = obj;

	data_set_list(dst);

	for (int i = 0; !rc && (i < msg->jobs_count); i++) {
		const job_state_response_job_t *state = &msg->jobs[i];

		if (state->array_task_id_bitmap) {
			/*
			 * Explicitly expanding all array jobs to avoid
			 * forcing clients to parse 10_[22-91919]
			 */
			bitstr_t *bits = state->array_task_id_bitmap;
			job_state_response_job_t job = {
				.job_id = state->job_id,
				.array_job_id = state->array_job_id,
				.state = state->state,
			};

			for (bitoff_t bit = bit_ffs(bits);
			     !rc && ((bit = bit_ffs_from_bit(bits, bit)) >= 0);
			     bit++) {
				job.array_task_id = bit;
				rc = DUMP(JOB_STATE_RESP_JOB, job,
					  data_list_append(dst), args);
			}
		} else {
			rc = DUMP(JOB_STATE_RESP_JOB, msg->jobs[i],
				  data_list_append(dst), args);
		}
	}

	return rc;
}

PARSE_DISABLED(JOB_STATE_RESP_JOB_JOB_ID)

static int DUMP_FUNC(JOB_STATE_RESP_JOB_JOB_ID)(const parser_t *const parser,
						void *obj, data_t *dst,
						args_t *args)
{
	int rc = SLURM_SUCCESS;
	job_state_response_job_t *src = obj;

	if (!src->job_id) {
		if (!is_complex_mode(args))
			data_set_string(dst, "");
	} else if (src->het_job_id) {
		data_set_string_fmt(dst, "%u+%u", src->job_id,
				    (src->job_id - src->het_job_id));
	} else if (!src->array_job_id) {
		data_set_string_fmt(dst, "%u", src->job_id);
	} else if (src->array_task_id_bitmap) {
		data_t *dtasks = data_new();

		xassert(bit_ffs(src->array_task_id_bitmap) >= 0);

		if (!(rc = DUMP(BITSTR_PTR, src->array_task_id_bitmap, dtasks,
				args))) {
			xassert(data_get_string(dtasks)[0]);

			if (data_convert_type(dtasks, DATA_TYPE_STRING) !=
			    DATA_TYPE_STRING)
				on_error(
					DUMPING, parser->type, args,
					ESLURM_DATA_CONV_FAILED,
					"job_state_response_msg_t->array_task_id_bitmap",
					__func__,
					"Unable to convert BITSTR to string");
			else
				data_set_string_fmt(dst, "%u_[%s]", src->job_id,
						    data_get_string(dtasks));
		}

		FREE_NULL_DATA(dtasks);
	} else if ((src->array_task_id == NO_VAL) ||
		   (src->array_task_id == INFINITE)) {
		/* Treat both NO_VAL and INFINITE as request for whole job */
		data_set_string_fmt(dst, "%u_*", src->job_id);
	} else if (src->array_task_id < NO_VAL) {
		data_set_string_fmt(dst, "%u_%u", src->job_id,
				    src->array_task_id);
	} else {
		if (!is_complex_mode(args))
			data_set_string(dst, "");
		rc = on_error(DUMPING, parser->type, args,
			      ESLURM_DATA_CONV_FAILED,
			      "job_state_response_msg_t", __func__,
			      "Unable to dump JobId from job state");
	}

	xassert(data_get_string(dst)[0]);
	return rc;
}

static int PARSE_FUNC(EXT_SENSORS_DATA)(const parser_t *const parser, void *obj,
					data_t *src, args_t *args,
					data_t *parent_path)
{
	/* ext_sensors_data_t removed - no-op place holder */
	return SLURM_SUCCESS;
}

static int DUMP_FUNC(EXT_SENSORS_DATA)(const parser_t *const parser, void *obj,
				       data_t *dst, args_t *args)
{
	data_set_dict(dst);
	return SLURM_SUCCESS;
}

static int PARSE_FUNC(POWER_FLAGS)(const parser_t *const parser, void *obj,
				   data_t *src, args_t *args,
				   data_t *parent_path)
{
	/* SLURM_POWER_FLAGS_* removed - no-op place holder */
	return SLURM_SUCCESS;
}

static int DUMP_FUNC(POWER_FLAGS)(const parser_t *const parser, void *obj,
				  data_t *dst, args_t *args)
{
	data_set_list(dst);
	return SLURM_SUCCESS;
}

/*
 * The following struct arrays are not following the normal Slurm style but are
 * instead being treated as piles of data instead of code.
 */
// clang-format off
#define add_parser(stype, mtype, req, field, overload, path, desc)    \
{                                                                     \
	.magic = MAGIC_PARSER,                                        \
	.model = PARSER_MODEL_ARRAY_LINKED_FIELD,                     \
	.ptr_offset = offsetof(stype, field),                         \
	.field_name = XSTRINGIFY(field),                              \
	.field_name_overloads = overload,                             \
	.key = path,                                                  \
	.required = req,                                              \
	.type = DATA_PARSER_ ## mtype,                                \
	.type_string = XSTRINGIFY(DATA_PARSER_ ## mtype),             \
	.obj_desc = desc,                                             \
	.obj_type_string = XSTRINGIFY(stype),                         \
	.size = sizeof(((stype *) NULL)->field),                      \
	.needs = NEED_NONE,                                           \
}
#define add_parser_deprec(stype, mtype, req, field, overload, path, desc, \
			  deprec)                                         \
{                                                                         \
	.magic = MAGIC_PARSER,                                            \
	.model = PARSER_MODEL_ARRAY_LINKED_FIELD,                         \
	.ptr_offset = offsetof(stype, field),                             \
	.field_name = XSTRINGIFY(field),                                  \
	.field_name_overloads = overload,                                 \
	.key = path,                                                      \
	.required = req,                                                  \
	.type = DATA_PARSER_ ## mtype,                                    \
	.type_string = XSTRINGIFY(DATA_PARSER_ ## mtype),                 \
	.obj_desc = desc,                                                 \
	.obj_type_string = XSTRINGIFY(stype),                             \
	.size = sizeof(((stype *) NULL)->field),                          \
	.needs = NEED_NONE,                                               \
	.deprecated = deprec,                                             \
}
#define add_parser_removed(stype, mtype, req, path, desc, deprec)                                        \
{                                                                         \
	.magic = MAGIC_PARSER,                                            \
	.model = PARSER_MODEL_ARRAY_REMOVED_FIELD,                        \
	.ptr_offset = NO_VAL,                                             \
	.key = path,                                                      \
	.required = req,                                                  \
	.type = DATA_PARSER_ ## mtype,                                    \
	.type_string = XSTRINGIFY(DATA_PARSER_ ## mtype),                 \
	.obj_desc = desc,                                                 \
	.obj_type_string = XSTRINGIFY(stype),                             \
	.needs = NEED_NONE,                                               \
	.deprecated = deprec,                                             \
}
#define add_parser_skip(stype, field)                                 \
{                                                                     \
	.magic = MAGIC_PARSER,                                        \
	.model = PARSER_MODEL_ARRAY_SKIP_FIELD,                       \
	.ptr_offset = offsetof(stype, field),                         \
	.field_name = XSTRINGIFY(field),                              \
	.type = DATA_PARSER_TYPE_INVALID,                             \
	.obj_type_string = XSTRINGIFY(stype),                         \
	.size = sizeof(((stype *) NULL)->field),                      \
	.needs = NEED_NONE,                                           \
}
/*
 * Parser that needs the location of struct as
 * it will reference multiple fields at once.
 */
#define add_complex_parser(stype, mtype, req, path, desc)             \
{                                                                     \
	.magic = MAGIC_PARSER,                                        \
	.model = PARSER_MODEL_ARRAY_LINKED_FIELD,                     \
	.ptr_offset = NO_VAL,                                         \
	.key = path,                                                  \
	.required = req,                                              \
	.type = DATA_PARSER_ ## mtype,                                \
	.type_string = XSTRINGIFY(DATA_PARSER_ ## mtype),             \
	.obj_desc = desc,                                             \
	.obj_type_string = XSTRINGIFY(stype),                         \
	.size = NO_VAL,                                               \
	.needs = NEED_NONE                                            \
}
#define add_parse_bit_eflag_array(stype, mtype, field, desc)          \
{                                                                     \
	.magic = MAGIC_PARSER,                                        \
	.model = PARSER_MODEL_ARRAY_LINKED_EXPLODED_FLAG_ARRAY_FIELD, \
	.ptr_offset = offsetof(stype, field),                         \
	.field_name = XSTRINGIFY(field),                              \
	.key = NULL,                                                  \
	.required = false,                                            \
	.type = DATA_PARSER_ ## mtype,                                \
	.type_string = XSTRINGIFY(DATA_PARSER_ ## mtype),             \
	.obj_desc = desc,                                             \
	.obj_type_string = XSTRINGIFY(stype),                         \
	.size = sizeof(((stype *) NULL)->field),                      \
	.needs = NEED_NONE,                                           \
}
#define add_parse_bit_flag_array(stype, mtype, req, field, path, desc)\
{                                                                     \
	.magic = MAGIC_PARSER,                                        \
	.model = PARSER_MODEL_ARRAY_LINKED_FIELD,                     \
	.ptr_offset = offsetof(stype, field),                         \
	.field_name = XSTRINGIFY(field),                              \
	.key = path,                                                  \
	.required = req,                                              \
	.type = DATA_PARSER_ ## mtype,                                \
	.type_string = XSTRINGIFY(DATA_PARSER_ ## mtype),             \
	.obj_desc = desc,                                             \
	.obj_type_string = XSTRINGIFY(stype),                         \
	.size = sizeof(((stype *) NULL)->field),                      \
	.needs = NEED_NONE,                                           \
}
#define add_parse_bit_flag_string(stype, mtype, req, field, path, desc) \
{                                                                       \
	.magic = MAGIC_PARSER,                                          \
	.model = PARSER_MODEL_ARRAY_LINKED_FIELD,                       \
	.ptr_offset = offsetof(stype, field),                           \
	.field_name = XSTRINGIFY(field),                                \
	.key = path,                                                    \
	.required = req,                                                \
	.type = DATA_PARSER_ ## mtype,                                  \
	.type_string = XSTRINGIFY(DATA_PARSER_ ## mtype),               \
	.obj_desc = desc,                                               \
	.obj_type_string = XSTRINGIFY(stype),                           \
	.size = sizeof(((stype *) NULL)->field),                        \
	.single_flag = true,                                            \
	.needs = NEED_NONE,                                             \
}
#define add_flag_bit(flag_value, flag_string)                         \
	add_flag_masked_bit(flag_value, INFINITE64, flag_string)
#define add_flag_masked_bit(flag_value, flag_mask, flag_string)       \
	add_flag_bit_entry(FLAG_BIT_TYPE_BIT, XSTRINGIFY(flag_value), \
			   flag_value, flag_mask,                     \
			   XSTRINGIFY(flag_mask), flag_string, false, \
			   NULL)
#define add_flag_equal(flag_value, flag_mask, flag_string)            \
	add_flag_bit_entry(FLAG_BIT_TYPE_EQUAL,                       \
			   XSTRINGIFY(flag_value),                    \
			   flag_value, flag_mask,                     \
			   XSTRINGIFY(flag_mask), flag_string,        \
			   false, NULL)
#define add_flag_bit_entry(flag_type, flag_value_string, flag_value,  \
			   flag_mask, flag_mask_string, flag_string,  \
			   hidden_flag, desc_str)                     \
{                                                                     \
	.magic = MAGIC_FLAG_BIT,                                      \
	.type = flag_type,                                            \
	.value = flag_value,                                          \
	.mask = flag_mask,                                            \
	.mask_size = sizeof(flag_mask),                               \
	.mask_name = flag_mask_string,                                \
	.name = flag_string,                                          \
	.flag_name = flag_value_string,                               \
	.flag_size = sizeof(flag_value),                              \
	.hidden = hidden_flag,                                        \
	.description = desc_str,                                      \
}

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_assoc_rec_t, mtype, false, field, 0, path, desc)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_assoc_rec_t, mtype, true, field, 0, path, desc)
static const parser_t PARSER_ARRAY(ASSOC_SHORT)[] = {
	/* Identifiers required for any given association */
	add_parse(STRING, acct, "account", "Association account (if assigned)"),
	add_parse(STRING, cluster, "cluster", "Association cluster (if assigned)"),
	add_parse(STRING, partition, "partition", "Association partition (if assigned)"),
	add_parse_req(STRING, user, "user", "Assocation user (if assigned)"),
	add_parse(UINT32, id, "id", "Numeric Association ID (if known)"),
};
#undef add_parse
#undef add_parse_req

static const flag_bit_t PARSER_FLAG_ARRAY(ASSOC_FLAGS)[] = {
	add_flag_bit(ASSOC_FLAG_DELETED, "DELETED"),
};

#define add_skip(field) \
	add_parser_skip(slurmdb_assoc_rec_t, field)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_assoc_rec_t, mtype, false, field, 0, path, desc)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_assoc_rec_t, mtype, true, field, 0, path, desc)
/* should mirror the structure of slurmdb_assoc_rec_t */
static const parser_t PARSER_ARRAY(ASSOC_REC_SET)[] = {
	add_skip(accounting_list),
	add_skip(acct),
	add_skip(assoc_next),
	add_skip(assoc_next_id),
	add_skip(bf_usage),
	add_skip(cluster),
	add_parse(STRING, comment, "comment", "Comment for the association"),
	add_parse(QOS_ID, def_qos_id, "defaultqos", "Which QOS id is this association default"),
	add_skip(flags),
	add_parse(UINT32_NO_VAL, grp_jobs, "grpjobs", "Max number of jobs the underlying group of associations can run at one time"),
	add_parse(UINT32_NO_VAL, grp_jobs_accrue, "grpjobsaccrue", "Max number of jobs the underlying group of associations can have accruing priority at one time"),
	add_parse(UINT32_NO_VAL, grp_submit_jobs, "grpsubmitjobs", "Max number of jobs the underlying group of associations can submit at one time"),
	add_parse(TRES_STR, grp_tres, "grptres", NULL),
	add_skip(grp_tres_ctld),
	add_parse(TRES_STR, grp_tres_mins, "grptresmins", "Max number of cpu minutes the underlying group of associations can run for"),
	add_skip(grp_tres_mins_ctld),
	add_parse(TRES_STR, grp_tres_run_mins, "grptresrunmins", "Max number of cpu minutes the underlying group of associations can having running at one time"),
	add_skip(grp_tres_run_mins_ctld),
	add_parse(UINT32_NO_VAL, grp_wall, "grpwall", "Total time in minutes the underlying group of associations can run for"),
	add_skip(id),
	add_skip(is_def),
	add_skip(leaf_usage),
	add_skip(lft),
	add_skip(lineage),
	add_parse(UINT32_NO_VAL, max_jobs, "maxjobs", "Max number of jobs this association can run at one time"),
	add_parse(UINT32_NO_VAL, max_jobs_accrue, "maxjobsaccrue", "Max number of jobs this association can have accruing priority time"),
	add_parse(UINT32_NO_VAL, max_submit_jobs, "maxsubmitjobs", "Max number of jobs that can be submitted by association"),
	add_parse(TRES_STR, max_tres_mins_pj, "maxtresminsperjob", "Max number of cpu minutes this association can have per job"),
	add_skip(max_tres_mins_ctld),
	add_parse(TRES_STR, max_tres_run_mins, "maxtresrunmins", "Max number of cpu minutes this association can having running at one time"),
	add_skip(max_tres_run_mins_ctld),
	add_parse(TRES_STR, max_tres_pj, "maxtresperjob", "Max number of cpus this association can allocate per job"),
	add_skip(max_tres_ctld),
	add_parse(TRES_STR, max_tres_pn, "maxtrespernode", "Max number of TRES this association can allocate per node"),
	add_skip(max_tres_pn_ctld),
	add_parse(UINT32_NO_VAL, max_wall_pj, "maxwalldurationperjob", "Longest time this association can run a job"),
	add_parse(UINT32_NO_VAL, min_prio_thresh, "minpriothresh", "Don't reserve resources for pending jobs unless they have a priority equal to or higher than this"),
	add_parse(STRING, parent_acct, "parent", "Name of parent account"),
	add_skip(parent_id),
	add_skip(partition),
	add_parse(UINT32_NO_VAL, priority, "priority", "Association priority"),
	add_parse(QOS_STRING_ID_LIST, qos_list, "qoslevel", "Default QoS' that jobs are able to run at for this association"),
	add_skip(rgt),
	add_parse(UINT32, shares_raw, "fairshare", "Number of shares allocated to this association"),
	/* slurmdbd should never set uid - it should always be zero */
	add_skip(uid),
	/*
	 * Used by SLURMDB_REMOVE_ASSOC_USAGE when modifying/updating
	 * account/user association. And this parser is intended for additions
	 * only.
	 */
	add_skip(usage),
	add_skip(user),
	add_skip(user_rec),
};
#undef add_parse
#undef add_parse_req
#undef add_skip

#define add_skip(field) \
	add_parser_skip(slurmdb_assoc_rec_t, field)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_assoc_rec_t, mtype, false, field, 0, path, desc)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_assoc_rec_t, mtype, true, field, 0, path, desc)
/* should mirror the structure of slurmdb_assoc_rec_t */
static const parser_t PARSER_ARRAY(ASSOC)[] = {
	add_parse(ACCOUNTING_LIST, accounting_list, "accounting", "Usage accounting"),
	add_parse(STRING, acct, "account", NULL),
	add_skip(assoc_next),
	add_skip(assoc_next_id),
	add_skip(bf_usage),
	add_parse(STRING, cluster, "cluster", NULL),
	add_parse(STRING, comment, "comment", "comment for the association"),
	add_parse(QOS_ID, def_qos_id, "default/qos", NULL),
	add_parse_bit_flag_array(slurmdb_assoc_rec_t, ASSOC_FLAGS, false, flags, "flags", NULL),
	add_parse(UINT32_NO_VAL, grp_jobs, "max/jobs/per/count", NULL),
	add_parse(UINT32_NO_VAL, grp_jobs_accrue, "max/jobs/per/accruing", NULL),
	add_parse(UINT32_NO_VAL, grp_submit_jobs, "max/jobs/per/submitted", NULL),
	add_parse(TRES_STR, grp_tres, "max/tres/total", NULL),
	add_skip(grp_tres_ctld),
	add_parse(TRES_STR, grp_tres_mins, "max/tres/group/minutes", NULL),
	add_skip(grp_tres_mins_ctld),
	add_parse(TRES_STR, grp_tres_run_mins, "max/tres/group/active", NULL),
	add_skip(grp_tres_run_mins_ctld),
	add_parse(TRES_STR, max_tres_run_mins, "max/tres/minutes/total", NULL),
	add_parse(UINT32_NO_VAL, grp_wall, "max/per/account/wall_clock", NULL),
	add_complex_parser(slurmdb_assoc_rec_t, ASSOC_ID, false, "id", NULL),
	add_parse(BOOL16, is_def, "is_default", NULL),
	add_skip(leaf_usage),
	add_skip(lft),
	add_parse(STRING, lineage, "lineage", "Complete path up the hierarchy to the root association"),
	add_parse(UINT32_NO_VAL, max_jobs, "max/jobs/active", NULL),
	add_parse(UINT32_NO_VAL, max_jobs_accrue, "max/jobs/accruing", NULL),
	add_parse(UINT32_NO_VAL, max_submit_jobs, "max/jobs/total", NULL),
	add_parse(TRES_STR, max_tres_mins_pj, "max/tres/minutes/per/job", NULL),
	add_skip(max_tres_mins_ctld),
	add_parse(TRES_STR, max_tres_pj, "max/tres/per/job", NULL),
	add_skip(max_tres_ctld),
	add_parse(TRES_STR, max_tres_pn, "max/tres/per/node", NULL),
	add_skip(max_tres_pn_ctld),
	add_parse(UINT32_NO_VAL, max_wall_pj, "max/jobs/per/wall_clock", NULL),
	add_parse(UINT32_NO_VAL, min_prio_thresh, "min/priority_threshold", NULL),
	add_parse(STRING, parent_acct, "parent_account", NULL),
	add_skip(parent_id),
	add_parse(STRING, partition, "partition", NULL),
	add_parse(UINT32_NO_VAL, priority, "priority", NULL),
	add_parse(QOS_STRING_ID_LIST, qos_list, "qos", NULL),
	add_skip(rgt),
	add_parse(UINT32, shares_raw, "shares_raw", NULL),
	/* slurmdbd should never set uid - it should always be zero */
	add_skip(uid),
	add_skip(usage),
	add_parse_req(STRING, user, "user", NULL),
	add_skip(user_rec),
};
#undef add_parse
#undef add_parse_req
#undef add_skip

static const flag_bit_t PARSER_FLAG_ARRAY(ADMIN_LVL)[] = {
	add_flag_equal(SLURMDB_ADMIN_NOTSET, INFINITE16, "Not Set"),
	add_flag_equal(SLURMDB_ADMIN_NONE, INFINITE16, "None"),
	add_flag_equal(SLURMDB_ADMIN_OPERATOR, INFINITE16, "Operator"),
	add_flag_equal(SLURMDB_ADMIN_SUPER_USER, INFINITE16, "Administrator"),
};

static const flag_bit_t PARSER_FLAG_ARRAY(USER_FLAGS)[] = {
	add_flag_equal(SLURMDB_USER_FLAG_NONE, INFINITE64, "NONE"),
	add_flag_bit(SLURMDB_USER_FLAG_DELETED, "DELETED"),
};

#define add_skip(field) \
	add_parser_skip(slurmdb_user_rec_t, field)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_user_rec_t, mtype, false, field, 0, path, desc)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_user_rec_t, mtype, true, field, 0, path, desc)
/* should mirror the structure of slurmdb_user_rec_t */
static const parser_t PARSER_ARRAY(USER_SHORT)[] = {
	add_parse(ADMIN_LVL, admin_level, "adminlevel", "Admin level of user.  Valid levels are None, Operator, and Admin."),
	add_skip(assoc_list),
	add_skip(bf_usage),
	add_parse(STRING, default_acct, "defaultaccount", "Identify the default bank account name to be used for a job if none is specified at submission time."),
	add_parse(STRING, default_wckey, "defaultwckey", "Identify the default Workload Characterization Key."),
	add_skip(flags),
	add_skip(name),
	add_skip(old_name),
	/* uid should always be 0 */
	add_skip(uid),
	add_skip(wckey_list),
};
#undef add_parse
#undef add_parse_req
#undef add_skip

#define add_skip(field) \
	add_parser_skip(slurmdb_user_rec_t, field)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_user_rec_t, mtype, false, field, 0, path, desc)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_user_rec_t, mtype, true, field, 0, path, desc)
/* should mirror the structure of slurmdb_user_rec_t */
static const parser_t PARSER_ARRAY(USER)[] = {
	add_parse(ADMIN_LVL, admin_level, "administrator_level", NULL),
	add_parse(ASSOC_SHORT_LIST, assoc_list, "associations", NULL),
	add_parse(COORD_LIST, coord_accts, "coordinators", NULL),
	add_parse(STRING, default_acct, "default/account", NULL),
	add_parse(STRING, default_wckey, "default/wckey", NULL),
	add_parse_bit_flag_array(slurmdb_user_rec_t, USER_FLAGS, false, flags, "flags", NULL),
	add_parse_req(STRING, name, "name", NULL),
	add_parse(STRING, old_name, "old_name", NULL),
	/* uid should always be 0 */
	add_skip(uid),
	add_parse(WCKEY_LIST, wckey_list, "wckeys", NULL),
};
#undef add_parse
#undef add_parse_req
#undef add_skip

static const flag_bit_t PARSER_FLAG_ARRAY(SLURMDB_JOB_FLAGS)[] = {
	add_flag_equal(SLURMDB_JOB_FLAG_NONE, INFINITE64, "NONE"),
	add_flag_equal(SLURMDB_JOB_CLEAR_SCHED, INFINITE64, "CLEAR_SCHEDULING"),
	add_flag_bit(SLURMDB_JOB_FLAG_NOTSET, "NOT_SET"),
	add_flag_bit(SLURMDB_JOB_FLAG_SUBMIT, "STARTED_ON_SUBMIT"),
	add_flag_bit(SLURMDB_JOB_FLAG_SCHED, "STARTED_ON_SCHEDULE"),
	add_flag_bit(SLURMDB_JOB_FLAG_BACKFILL, "STARTED_ON_BACKFILL"),
	add_flag_bit(SLURMDB_JOB_FLAG_START_R, "START_RECEIVED"),
};

#define add_skip(field) \
	add_parser_skip(slurmdb_instance_rec_t, field)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_instance_rec_t, mtype, false, field, 0, path, desc)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_instance_rec_t, mtype, true, field, 0, path, desc)
/* should mirror the structure of slurmdb_instance_rec_t */
static const parser_t PARSER_ARRAY(INSTANCE)[] ={
	add_parse(STRING, cluster, "cluster", NULL),
	add_parse(STRING, extra, "extra", NULL),
	add_parse(STRING, instance_id, "instance_id", NULL),
	add_parse(STRING, instance_type, "instance_type", NULL),
	add_parse(STRING, node_name, "node_name", NULL),
	add_parse(TIMESTAMP, time_end, "time/time_end", NULL),
	add_parse(TIMESTAMP, time_start, "time/time_start", NULL),
};
#undef add_parse
#undef add_parse_req
#undef add_skip


#define add_skip(field) \
	add_parser_skip(slurmdb_job_rec_t, field)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_job_rec_t, mtype, false, field, 0, path, desc)
#define add_parse_overload(mtype, field, overloads, path, desc) \
	add_parser(slurmdb_job_rec_t, mtype, false, field, overloads, path, desc)
/* should mirror the structure of slurmdb_job_rec_t  */
static const parser_t PARSER_ARRAY(JOB)[] = {
	add_parse(STRING, account, "account", NULL),
	add_parse(STRING, admin_comment, "comment/administrator", NULL),
	add_parse(UINT32, alloc_nodes, "allocation_nodes", NULL),
	add_parse(UINT32, array_job_id, "array/job_id", NULL),
	add_parse(UINT32, array_max_tasks, "array/limits/max/running/tasks", NULL),
	add_parse(UINT32_NO_VAL, array_task_id, "array/task_id", NULL),
	add_parse(STRING, array_task_str, "array/task", NULL),
	add_complex_parser(slurmdb_job_rec_t, JOB_ASSOC_ID, false, "association", NULL),
	add_parse(STRING, blockid, "block", NULL),
	add_parse(STRING, cluster, "cluster", NULL),
	add_parse(STRING, constraints, "constraints", NULL),
	add_parse(STRING, container, "container", NULL),
	add_skip(db_index),
	add_parse(PROCESS_EXIT_CODE, derived_ec, "derived_exit_code", NULL),
	add_parse(STRING, derived_es, "comment/job", NULL),
	add_parse(UINT32, elapsed, "time/elapsed", NULL),
	add_parse(TIMESTAMP, eligible, "time/eligible", NULL),
	add_parse(TIMESTAMP, end, "time/end", NULL),
	add_skip(env),
	add_parse(PROCESS_EXIT_CODE, exitcode, "exit_code", NULL),
	add_parse(STRING, extra, "extra", NULL),
	add_parse(STRING, failed_node, "failed_node", NULL),
	add_parse_bit_flag_array(slurmdb_job_rec_t, SLURMDB_JOB_FLAGS, false, flags, "flags", NULL),
	add_skip(first_step_ptr),
	add_parse(GROUP_ID, gid, "group", NULL),
	add_parse(UINT32, het_job_id, "het/job_id", NULL),
	add_parse(UINT32_NO_VAL, het_job_offset, "het/job_offset", NULL),
	add_parse(UINT32, jobid, "job_id", NULL),
	add_parse(STRING, jobname, "name", NULL),
	add_skip(lft),
	add_parse(STRING, licenses, "licenses", NULL),
	add_parse(STRING, mcs_label, "mcs/label", NULL),
	add_parse(STRING, nodes, "nodes", NULL),
	add_parse(STRING, partition, "partition", NULL),
	add_parse_overload(HOLD, priority, 1, "hold", "Hold (true) or release (false) job"),
	add_parse_overload(UINT32_NO_VAL, priority, 1, "priority", "Request specific job priority"),
	add_parse(QOS_ID, qosid, "qos", NULL),
	add_parse(UINT32, req_cpus, "required/CPUs", NULL),
	add_parse_overload(MEM_PER_CPUS, req_mem, 1, "required/memory_per_cpu", NULL),
	add_parse_overload(MEM_PER_NODE, req_mem, 1, "required/memory_per_node", NULL),
	add_parse(USER_ID, requid, "kill_request_user", NULL),
	add_parse(UINT32, resvid, "reservation/id", NULL),
	add_parse(STRING, resv_name, "reservation/name", NULL),
	add_complex_parser(slurmdb_job_rec_t, JOB_PLANNED_TIME, false, "time/planned", "Time in seconds required to start job after becoming eligible to run"),
	add_parse(STRING, script, "script", NULL),
	add_skip(show_full),
	add_parse(TIMESTAMP, start, "time/start", NULL),
	add_parse_bit_flag_array(slurmdb_job_rec_t, JOB_STATE, false, state, "state/current", NULL),
	add_parse(JOB_REASON, state_reason_prev, "state/reason", NULL),
	add_parse(STEP_LIST, steps, "steps", NULL),
	add_parse(TIMESTAMP, submit, "time/submission", NULL),
	add_parse(STRING, submit_line, "submit_line", NULL),
	add_parse(UINT32, suspended, "time/suspended", NULL),
	add_parse(STRING, system_comment, "comment/system", NULL),
	add_parse(UINT64, sys_cpu_sec, "time/system/seconds", NULL),
	add_parse(UINT64, sys_cpu_usec, "time/system/microseconds", NULL),
	add_parse(UINT32_NO_VAL, timelimit, "time/limit", NULL),
	add_parse(UINT64, tot_cpu_sec, "time/total/seconds", NULL),
	add_parse(UINT64, tot_cpu_usec, "time/total/microseconds", NULL),
	add_parse(TRES_STR, tres_alloc_str, "tres/allocated", NULL),
	add_parse(TRES_STR, tres_req_str, "tres/requested", NULL),
	add_skip(uid), /* dup with complex parser JOB_USER  below */
	add_parse(STRING, used_gres, "used_gres", NULL),
	add_skip(user), /* dup with complex parser JOB_USER below */
	/* parse uid or user depending on which is available */
	add_complex_parser(slurmdb_job_rec_t, JOB_USER, false, "user", NULL),
	add_parse(UINT64, user_cpu_sec, "time/user/seconds", NULL),
	add_parse(UINT64, user_cpu_usec, "time/user/microseconds", NULL),
	add_parse(WCKEY_TAG, wckey, "wckey", NULL),
	add_skip(wckeyid),
	add_parse(STRING, work_dir, "working_directory", NULL),
};
#undef add_parse
#undef add_skip
#undef add_parse_overload

static const flag_bit_t PARSER_FLAG_ARRAY(ACCOUNT_FLAGS)[] = {
	add_flag_bit(SLURMDB_ACCT_FLAG_DELETED, "DELETED"),
};

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_account_rec_t, mtype, false, field, 0, path, desc)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_account_rec_t, mtype, true, field, 0, path, desc)
/* should mirror the structure of slurmdb_account_rec_t */
static const parser_t PARSER_ARRAY(ACCOUNT)[] = {
	add_parse(ASSOC_SHORT_LIST, assoc_list, "associations", NULL),
	add_parse(COORD_LIST, coordinators, "coordinators", NULL),
	add_parse_req(STRING, description, "description", NULL),
	add_parse_req(STRING, name, "name", NULL),
	add_parse_req(STRING, organization, "organization", NULL),
	add_parse_bit_flag_array(slurmdb_account_rec_t, ACCOUNT_FLAGS, false, flags, "flags", NULL),
};
#undef add_parse
#undef add_parse_req

#define add_skip(field) \
	add_parser_skip(slurmdb_account_rec_t, field)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_account_rec_t, mtype, false, field, 0, path, desc)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_account_rec_t, mtype, true, field, 0, path, desc)
/*
 * Should mirror the structure of slurmdb_account_rec_t.
 * Only parse same fields as in sacctmgr_add_account().
 */
static const parser_t PARSER_ARRAY(ACCOUNT_SHORT)[] = {
	add_skip(assoc_list),
	add_skip(coordinators),
	add_parse(STRING, description, "description", "An arbitrary string describing an account"),
	add_skip(name),
	add_parse(STRING, organization, "organization", "Organization to which the account belongs"),
	add_skip(flags),
};
#undef add_parse
#undef add_parse_req
#undef add_skip

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_accounting_rec_t, mtype, false, field, 0, path, desc)
/* should mirror the structure of slurmdb_accounting_rec_t */
static const parser_t PARSER_ARRAY(ACCOUNTING)[] = {
	add_parse(UINT64, alloc_secs, "allocated/seconds", NULL),
	add_parse(UINT32, id, "id", NULL),
	add_parse(TIMESTAMP, period_start, "start", NULL),
	add_parse(TRES, tres_rec, "TRES", NULL),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_coord_rec_t, mtype, false, field, 0, path, desc)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_coord_rec_t, mtype, true, field, 0, path, desc)
/* should mirror the structure of slurmdb_coord_rec_t  */
static const parser_t PARSER_ARRAY(COORD)[] = {
	add_parse_req(STRING, name, "name", NULL),
	add_parse(BOOL16, direct, "direct", NULL),
};
#undef add_parse
#undef add_parse_req

static const flag_bit_t PARSER_FLAG_ARRAY(WCKEY_FLAGS)[] = {
	add_flag_bit(SLURMDB_WCKEY_FLAG_DELETED, "DELETED"),
};

#define add_skip(field) \
	add_parser_skip(slurmdb_wckey_rec_t, field)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_wckey_rec_t, mtype, false, field, 0, path, desc)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_wckey_rec_t, mtype, true, field, 0, path, desc)
/* should mirror the structure of slurmdb_wckey_rec_t */
static const parser_t PARSER_ARRAY(WCKEY)[] = {
	add_parse(ACCOUNTING_LIST, accounting_list, "accounting", NULL),
	add_parse_req(STRING, cluster, "cluster", NULL),
	add_parse(UINT32, id, "id", NULL),
	add_parse_req(STRING, name, "name", NULL),
	add_parse_req(STRING, user, "user", NULL),
	add_skip(uid),
	add_parse_bit_flag_array(slurmdb_wckey_rec_t, WCKEY_FLAGS, false, flags, "flags", NULL),
};
#undef add_parse
#undef add_parse_req
#undef add_skip

#define add_skip(field) \
	add_parser_skip(slurmdb_tres_rec_t, field)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_tres_rec_t, mtype, false, field, 0, path, desc)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_tres_rec_t, mtype, true, field, 0, path, desc)
/* should mirror the structure of slurmdb_tres_rec_t  */
static const parser_t PARSER_ARRAY(TRES)[] = {
	add_skip(alloc_secs), /* sreport func */
	add_skip(rec_count), /* not packed */
	add_parse_req(STRING, type, "type", NULL),
	add_parse(STRING, name, "name", NULL),
	add_parse(UINT32, id, "id", NULL),
	add_parse(INT64, count, "count", NULL),
};
#undef add_parse
#undef add_parse_req
#undef add_skip

static const flag_bit_t PARSER_FLAG_ARRAY(QOS_FLAGS)[] = {
	add_flag_masked_bit(QOS_FLAG_NOTSET, ~QOS_FLAG_BASE, "NOT_SET"),
	add_flag_masked_bit(QOS_FLAG_ADD, ~QOS_FLAG_BASE, "ADD"),
	add_flag_masked_bit(QOS_FLAG_REMOVE, ~QOS_FLAG_BASE, "REMOVE"),
	add_flag_masked_bit(QOS_FLAG_PART_MIN_NODE, QOS_FLAG_BASE, "PARTITION_MINIMUM_NODE"),
	add_flag_masked_bit(QOS_FLAG_PART_MAX_NODE, QOS_FLAG_BASE, "PARTITION_MAXIMUM_NODE"),
	add_flag_masked_bit(QOS_FLAG_PART_TIME_LIMIT, QOS_FLAG_BASE, "PARTITION_TIME_LIMIT"),
	add_flag_masked_bit(QOS_FLAG_ENFORCE_USAGE_THRES, QOS_FLAG_BASE, "ENFORCE_USAGE_THRESHOLD"),
	add_flag_masked_bit(QOS_FLAG_NO_RESERVE, QOS_FLAG_BASE, "NO_RESERVE"),
	add_flag_masked_bit(QOS_FLAG_REQ_RESV, QOS_FLAG_BASE, "REQUIRED_RESERVATION"),
	add_flag_masked_bit(QOS_FLAG_DENY_LIMIT, QOS_FLAG_BASE, "DENY_LIMIT"),
	add_flag_masked_bit(QOS_FLAG_OVER_PART_QOS, QOS_FLAG_BASE, "OVERRIDE_PARTITION_QOS"),
	add_flag_masked_bit(QOS_FLAG_NO_DECAY, QOS_FLAG_BASE, "NO_DECAY"),
	add_flag_masked_bit(QOS_FLAG_USAGE_FACTOR_SAFE, QOS_FLAG_BASE, "USAGE_FACTOR_SAFE"),
	add_flag_masked_bit(QOS_FLAG_RELATIVE, QOS_FLAG_BASE, "RELATIVE"),
};

static const flag_bit_t PARSER_FLAG_ARRAY(QOS_PREEMPT_MODES)[] = {
	add_flag_equal(PREEMPT_MODE_OFF, INFINITE64, "DISABLED"),
	add_flag_bit(PREEMPT_MODE_SUSPEND, "SUSPEND"),
	add_flag_bit(PREEMPT_MODE_REQUEUE, "REQUEUE"),
	add_flag_bit(PREEMPT_MODE_CANCEL, "CANCEL"),
	add_flag_bit(PREEMPT_MODE_GANG, "GANG"),
};

#define add_skip(field) \
	add_parser_skip(slurmdb_qos_rec_t, field)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_qos_rec_t, mtype, false, field, 0, path, desc)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_qos_rec_t, mtype, true, field, 0, path, desc)
/* should mirror the structure of slurmdb_qos_rec_t */
static const parser_t PARSER_ARRAY(QOS)[] = {
	add_parse(STRING, description, "description", NULL),
	add_parse_bit_flag_array(slurmdb_qos_rec_t, QOS_FLAGS, false, flags, "flags", NULL),
	add_parse(UINT32, id, "id", NULL),
	add_parse(UINT32, grace_time, "limits/grace_time", NULL),
	add_parse(UINT32_NO_VAL, grp_jobs_accrue, "limits/max/active_jobs/accruing", NULL),
	add_parse(UINT32_NO_VAL, grp_jobs, "limits/max/active_jobs/count", NULL),
	add_parse(TRES_STR, grp_tres, "limits/max/tres/total", NULL),
	add_skip(grp_tres_ctld), /* not packed */
	add_parse(TRES_STR, grp_tres_run_mins, "limits/max/tres/minutes/per/qos", NULL),
	add_skip(grp_tres_run_mins_ctld), /* not packed */
	add_parse(STRING, name, "name", NULL),
	add_parse(UINT32_NO_VAL, grp_wall, "limits/max/wall_clock/per/qos", NULL),
	add_parse(FLOAT64_NO_VAL, limit_factor, "limits/factor", NULL),
	add_parse(UINT32_NO_VAL, max_jobs_pa, "limits/max/jobs/active_jobs/per/account", NULL),
	add_parse(UINT32_NO_VAL, max_jobs_pu, "limits/max/jobs/active_jobs/per/user", NULL),
	add_parse(UINT32_NO_VAL, max_jobs_accrue_pa, "limits/max/accruing/per/account", NULL),
	add_parse(UINT32_NO_VAL, max_jobs_accrue_pu, "limits/max/accruing/per/user", NULL),
	add_parse(UINT32_NO_VAL, max_submit_jobs_pa, "limits/max/jobs/per/account", NULL),
	add_parse(UINT32_NO_VAL, max_submit_jobs_pu, "limits/max/jobs/per/user", NULL),
	add_parse(TRES_STR, max_tres_mins_pj, "limits/max/tres/minutes/per/job", NULL),
	add_skip(max_tres_mins_pj_ctld), /* not packed */
	add_parse(TRES_STR, max_tres_pa, "limits/max/tres/per/account", NULL),
	add_skip(max_tres_pa_ctld), /* not packed */
	add_parse(TRES_STR, max_tres_pj, "limits/max/tres/per/job", NULL),
	add_skip(max_tres_pj_ctld), /* not packed */
	add_parse(TRES_STR, max_tres_pn, "limits/max/tres/per/node", NULL),
	add_skip(max_tres_pn_ctld), /* not packed */
	add_parse(TRES_STR, max_tres_pu, "limits/max/tres/per/user", NULL),
	add_skip(max_tres_pu_ctld), /* not packed */
	add_parse(TRES_STR, max_tres_run_mins_pa, "limits/max/tres/minutes/per/account", NULL),
	add_skip(max_tres_run_mins_pa_ctld), /* not packed */
	add_parse(TRES_STR, max_tres_run_mins_pu, "limits/max/tres/minutes/per/user", NULL),
	add_skip(max_tres_run_mins_pu_ctld), /* not packed */
	add_parse(UINT32_NO_VAL, max_wall_pj, "limits/max/wall_clock/per/job", NULL),
	add_parse(UINT32_NO_VAL, min_prio_thresh, "limits/min/priority_threshold", NULL),
	add_parse(TRES_STR, min_tres_pj, "limits/min/tres/per/job", NULL),
	add_skip(min_tres_pj_ctld), /* not packed */
	add_complex_parser(slurmdb_qos_rec_t, QOS_PREEMPT_LIST, false, "preempt/list", NULL),
	add_parse_bit_flag_array(slurmdb_qos_rec_t, QOS_PREEMPT_MODES, false, preempt_mode, "preempt/mode", NULL),
	add_parse(UINT32_NO_VAL, preempt_exempt_time, "preempt/exempt_time", NULL),
	add_parse(UINT32_NO_VAL, priority, "priority", NULL),
	add_skip(usage), /* not packed */
	add_parse(FLOAT64_NO_VAL, usage_factor, "usage_factor", NULL),
	add_parse(FLOAT64_NO_VAL, usage_thres, "usage_threshold", NULL),
	add_skip(blocked_until), /* not packed */
};
#undef add_parse
#undef add_parse_req
#undef add_skip
#undef add_skip_flag

#define add_skip(field) \
	add_parser_skip(slurmdb_step_rec_t, field)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_step_rec_t, mtype, false, field, 0, path, desc)
/* should mirror the structure of slurmdb_step_rec_t */
static const parser_t PARSER_ARRAY(STEP)[] = {
	add_parse(UINT32, elapsed, "time/elapsed", NULL),
	add_parse(TIMESTAMP_NO_VAL, end, "time/end", NULL),
	add_parse(PROCESS_EXIT_CODE, exitcode, "exit_code", NULL),
	add_skip(job_ptr), /* redundant here */
	add_parse(UINT32, nnodes, "nodes/count", NULL),
	add_parse(STRING, nodes, "nodes/range", NULL),
	add_parse(UINT32, ntasks, "tasks/count", NULL),
	add_parse(STRING, pid_str, "pid", NULL),
	add_parse(UINT32_NO_VAL, req_cpufreq_min, "CPU/requested_frequency/min", NULL),
	add_parse(UINT32_NO_VAL, req_cpufreq_max, "CPU/requested_frequency/max", NULL),
	add_parse(CPU_FREQ_FLAGS, req_cpufreq_gov, "CPU/governor", NULL),
	add_parse(USER_ID, requid, "kill_request_user", NULL),
	add_parse(TIMESTAMP_NO_VAL, start, "time/start", NULL),
	add_parse_bit_flag_array(slurmdb_step_rec_t, JOB_STATE, false, state, "state", NULL),
	add_parse(UINT64, stats.act_cpufreq, "statistics/CPU/actual_frequency", NULL),
	add_parse(UINT64_NO_VAL, stats.consumed_energy, "statistics/energy/consumed", NULL),
	add_parse(SLURM_STEP_ID_STRING, step_id, "step/id", NULL),
	add_parse(STRING, stepname, "step/name", NULL),
	add_parse(UINT32, suspended, "time/suspended", NULL),
	add_parse(UINT64, sys_cpu_sec, "time/system/seconds", NULL),
	add_parse(UINT32, sys_cpu_usec, "time/system/microseconds", NULL),
	add_parse(TASK_DISTRIBUTION, task_dist, "task/distribution", NULL),
	add_parse(UINT64, tot_cpu_sec, "time/total/seconds", NULL),
	add_parse(UINT32, tot_cpu_usec, "time/total/microseconds", NULL),
	add_parse(UINT64, user_cpu_sec, "time/user/seconds", NULL),
	add_parse(UINT32, user_cpu_usec, "time/user/microseconds", NULL),
	add_complex_parser(slurmdb_step_rec_t, STEP_NODES, false, "nodes/list", NULL),
	add_complex_parser(slurmdb_step_rec_t, STEP_TRES_REQ_MAX, false, "tres/requested/max", NULL),
	add_complex_parser(slurmdb_step_rec_t, STEP_TRES_REQ_MIN, false, "tres/requested/min", NULL),
	add_complex_parser(slurmdb_step_rec_t, STEP_TRES_USAGE_MAX, false, "tres/consumed/max", NULL),
	add_complex_parser(slurmdb_step_rec_t, STEP_TRES_USAGE_MIN, false, "tres/consumed/min", NULL),
	add_parse(TRES_STR, stats.tres_usage_in_ave, "tres/requested/average", NULL),
	add_parse(TRES_STR, stats.tres_usage_in_tot, "tres/requested/total", NULL),
	add_parse(TRES_STR, stats.tres_usage_out_ave, "tres/consumed/average", NULL),
	add_parse(TRES_STR, stats.tres_usage_out_tot, "tres/consumed/total", NULL),
	add_parse(TRES_STR, tres_alloc_str, "tres/allocated", NULL),
};
#undef add_parse
#undef add_skip

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_stats_rec_t, mtype, false, field, 0, path, desc)
/* should mirror the structure of slurmdb_stats_rec_t */
static const parser_t PARSER_ARRAY(STATS_REC)[] = {
	add_parse(TIMESTAMP, time_start, "time_start", NULL),
	add_parse(ROLLUP_STATS_PTR, dbd_rollup_stats, "rollups", NULL),
	add_parse(STATS_RPC_LIST, rpc_list, "RPCs", NULL),
	add_parse(STATS_USER_LIST, user_list, "users", NULL),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_rpc_obj_t, mtype, false, field, 0, path, desc)
/* should mirror the structure of slurmdb_rpc_obj_t */
static const parser_t PARSER_ARRAY(STATS_USER)[] = {
	add_parse(USER_ID, id, "user", NULL),
	add_parse(UINT32, cnt, "count", NULL),
	add_parse(UINT64, time_ave, "time/average", NULL),
	add_parse(UINT64, time, "time/total", NULL),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_rpc_obj_t, mtype, false, field, 0, path, desc)
/* should mirror the structure of slurmdb_rpc_obj_t */
static const parser_t PARSER_ARRAY(STATS_RPC)[] = {
	add_parse(SLURMDB_RPC_ID, id, "rpc", NULL),
	add_parse(UINT32, cnt, "count", NULL),
	add_parse(UINT64, time_ave, "time/average", NULL),
	add_parse(UINT64, time, "time/total", NULL),
};
#undef add_parse

static const flag_bit_t PARSER_FLAG_ARRAY(CLUSTER_REC_FLAGS)[] = {
	add_flag_bit(CLUSTER_FLAG_REGISTER, "REGISTERING"),
	add_flag_bit(CLUSTER_FLAG_MULTSD, "MULTIPLE_SLURMD"),
	add_flag_bit(CLUSTER_FLAG_FE, "FRONT_END"),
	add_flag_bit(CLUSTER_FLAG_FED, "FEDERATION"),
	add_flag_bit(CLUSTER_FLAG_EXT, "EXTERNAL"),
};

#define add_skip(field) \
	add_parser_skip(slurmdb_cluster_rec_t, field)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_cluster_rec_t, mtype, false, field, 0, path, desc)
#define add_removed(mtype, path, desc, deprec) \
	add_parser_removed(slurmdb_cluster_rec_t, mtype, false, path, desc, deprec)
/* should mirror the structure of slurmdb_cluster_rec_t */
static const parser_t PARSER_ARRAY(CLUSTER_REC)[] = {
	add_skip(classification), /* to be deprecated */
	add_skip(comm_fail_time), /* not packed */
	add_skip(control_addr), /* not packed */
	add_parse(STRING, control_host, "controller/host", NULL),
	add_parse(UINT32, control_port, "controller/port", NULL),
	add_skip(dim_size), /* BG deprecated */
	add_skip(fed), /* federation not supportted */
	add_parse_bit_flag_array(slurmdb_cluster_rec_t, CLUSTER_REC_FLAGS, false, flags, "flags", NULL),
	add_skip(lock), /* not packed */
	add_parse(STRING, name, "name", NULL),
	add_parse(STRING, nodes, "nodes", NULL),
	add_removed(SELECT_PLUGIN_ID, "select_plugin", NULL, SLURM_24_05_PROTOCOL_VERSION),
	add_parse(ASSOC_SHORT_PTR, root_assoc, "associations/root", NULL),
	add_parse(UINT16, rpc_version, "rpc_version", NULL),
	add_skip(send_rpc), /* not packed */
	add_parse(TRES_STR, tres_str, "tres", NULL),
};
#undef add_parse
#undef add_skip
#undef add_removed

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_cluster_accounting_rec_t, mtype, false, field, 0, path, desc)
/* should mirror the structure of slurmdb_cluster_accounting_rec_t */
static const parser_t PARSER_ARRAY(CLUSTER_ACCT_REC)[] = {
	add_parse(UINT64, alloc_secs, "time/allocated", NULL),
	add_parse(UINT64, down_secs, "time/down", NULL),
	add_parse(UINT64, idle_secs, "time/idle", NULL),
	add_parse(UINT64, over_secs, "time/overcommitted", NULL),
	add_parse(UINT64, pdown_secs, "time/planned_down", NULL),
	add_parse(TIMESTAMP, period_start, "time/start", NULL),
	add_parse(STRING, tres_rec.name, "tres/name", NULL),
	add_parse(STRING, tres_rec.type, "tres/type", NULL),
	add_parse(UINT32, tres_rec.id, "tres/id", NULL),
	add_parse(UINT64, tres_rec.count, "tres/count", NULL),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_tres_nct_rec_t, mtype, false, field, 0, path, desc)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_tres_nct_rec_t, mtype, true, field, 0, path, desc)
/* should mirror the structure of slurmdb_tres_nct_rec_t  */
static const parser_t PARSER_ARRAY(TRES_NCT)[] = {
	add_parse_req(STRING, type, "type", NULL),
	add_parse(STRING, name, "name", NULL),
	add_parse(UINT32, id, "id", NULL),
	add_parse(INT64, count, "count", NULL),
	add_parse(INT64, task, "task", NULL),
	add_parse(STRING, node, "node", NULL),
};
#undef add_parse
#undef add_parse_req

#define add_skip(field) \
	add_parser_skip(slurmdb_assoc_usage_t, field)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_assoc_usage_t, mtype, false, field, 0, path, desc)
/* should mirror the structure of slurmdb_assoc_usage_t */
static const parser_t PARSER_ARRAY(ASSOC_USAGE)[] = {
	add_parse(UINT32, accrue_cnt, "accrue_job_count", NULL),
	add_skip(children_list), /* not packed */
	add_skip(grp_node_bitmap), /* not packed */
	add_skip(grp_node_job_cnt), /* not packed */
	add_skip(grp_used_tres), /* not packed */
	add_skip(grp_used_tres_run_secs), /* not packed */
	add_parse(FLOAT64, grp_used_wall, "group_used_wallclock", NULL),
	add_parse(FLOAT64, fs_factor, "fairshare_factor", NULL),
	add_parse(UINT32, level_shares, "fairshare_shares", NULL),
	add_skip(parent_assoc_ptr), /* not packed */
	add_parse(FLOAT64, priority_norm, "normalized_priority", NULL),
	add_skip(fs_assoc_ptr), /* not packed */
	add_parse(FLOAT64, shares_norm, "normalized_shares", NULL),
	add_parse(FLOAT128, usage_efctv, "effective_normalized_usage", NULL),
	add_parse(FLOAT128, usage_norm, "normalized_usage", NULL),
	add_parse(FLOAT128, usage_raw, "raw_usage", NULL),
	add_skip(usage_tres_raw), /* not packed */
	add_parse(UINT32, used_jobs, "active_jobs", NULL),
	add_parse(UINT32, used_submit_jobs, "job_count", NULL),
	add_parse(FLOAT128, level_fs, "fairshare_level", NULL),
	add_skip(valid_qos),
};
#undef add_parse
#undef add_skip

#define add_skip(field) \
	add_parser_skip(stats_info_response_msg_t, field)
#define add_parse(mtype, field, path, desc) \
	add_parser(stats_info_response_msg_t, mtype, false, field, 0, path, desc)
#define add_cparse(mtype, path, desc) \
	add_complex_parser(stats_info_response_msg_t, mtype, false, path, desc)
static const parser_t PARSER_ARRAY(STATS_MSG)[] = {
	add_parse(UINT32, parts_packed, "parts_packed", NULL),
	add_parse(TIMESTAMP_NO_VAL, req_time, "req_time", NULL),
	add_parse(TIMESTAMP_NO_VAL, req_time_start, "req_time_start", NULL),
	add_parse(UINT32, server_thread_count, "server_thread_count", NULL),
	add_parse(UINT32, agent_queue_size, "agent_queue_size", NULL),
	add_parse(UINT32, agent_count, "agent_count", NULL),
	add_parse(UINT32, agent_thread_count, "agent_thread_count", NULL),
	add_parse(UINT32, dbd_agent_queue_size, "dbd_agent_queue_size", NULL),
	add_parse(UINT32, gettimeofday_latency, "gettimeofday_latency", NULL),
	add_parse(UINT32, schedule_cycle_max, "schedule_cycle_max", NULL),
	add_parse(UINT32, schedule_cycle_last, "schedule_cycle_last", NULL),
	add_parse(UINT32, schedule_cycle_sum, "schedule_cycle_sum", NULL),
	add_parse(UINT32, schedule_cycle_counter, "schedule_cycle_total", NULL),
	add_cparse(STATS_MSG_CYCLE_MEAN, "schedule_cycle_mean", NULL),
	add_cparse(STATS_MSG_CYCLE_MEAN_DEPTH, "schedule_cycle_mean_depth", NULL),
	add_cparse(STATS_MSG_CYCLE_PER_MIN, "schedule_cycle_per_minute", NULL),
	add_parse(UINT32, schedule_cycle_depth, "schedule_cycle_depth", NULL),
	add_cparse(STATS_MSG_SCHEDULE_EXIT, "schedule_exit", NULL),
	add_skip(schedule_exit), /* handled by STATS_MSG_SCHEDULE_EXIT */
	add_skip(schedule_exit_cnt), /* handled by STATS_MSG_SCHEDULE_EXIT */
	add_parse(UINT32, schedule_queue_len, "schedule_queue_length", NULL),
	add_parse(UINT32, jobs_submitted, "jobs_submitted", NULL),
	add_parse(UINT32, jobs_started, "jobs_started", NULL),
	add_parse(UINT32, jobs_completed, "jobs_completed", NULL),
	add_parse(UINT32, jobs_canceled, "jobs_canceled", NULL),
	add_parse(UINT32, jobs_failed, "jobs_failed", NULL),
	add_parse(UINT32, jobs_pending, "jobs_pending", NULL),
	add_parse(UINT32, jobs_running, "jobs_running", NULL),
	add_parse(TIMESTAMP_NO_VAL, job_states_ts, "job_states_ts", NULL),
	add_parse(UINT32, bf_backfilled_jobs, "bf_backfilled_jobs", NULL),
	add_parse(UINT32, bf_last_backfilled_jobs, "bf_last_backfilled_jobs", NULL),
	add_parse(UINT32, bf_backfilled_het_jobs, "bf_backfilled_het_jobs", NULL),
	add_parse(UINT32, bf_cycle_counter, "bf_cycle_counter", NULL),
	add_cparse(STATS_MSG_BF_CYCLE_MEAN, "bf_cycle_mean", NULL),
	add_cparse(STATS_MSG_BF_DEPTH_MEAN, "bf_depth_mean", NULL),
	add_cparse(STATS_MSG_BF_DEPTH_MEAN_TRY, "bf_depth_mean_try", NULL),
	add_parse(UINT64, bf_cycle_sum, "bf_cycle_sum", NULL),
	add_parse(UINT32, bf_cycle_last, "bf_cycle_last", NULL),
	add_parse(UINT32, bf_cycle_max, "bf_cycle_max", NULL),
	add_cparse(STATS_MSG_BF_EXIT, "bf_exit", NULL),
	add_skip(bf_exit), /* handled by STATS_MSG_BF_EXIT */
	add_parse(UINT32, bf_last_depth, "bf_last_depth", NULL),
	add_parse(UINT32, bf_last_depth_try, "bf_last_depth_try", NULL),
	add_parse(UINT32, bf_depth_sum, "bf_depth_sum", NULL),
	add_parse(UINT32, bf_depth_try_sum, "bf_depth_try_sum", NULL),
	add_parse(UINT32, bf_queue_len, "bf_queue_len", NULL),
	add_cparse(STATS_MSG_BF_QUEUE_LEN_MEAN, "bf_queue_len_mean", NULL),
	add_parse(UINT32, bf_queue_len_sum, "bf_queue_len_sum", NULL),
	add_parse(UINT32, bf_table_size, "bf_table_size", NULL),
	add_parse(UINT32, bf_table_size_sum, "bf_table_size_sum", NULL),
	add_cparse(STATS_MSG_BF_QUEUE_LEN_MEAN, "bf_table_size_mean", NULL),
	add_parse(TIMESTAMP_NO_VAL, bf_when_last_cycle, "bf_when_last_cycle", NULL),
	add_cparse(STATS_MSG_BF_ACTIVE, "bf_active", NULL),
	add_skip(rpc_type_size),
	add_cparse(STATS_MSG_RPCS_BY_TYPE, "rpcs_by_message_type", NULL),
	add_skip(rpc_type_id), /* handled by STATS_MSG_RPCS_BY_TYPE */
	add_skip(rpc_type_cnt), /* handled by STATS_MSG_RPCS_BY_TYPE */
	add_skip(rpc_type_time), /* handled by STATS_MSG_RPCS_BY_TYPE */
	add_cparse(STATS_MSG_RPCS_BY_USER, "rpcs_by_user", NULL),
	add_skip(rpc_user_size), /* handled by STATS_MSG_RPCS_BY_TYPE */
	add_skip(rpc_user_id), /* handled by STATS_MSG_RPCS_BY_TYPE */
	add_skip(rpc_user_cnt), /* handled by STATS_MSG_RPCS_BY_TYPE */
	add_skip(rpc_user_time), /* handled by STATS_MSG_RPCS_BY_TYPE */
	add_cparse(STATS_MSG_RPCS_QUEUE, "pending_rpcs", "Pending RPC statistics"),
	add_skip(rpc_queue_type_count), /* handled by STATS_MSG_RPCS_QUEUE */
	add_skip(rpc_queue_type_id), /* handled by STATS_MSG_RPCS_QUEUE */
	add_skip(rpc_queue_count), /* handled by STATS_MSG_RPCS_QUEUE */
	add_cparse(STATS_MSG_RPCS_DUMP, "pending_rpcs_by_hostlist", "Pending RPCs hostlists"),
	add_skip(rpc_dump_count), /* handled by STATS_MSG_RPCS_DUMP */
	add_skip(rpc_dump_types), /* handled by STATS_MSG_RPCS_DUMP */
	add_skip(rpc_dump_hostlist), /* handled by STATS_MSG_RPCS_DUMP */
};
#undef add_parse
#undef add_cparse
#undef add_skip

#define add_parse(mtype, field, path, desc) \
	add_parser(bf_exit_fields_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(BF_EXIT_FIELDS)[] = {
	add_parse(UINT32, end_job_queue, "end_job_queue", NULL),
	add_parse(UINT32, bf_max_job_start, "bf_max_job_start", NULL),
	add_parse(UINT32, bf_max_job_test, "bf_max_job_test", NULL),
	add_parse(UINT32, bf_max_time, "bf_max_time", NULL),
	add_parse(UINT32, bf_node_space_size, "bf_node_space_size", NULL),
	add_parse(UINT32, state_changed, "state_changed", NULL),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(schedule_exit_fields_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(SCHEDULE_EXIT_FIELDS)[] = {
	add_parse(UINT32, end_job_queue, "end_job_queue", NULL),
	add_parse(UINT32, default_queue_depth, "default_queue_depth", NULL),
	add_parse(UINT32, max_job_start, "max_job_start", NULL),
	add_parse(UINT32, max_rpc_cnt, "max_rpc_cnt", NULL),
	add_parse(UINT32, max_sched_time, "max_sched_time", NULL),
	add_parse(UINT32, licenses, "licenses", NULL),
};
#undef add_parse

static const flag_bit_t PARSER_FLAG_ARRAY(NODE_STATES)[] = {
	add_flag_equal(NO_VAL, INFINITE, "INVALID"),
	add_flag_equal(NODE_STATE_UNKNOWN, NODE_STATE_BASE, "UNKNOWN"),
	add_flag_equal(NODE_STATE_DOWN, NODE_STATE_BASE, "DOWN"),
	add_flag_equal(NODE_STATE_IDLE, NODE_STATE_BASE, "IDLE"),
	add_flag_equal(NODE_STATE_ALLOCATED, NODE_STATE_BASE, "ALLOCATED"),
	add_flag_equal(NODE_STATE_ERROR, NODE_STATE_BASE, "ERROR"),
	add_flag_equal(NODE_STATE_MIXED, NODE_STATE_BASE, "MIXED"),
	add_flag_equal(NODE_STATE_FUTURE, NODE_STATE_BASE, "FUTURE"),
	add_flag_masked_bit(NODE_STATE_NET, NODE_STATE_FLAGS, "PERFCTRS"),
	add_flag_masked_bit(NODE_STATE_RES, NODE_STATE_FLAGS, "RESERVED"),
	add_flag_masked_bit(NODE_STATE_UNDRAIN, NODE_STATE_FLAGS, "UNDRAIN"),
	add_flag_masked_bit(NODE_STATE_CLOUD, NODE_STATE_FLAGS, "CLOUD"),
	add_flag_masked_bit(NODE_RESUME, NODE_STATE_FLAGS, "RESUME"),
	add_flag_masked_bit(NODE_STATE_DRAIN, NODE_STATE_FLAGS, "DRAIN"),
	add_flag_masked_bit(NODE_STATE_COMPLETING, NODE_STATE_FLAGS, "COMPLETING"),
	add_flag_masked_bit(NODE_STATE_NO_RESPOND, NODE_STATE_FLAGS, "NOT_RESPONDING"),
	add_flag_masked_bit(NODE_STATE_POWERED_DOWN, NODE_STATE_FLAGS, "POWERED_DOWN"),
	add_flag_masked_bit(NODE_STATE_FAIL, NODE_STATE_FLAGS, "FAIL"),
	add_flag_masked_bit(NODE_STATE_POWERING_UP, NODE_STATE_FLAGS, "POWERING_UP"),
	add_flag_masked_bit(NODE_STATE_MAINT, NODE_STATE_FLAGS, "MAINTENANCE"),
	add_flag_masked_bit(NODE_STATE_REBOOT_REQUESTED, NODE_STATE_FLAGS, "REBOOT_REQUESTED"),
	add_flag_masked_bit(NODE_STATE_REBOOT_CANCEL, NODE_STATE_FLAGS, "REBOOT_CANCELED"),
	add_flag_masked_bit(NODE_STATE_POWERING_DOWN, NODE_STATE_FLAGS, "POWERING_DOWN"),
	add_flag_masked_bit(NODE_STATE_DYNAMIC_FUTURE, NODE_STATE_FLAGS, "DYNAMIC_FUTURE"),
	add_flag_masked_bit(NODE_STATE_REBOOT_ISSUED, NODE_STATE_FLAGS, "REBOOT_ISSUED"),
	add_flag_masked_bit(NODE_STATE_PLANNED, NODE_STATE_FLAGS, "PLANNED"),
	add_flag_masked_bit(NODE_STATE_INVALID_REG, NODE_STATE_FLAGS, "INVALID_REG"),
	add_flag_masked_bit(NODE_STATE_POWER_DOWN, NODE_STATE_FLAGS, "POWER_DOWN"),
	add_flag_masked_bit(NODE_STATE_POWER_UP, NODE_STATE_FLAGS, "POWER_UP"),
	add_flag_masked_bit(NODE_STATE_POWER_DRAIN, NODE_STATE_FLAGS, "POWER_DRAIN"),
	add_flag_masked_bit(NODE_STATE_DYNAMIC_NORM, NODE_STATE_FLAGS, "DYNAMIC_NORM"),
};

static const flag_bit_t PARSER_FLAG_ARRAY(PARTITION_STATES)[] = {
	add_flag_equal(PARTITION_INACTIVE, INFINITE16, "INACTIVE"),
	add_flag_equal(NO_VAL16, INFINITE16, "UNKNOWN"),
	add_flag_equal(PARTITION_UP, INFINITE16, "UP"),
	add_flag_equal(PARTITION_DOWN, INFINITE16, "DOWN"),
	add_flag_equal(PARTITION_DRAIN, INFINITE16, "DRAIN"),
};

#define add_parse(mtype, field, path, desc) \
	add_parser(node_info_t, mtype, false, field, 0, path, desc)
#define add_cparse(mtype, path, desc) \
	add_complex_parser(node_info_t, mtype, false, path, desc)
#define add_removed(mtype, path, desc, deprec) \
	add_parser_removed(node_info_t, mtype, false, path, desc, deprec)
static const parser_t PARSER_ARRAY(NODE)[] = {
	add_parse(STRING, arch, "architecture", NULL),
	add_parse(STRING, bcast_address, "burstbuffer_network_address", NULL),
	add_parse(UINT16, boards, "boards", NULL),
	add_parse(TIMESTAMP_NO_VAL, boot_time, "boot_time", NULL),
	add_parse(STRING, cluster_name, "cluster_name", NULL),
	add_parse(UINT16, cores, "cores", NULL),
	add_parse(UINT16, core_spec_cnt, "specialized_cores", NULL),
	add_parse(UINT32, cpu_bind, "cpu_binding", NULL),
	add_parse(UINT32, cpu_load, "cpu_load", NULL),
	add_parse(UINT64_NO_VAL, free_mem, "free_mem", NULL),
	add_parse(UINT16, cpus, "cpus", NULL),
	add_parse(UINT16, cpus_efctv, "effective_cpus", NULL),
	add_parse(STRING, cpu_spec_list, "specialized_cpus", NULL),
	add_parse(ACCT_GATHER_ENERGY_PTR, energy, "energy", NULL),
	add_removed(EXT_SENSORS_DATA_PTR, "external_sensors", NULL, SLURM_24_05_PROTOCOL_VERSION),
	add_parse(STRING, extra, "extra", NULL),
	add_parse(POWER_MGMT_DATA_PTR, power, "power", NULL),
	add_parse(CSV_STRING, features, "features", NULL),
	add_parse(CSV_STRING, features_act, "active_features", NULL),
	add_parse(STRING, gres, "gres", NULL),
	add_parse(STRING, gres_drain, "gres_drained", NULL),
	add_parse(STRING, gres_used, "gres_used", NULL),
	add_parse(STRING, instance_id, "instance_id", NULL),
	add_parse(STRING, instance_type, "instance_type", NULL),
	add_parse(TIMESTAMP_NO_VAL, last_busy, "last_busy", NULL),
	add_parse(STRING, mcs_label, "mcs_label", NULL),
	add_parse(UINT64, mem_spec_limit, "specialized_memory", NULL),
	add_parse(STRING, name, "name", NULL),
	add_parse(NODE_STATES, next_state, "next_state_after_reboot", NULL),
	add_parse(STRING, node_addr, "address", NULL),
	add_parse(STRING, node_hostname, "hostname", NULL),
	add_parse_bit_flag_array(node_info_t, NODE_STATES, false, node_state, "state", NULL),
	add_parse(STRING, os, "operating_system", NULL),
	add_parse(USER_ID, owner, "owner", NULL),
	add_parse(CSV_STRING, partitions, "partitions", NULL),
	add_parse(UINT16, port, "port", NULL),
	add_parse(UINT64, real_memory, "real_memory", NULL),
	add_parse(STRING, comment, "comment", NULL),
	add_parse(STRING, reason, "reason", NULL),
	add_parse(TIMESTAMP_NO_VAL, reason_time, "reason_changed_at", NULL),
	add_parse(USER_ID, reason_uid, "reason_set_by_user", NULL),
	add_parse(TIMESTAMP_NO_VAL, resume_after, "resume_after", NULL),
	add_parse(STRING, resv_name, "reservation", NULL),
	add_cparse(NODE_SELECT_ALLOC_MEMORY, "alloc_memory", NULL),
	add_cparse(NODE_SELECT_ALLOC_CPUS, "alloc_cpus", NULL),
	add_cparse(NODE_SELECT_ALLOC_IDLE_CPUS, "alloc_idle_cpus", NULL),
	add_cparse(NODE_SELECT_TRES_USED, "tres_used", NULL),
	add_cparse(NODE_SELECT_TRES_WEIGHTED, "tres_weighted", NULL),
	add_parse(TIMESTAMP_NO_VAL, slurmd_start_time, "slurmd_start_time", NULL),
	add_parse(UINT16, sockets, "sockets", NULL),
	add_parse(UINT16, threads, "threads", NULL),
	add_parse(UINT32, tmp_disk, "temporary_disk", NULL),
	add_parse(UINT32, weight, "weight", NULL),
	add_parse(STRING, tres_fmt_str, "tres", NULL),
	add_parse(STRING, version, "version", NULL),
};
#undef add_parse
#undef add_cparse
#undef add_removed

#define add_parse(mtype, field, path, desc) \
	add_parser(slurm_license_info_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(LICENSE)[] = {
	add_parse(STRING, name, "LicenseName", NULL),
	add_parse(UINT32, total, "Total", NULL),
	add_parse(UINT32, in_use, "Used", NULL),
	add_parse(UINT32, available, "Free", NULL),
	add_parse(BOOL, remote, "Remote", NULL),
	add_parse(UINT32, reserved, "Reserved", NULL),
	add_parse(UINT32, last_consumed, "LastConsumed", NULL),
	add_parse(UINT32, last_deficit, "LastDeficit", NULL),
	add_parse(TIMESTAMP, last_update, "LastUpdate", NULL),
};
#undef add_parse

static const flag_bit_t PARSER_FLAG_ARRAY(JOB_FLAGS)[] = {
	add_flag_bit(KILL_INV_DEP, "KILL_INVALID_DEPENDENCY"),
	add_flag_bit(NO_KILL_INV_DEP, "NO_KILL_INVALID_DEPENDENCY"),
	add_flag_bit(HAS_STATE_DIR, "HAS_STATE_DIRECTORY"),
	add_flag_bit(BACKFILL_TEST, "TESTING_BACKFILL"),
	add_flag_bit(GRES_ENFORCE_BIND, "GRES_BINDING_ENFORCED"),
	add_flag_bit(TEST_NOW_ONLY, "TEST_NOW_ONLY"),
	add_flag_bit(JOB_SEND_ENV, "SEND_JOB_ENVIRONMENT"),
	add_flag_bit(SPREAD_JOB, "SPREAD_JOB"),
	add_flag_bit(USE_MIN_NODES, "PREFER_MINIMUM_NODE_COUNT"),
	add_flag_bit(JOB_KILL_HURRY, "JOB_KILL_HURRY"),
	add_flag_bit(TRES_STR_CALC, "SKIP_TRES_STRING_ACCOUNTING"),
	add_flag_bit(SIB_JOB_FLUSH, "SIBLING_CLUSTER_UPDATE_ONLY"),
	add_flag_bit(HET_JOB_FLAG, "HETEROGENEOUS_JOB"),
	add_flag_bit(JOB_NTASKS_SET, "EXACT_TASK_COUNT_REQUESTED"),
	add_flag_bit(JOB_CPUS_SET, "EXACT_CPU_COUNT_REQUESTED"),
	add_flag_bit(BF_WHOLE_NODE_TEST, "TESTING_WHOLE_NODE_BACKFILL"),
	add_flag_bit(TOP_PRIO_TMP, "TOP_PRIORITY_JOB"),
	add_flag_bit(JOB_ACCRUE_OVER, "ACCRUE_COUNT_CLEARED"),
	add_flag_bit(GRES_DISABLE_BIND, "GRED_BINDING_DISABLED"),
	add_flag_bit(JOB_WAS_RUNNING, "JOB_WAS_RUNNING"),
	add_flag_bit(RESET_ACCRUE_TIME, "JOB_ACCRUE_TIME_RESET"),
	add_flag_bit(CRON_JOB, "CRON_JOB"),
	add_flag_bit(JOB_MEM_SET, "EXACT_MEMORY_REQUESTED"),
	add_flag_bit(USE_DEFAULT_ACCT, "USING_DEFAULT_ACCOUNT"),
	add_flag_bit(USE_DEFAULT_PART, "USING_DEFAULT_PARTITION"),
	add_flag_bit(USE_DEFAULT_QOS, "USING_DEFAULT_QOS"),
	add_flag_bit(USE_DEFAULT_WCKEY, "USING_DEFAULT_WCKEY"),
	add_flag_bit(JOB_DEPENDENT, "DEPENDENT"),
	add_flag_bit(JOB_MAGNETIC, "MAGNETIC"),
	add_flag_bit(JOB_PART_ASSIGNED, "PARTITION_ASSIGNED"),
	add_flag_bit(BACKFILL_SCHED, "BACKFILL_ATTEMPTED"),
	add_flag_bit(BACKFILL_LAST, "SCHEDULING_ATTEMPTED"),
	add_flag_bit(JOB_SEND_SCRIPT, "SAVE_BATCH_SCRIPT"),
};

static const flag_bit_t PARSER_FLAG_ARRAY(JOB_SHOW_FLAGS)[] = {
	add_flag_bit(SHOW_ALL, "ALL"),
	add_flag_bit(SHOW_DETAIL, "DETAIL"),
	add_flag_bit(SHOW_MIXED, "MIXED"),
	add_flag_bit(SHOW_LOCAL, "LOCAL"),
	add_flag_bit(SHOW_SIBLING, "SIBLING"),
	add_flag_bit(SHOW_FEDERATION, "FEDERATION"),
	add_flag_bit(SHOW_FUTURE, "FUTURE"),
};

static const flag_bit_t PARSER_FLAG_ARRAY(JOB_MAIL_FLAGS)[] = {
	add_flag_bit(MAIL_JOB_BEGIN, "BEGIN"),
	add_flag_bit(MAIL_JOB_END, "END"),
	add_flag_bit(MAIL_JOB_FAIL, "FAIL"),
	add_flag_bit(MAIL_JOB_REQUEUE, "REQUEUE"),
	add_flag_bit(MAIL_JOB_TIME100, "TIME=100%"),
	add_flag_bit(MAIL_JOB_TIME90, "TIME=90%"),
	add_flag_bit(MAIL_JOB_TIME80, "TIME=80%"),
	add_flag_bit(MAIL_JOB_TIME50, "TIME=50%"),
	add_flag_bit(MAIL_JOB_STAGE_OUT, "STAGE_OUT"),
	add_flag_bit(MAIL_ARRAY_TASKS, "ARRAY_TASKS"),
	add_flag_bit(MAIL_INVALID_DEPEND, "INVALID_DEPENDENCY"),
};

static const flag_bit_t PARSER_FLAG_ARRAY(ACCT_GATHER_PROFILE)[] = {
	add_flag_equal(ACCT_GATHER_PROFILE_NOT_SET, INFINITE, "NOT_SET"),
	add_flag_equal(ACCT_GATHER_PROFILE_NONE, INFINITE, "NONE"),
	add_flag_bit(ACCT_GATHER_PROFILE_ENERGY, "ENERGY"),
	add_flag_bit(ACCT_GATHER_PROFILE_LUSTRE, "LUSTRE"),
	add_flag_bit(ACCT_GATHER_PROFILE_NETWORK, "NETWORK"),
	add_flag_bit(ACCT_GATHER_PROFILE_TASK, "TASK"),
};

static const flag_bit_t PARSER_FLAG_ARRAY(JOB_SHARED)[] = {
	add_flag_equal(JOB_SHARED_NONE, INFINITE16, "none"),
	add_flag_equal(JOB_SHARED_OK, INFINITE16, "oversubscribe"),
	add_flag_equal(JOB_SHARED_USER, INFINITE16, "user"),
	add_flag_equal(JOB_SHARED_MCS, INFINITE16, "mcs"),
};

static const flag_bit_t PARSER_FLAG_ARRAY(JOB_EXCLUSIVE_FLAGS)[] = {
	add_flag_equal(JOB_SHARED_NONE, INFINITE16, "true"),
	add_flag_equal(JOB_SHARED_OK, INFINITE16, "false"),
	add_flag_equal(JOB_SHARED_USER, INFINITE16, "user"),
	add_flag_equal(JOB_SHARED_MCS, INFINITE16, "mcs"),
};

static const flag_bit_t PARSER_FLAG_ARRAY(OVERSUBSCRIBE_FLAGS)[] = {
	add_flag_bit(SHARED_FORCE, "force"),
};

#define add_skip(field) \
	add_parser_skip(slurm_job_info_t, field)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurm_job_info_t, mtype, false, field, 0, path, desc)
#define add_parse_overload(mtype, field, overloads, path, desc) \
	add_parser(slurm_job_info_t, mtype, false, field, overloads, path, desc)
#define add_parse_deprec(mtype, field, overloads, path, desc, deprec) \
	add_parser_deprec(slurm_job_info_t, mtype, false, field, overloads, path, desc, deprec)
#define add_cparse(mtype, path, desc) \
	add_complex_parser(slurm_job_info_t, mtype, false, path, desc)
#define add_removed(mtype, path, desc, deprec) \
	add_parser_removed(slurm_job_info_t, mtype, false, path, desc, deprec)
static const parser_t PARSER_ARRAY(JOB_INFO)[] = {
	add_parse(STRING, account, "account", NULL),
	add_parse(TIMESTAMP_NO_VAL, accrue_time, "accrue_time", NULL),
	add_parse(STRING, admin_comment, "admin_comment", NULL),
	add_parse(STRING, alloc_node, "allocating_node", NULL),
	add_skip(alloc_sid),
	add_skip(array_bitmap),
	add_parse(UINT32_NO_VAL, array_job_id, "array_job_id", NULL),
	add_parse(UINT32_NO_VAL, array_task_id, "array_task_id", NULL),
	add_parse(UINT32_NO_VAL, array_max_tasks, "array_max_tasks", NULL),
	add_parse(STRING, array_task_str, "array_task_string", NULL),
	add_parse(UINT32, assoc_id, "association_id", NULL),
	add_parse(STRING, batch_features, "batch_features", NULL),
	add_parse(BOOL16, batch_flag, "batch_flag", NULL),
	add_parse(STRING, batch_host, "batch_host", NULL),
	add_parse_bit_flag_array(slurm_job_info_t, JOB_FLAGS, false, bitflags, "flags", NULL),
	add_skip(boards_per_node),
	add_parse(STRING, burst_buffer, "burst_buffer", NULL),
	add_parse(STRING, burst_buffer_state, "burst_buffer_state", NULL),
	add_parse(STRING, cluster, "cluster", NULL),
	add_parse(STRING, cluster_features, "cluster_features", NULL),
	add_parse(STRING, command, "command", NULL),
	add_parse(STRING, comment, "comment", NULL),
	add_parse(STRING, container, "container", NULL),
	add_parse(STRING, container_id, "container_id", NULL),
	add_parse(BOOL16_NO_VAL, contiguous, "contiguous", NULL),
	add_parse_overload(CORE_SPEC, core_spec, 1, "core_spec", NULL),
	add_parse_overload(THREAD_SPEC, core_spec, 1, "thread_spec", NULL),
	add_parse(UINT16_NO_VAL, cores_per_socket, "cores_per_socket", NULL),
	add_parse(FLOAT64_NO_VAL, billable_tres, "billable_tres", NULL),
	add_parse(UINT16_NO_VAL, cpus_per_task, "cpus_per_task", NULL),
	add_parse(UINT32_NO_VAL, cpu_freq_min, "cpu_frequency_minimum", NULL),
	add_parse(UINT32_NO_VAL, cpu_freq_max, "cpu_frequency_maximum", NULL),
	add_parse(UINT32_NO_VAL, cpu_freq_gov, "cpu_frequency_governor", NULL),
	add_parse(STRING, cpus_per_tres, "cpus_per_tres", NULL),
	add_parse(STRING, cronspec, "cron", NULL),
	add_parse(TIMESTAMP_NO_VAL, deadline, "deadline", NULL),
	add_parse(UINT32_NO_VAL, delay_boot, "delay_boot", NULL),
	add_parse(STRING, dependency, "dependency", NULL),
	add_parse(PROCESS_EXIT_CODE, derived_ec, "derived_exit_code", NULL),
	add_parse(TIMESTAMP_NO_VAL, eligible_time, "eligible_time", NULL),
	add_parse(TIMESTAMP_NO_VAL, end_time, "end_time", NULL),
	add_parse(STRING, exc_nodes, "excluded_nodes", NULL),
	add_skip(exc_node_inx),
	add_parse(PROCESS_EXIT_CODE, exit_code, "exit_code", NULL),
	add_parse(STRING, extra, "extra", NULL),
	add_parse(STRING, failed_node, "failed_node", NULL),
	add_parse(STRING, features, "features", NULL),
	add_parse(STRING, fed_origin_str, "federation_origin", NULL),
	add_skip(fed_siblings_active),
	add_parse(STRING, fed_siblings_active_str, "federation_siblings_active", NULL),
	add_skip(fed_siblings_viable),
	add_parse(STRING, fed_siblings_viable_str, "federation_siblings_viable", NULL),
	add_skip(gres_detail_cnt),
	add_skip(gres_detail_str), /* handled by JOB_INFO_GRES_DETAIL */
	add_cparse(JOB_INFO_GRES_DETAIL, "gres_detail", NULL),
	add_parse_overload(UINT32, group_id, 1, "group_id", NULL),
	add_parse_overload(GROUP_ID, group_id, 1, "group_name", NULL),
	add_parse(UINT32_NO_VAL, het_job_id, "het_job_id", NULL),
	add_parse(STRING, het_job_id_set, "het_job_id_set", NULL),
	add_parse(UINT32_NO_VAL, het_job_offset, "het_job_offset", NULL),
	add_parse(UINT32, job_id, "job_id", NULL),
	add_parse(JOB_RES_PTR, job_resrcs, "job_resources", NULL),
	add_parse(CSV_STRING, job_size_str, "job_size_str", NULL),
	add_parse_bit_flag_array(slurm_job_info_t, JOB_STATE, false, job_state, "job_state", NULL),
	add_parse(TIMESTAMP_NO_VAL, last_sched_eval, "last_sched_evaluation", NULL),
	add_parse(STRING, licenses, "licenses", NULL),
	add_parse_bit_flag_array(slurm_job_info_t, JOB_MAIL_FLAGS, false, mail_type, "mail_type", NULL),
	add_parse(STRING, mail_user, "mail_user", NULL),
	add_parse(UINT32_NO_VAL, max_cpus, "max_cpus", NULL),
	add_parse(UINT32_NO_VAL, max_nodes, "max_nodes", NULL),
	add_parse(STRING, mcs_label, "mcs_label", NULL),
	add_parse(STRING, mem_per_tres, "memory_per_tres", NULL),
	add_parse(STRING, name, "name", NULL),
	add_parse(STRING, network, "network", NULL),
	add_parse(STRING, nodes, "nodes", NULL),
	add_parse(NICE, nice, "nice", NULL),
	add_parse(UINT16_NO_VAL, ntasks_per_core, "tasks_per_core", NULL),
	add_parse(UINT16_NO_VAL, ntasks_per_tres, "tasks_per_tres", NULL),
	add_parse(UINT16_NO_VAL, ntasks_per_node, "tasks_per_node", NULL),
	add_parse(UINT16_NO_VAL, ntasks_per_socket, "tasks_per_socket", NULL),
	add_parse(UINT16_NO_VAL, ntasks_per_board, "tasks_per_board", NULL),
	add_parse(UINT32_NO_VAL, num_cpus, "cpus", NULL),
	add_parse(UINT32_NO_VAL, num_nodes, "node_count", NULL),
	add_parse(UINT32_NO_VAL, num_tasks, "tasks", NULL),
	add_parse(STRING, partition, "partition", NULL),
	add_parse(STRING, prefer, "prefer", NULL),
	add_parse_overload(MEM_PER_CPUS, pn_min_memory, 1, "memory_per_cpu", NULL),
	add_parse_overload(MEM_PER_NODE, pn_min_memory, 1, "memory_per_node", NULL),
	add_parse(UINT16_NO_VAL, pn_min_cpus, "minimum_cpus_per_node", NULL),
	add_parse(UINT32_NO_VAL, pn_min_tmp_disk, "minimum_tmp_disk_per_node", NULL),
	add_removed(POWER_FLAGS, "power/flags", NULL, SLURM_24_05_PROTOCOL_VERSION),
	add_parse(TIMESTAMP_NO_VAL, preempt_time, "preempt_time", NULL),
	add_parse(TIMESTAMP_NO_VAL, preemptable_time, "preemptable_time", NULL),
	add_parse(TIMESTAMP_NO_VAL, pre_sus_time, "pre_sus_time", NULL),
	add_parse_overload(HOLD, priority, 1, "hold", "Hold (true) or release (false) job"),
	add_parse_overload(UINT32_NO_VAL, priority, 1, "priority", "Request specific job priority"),
	add_parse(ACCT_GATHER_PROFILE, profile, "profile", NULL),
	add_parse(QOS_NAME, qos, "qos", NULL),
	add_parse(BOOL, reboot, "reboot", NULL),
	add_parse(STRING, req_nodes, "required_nodes", NULL),
	add_skip(req_node_inx),
	add_parse(UINT32, req_switch, "minimum_switches", NULL),
	add_parse(BOOL16, requeue, "requeue", NULL),
	add_parse(TIMESTAMP_NO_VAL, resize_time, "resize_time", NULL),
	add_parse(UINT16, restart_cnt, "restart_cnt", NULL),
	add_parse(STRING, resv_name, "resv_name", NULL),
	add_parse(STRING, sched_nodes, "scheduled_nodes", NULL),
	add_parse(STRING, selinux_context, "selinux_context", NULL),
	add_parse_overload(JOB_SHARED, shared, 2, "shared", NULL),
	add_parse_deprec(JOB_EXCLUSIVE, shared, 2, "exclusive", NULL, SLURM_23_11_PROTOCOL_VERSION),
	add_parse_deprec(BOOL16, shared, 2, "oversubscribe", NULL, SLURM_23_11_PROTOCOL_VERSION),
	add_parse_bit_flag_array(slurm_job_info_t, JOB_SHOW_FLAGS, false, show_flags, "show_flags", NULL),
	add_parse(UINT16, sockets_per_board, "sockets_per_board", NULL),
	add_parse(UINT16_NO_VAL, sockets_per_node, "sockets_per_node", NULL),
	add_parse(TIMESTAMP_NO_VAL, start_time, "start_time", NULL),
	add_skip(start_protocol_ver),
	add_parse(STRING, state_desc, "state_description", NULL),
	add_parse(JOB_REASON, state_reason, "state_reason", NULL),
	add_skip(std_err),
	add_skip(std_in),
	add_skip(std_out),
	add_cparse(JOB_INFO_STDERR, "standard_error", NULL),
	add_cparse(JOB_INFO_STDIN, "standard_input", NULL),
	add_cparse(JOB_INFO_STDOUT, "standard_output", NULL),
	add_parse(TIMESTAMP_NO_VAL, submit_time, "submit_time", NULL),
	add_parse(TIMESTAMP_NO_VAL, suspend_time, "suspend_time", NULL),
	add_parse(STRING, system_comment, "system_comment", NULL),
	add_parse(UINT32_NO_VAL, time_limit, "time_limit", NULL),
	add_parse(UINT32_NO_VAL, time_min, "time_minimum", NULL),
	add_parse(UINT16_NO_VAL, threads_per_core, "threads_per_core", NULL),
	add_parse(STRING, tres_bind, "tres_bind", NULL),
	add_parse(STRING, tres_freq, "tres_freq", NULL),
	add_parse(STRING, tres_per_job, "tres_per_job", NULL),
	add_parse(STRING, tres_per_node, "tres_per_node", NULL),
	add_parse(STRING, tres_per_socket, "tres_per_socket", NULL),
	add_parse(STRING, tres_per_task, "tres_per_task", NULL),
	add_parse(STRING, tres_req_str, "tres_req_str", NULL),
	add_parse(STRING, tres_alloc_str, "tres_alloc_str", NULL),
	add_parse_overload(UINT32, user_id, 1, "user_id", NULL),
	add_parse_overload(USER_ID, user_id, 1, "user_name", NULL),
	add_parse(UINT32, wait4switch, "maximum_switch_wait_time", NULL),
	add_parse(STRING, wckey, "wckey", NULL),
	add_parse(STRING, work_dir, "current_working_directory", NULL),
};
#undef add_parse
#undef add_parse_overload
#undef add_parse_deprec
#undef add_cparse
#undef add_skip

static const flag_bit_t PARSER_FLAG_ARRAY(JOB_RES_CORE_STATUS)[] = {
	add_flag_equal(JOB_RES_CORE_INVALID, INFINITE64, "INVALID"),
	add_flag_equal(JOB_RES_CORE_UNALLOC, INFINITE64, "UNALLOCATED"),
	add_flag_bit(JOB_RES_CORE_ALLOC, "ALLOCATED"),
	add_flag_bit(JOB_RES_CORE_IN_USE , "IN_USE"),
};

#define add_parse_req(mtype, field, path, desc) \
	add_parser(JOB_RES_CORE_t, mtype, true, field, 0, path, desc)
static const parser_t PARSER_ARRAY(JOB_RES_CORE)[] = {
	add_parse_req(UINT32, index, "index", "Core index"),
	add_parse_req(JOB_RES_CORE_STATUS, status, "status", "Core status"),
};
#undef add_parse_req

#define add_parse_req(mtype, field, path, desc) \
	add_parser(JOB_RES_SOCKET_t, mtype, true, field, 0, path, desc)
static const parser_t PARSER_ARRAY(JOB_RES_SOCKET)[] = {
	add_parse_req(UINT32, index, "index", "Core index"),
	add_parse_req(JOB_RES_CORE_ARRAY, cores, "cores", "Core in socket"),
};
#undef add_parse_req

#define add_parse_req(mtype, field, path, desc) \
	add_parser(JOB_RES_NODE_t, mtype, true, field, 0, path, desc)
static const parser_t PARSER_ARRAY(JOB_RES_NODE)[] = {
	add_parse_req(UINT32, index, "index", "Node index"),
	add_parse_req(STRING, name, "name", "Node name"),
	add_parse_req(UINT16, cpus.count, "cpus/count", "Total number of CPUs assigned to job"),
	add_parse_req(UINT16, cpus.used, "cpus/used", "Total number of CPUs used by job"),
	add_parse_req(UINT64, memory.used, "memory/used", "Total memory (MiB) used by job"),
	add_parse_req(UINT64, memory.allocated, "memory/allocated", "Total memory (MiB) allocated to job"),
	add_parse_req(JOB_RES_SOCKET_ARRAY, sockets, "sockets", "Socket allocations in node"),
};
#undef add_parse_req

#define add_skip(field) \
	add_parser_skip(job_resources_t, field)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(job_resources_t, mtype, true, field, 0, path, desc)
#define add_cparse_req(mtype, path, desc) \
	add_complex_parser(job_resources_t, mtype, true, path, desc)
static const parser_t PARSER_ARRAY(JOB_RES)[] = {
	add_skip(core_bitmap),
	add_skip(core_bitmap_used),
	add_skip(cpu_array_cnt),
	add_skip(cpu_array_value),
	add_skip(cpu_array_reps),
	add_skip(cpus),
	add_skip(cpus_used),
	add_skip(cores_per_socket),
	add_parse_req(CR_TYPE, cr_type, "select_type", "Scheduling consumption resource selection type"),
	add_skip(memory_allocated),
	add_skip(memory_used),
	add_parse_req(UINT32, nhosts, "nodes/count", "Number of nodes assigned to job"),
	add_skip(node_bitmap),
	add_parse_req(NODE_CR_TYPE, node_req, "nodes/select_type", "Node scheduling selection request"),
	add_parse_req(STRING, nodes, "nodes/list", "host list for job"),
	add_parse_req(UINT32, ncpus, "cpus", "Number of processors in the allocation"),
	add_skip(sock_core_rep_count),
	add_skip(sockets_per_node),
	add_skip(tasks_per_node), /* not packed */
	add_parse_req(UINT16_NO_VAL, threads_per_core, "threads_per_core", "Number of processor threads per CPU core"),
	add_parse_req(BOOL, whole_node, "nodes/whole", "Job allocated full nodes"),
	add_cparse_req(JOB_RES_NODES, "nodes/allocation", "resource allocations by node"),
};
#undef add_parse_req
#undef add_cparse_req
#undef add_skip

#define add_parse(mtype, field, path, desc) \
	add_parser(controller_ping_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(CONTROLLER_PING)[] = {
	add_parse(STRING, hostname, "hostname", NULL),
	add_parse(CONTROLLER_PING_RESULT, pinged, "pinged", NULL),
	add_parse(UINT64, latency, "latency", NULL),
	add_parse(CONTROLLER_PING_MODE, offset, "mode", NULL),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(job_step_info_t, mtype, false, field, 0, path, desc)
#define add_skip(field) \
	add_parser_skip(job_step_info_t, field)
static const parser_t PARSER_ARRAY(STEP_INFO)[] = {
	add_parse(UINT32, array_job_id, "array/job_id", NULL),
	add_parse(UINT32, array_task_id, "array/task_id", NULL),
	add_parse(STRING, cluster, "cluster", NULL),
	add_parse(STRING, container, "container", NULL),
	add_parse(STRING, container_id, "container_id", NULL),
	add_parse(UINT32_NO_VAL, cpu_freq_min, "cpu/frequency/min", NULL),
	add_parse(UINT32_NO_VAL, cpu_freq_max, "cpu/frequency/max", NULL),
	add_parse(UINT32_NO_VAL, cpu_freq_gov, "cpu/frequency/governor", NULL),
	add_parse(STRING, cpus_per_tres, "tres/per/cpu", NULL),
	add_parse(STRING, mem_per_tres, "tres/per/memory", NULL),
	add_parse(STRING, name, "name", NULL),
	add_parse(STRING, network, "network", NULL),
	add_parse(STRING, nodes, "nodes", NULL),
	add_skip(node_inx),
	add_parse(UINT32, num_cpus, "number_cpus", NULL),
	add_parse(UINT32, num_tasks, "number_tasks", NULL),
	add_parse(STRING, partition, "partition", NULL),
	add_parse(STRING, resv_ports, "reserved_ports", NULL),
	add_parse(TIMESTAMP_NO_VAL, run_time, "time/running", NULL),
	add_parse(STRING, srun_host, "srun/host", NULL),
	add_parse(UINT32, srun_pid, "srun/pid", NULL),
	add_parse(TIMESTAMP_NO_VAL, start_time, "time/start", NULL),
	add_skip(start_protocol_ver),
	add_parse_bit_flag_array(job_step_info_t, JOB_STATE, false, state, "state", NULL),
	add_parse(SLURM_STEP_ID_STRING, step_id, "id", NULL),
	add_parse(STRING, submit_line, "submit_line", NULL),
	add_parse(TASK_DISTRIBUTION, task_dist, "task/distribution", NULL),
	add_parse(UINT32_NO_VAL, time_limit, "time/limit", NULL),
	add_parse(STRING, tres_alloc_str, "tres/allocation", NULL),
	add_parse(STRING, tres_bind, "tres/binding", NULL),
	add_parse(STRING, tres_freq, "tres/frequency", NULL),
	add_parse(STRING, tres_per_step, "tres/per/step", NULL),
	add_parse(STRING, tres_per_node, "tres/per/node", NULL),
	add_parse(STRING, tres_per_socket, "tres/per/socket", NULL),
	add_parse(STRING, tres_per_task, "tres/per/task", NULL),
	add_parse(USER_ID, user_id, "user", NULL),
};
#undef add_parse
#undef add_skip

#define add_parse(mtype, field, path, desc) \
	add_parser(partition_info_t, mtype, false, field, 0, path, desc)
#define add_parse_overload(mtype, field, overloads, path, desc) \
	add_parser(partition_info_t, mtype, false, field, overloads, path, desc)
#define add_skip(field) \
	add_parser_skip(partition_info_t, field)
static const parser_t PARSER_ARRAY(PARTITION_INFO)[] = {
	add_parse(STRING, allow_alloc_nodes, "nodes/allowed_allocation", NULL),
	add_parse(STRING, allow_accounts, "accounts/allowed", NULL),
	add_parse(STRING, allow_groups, "groups/allowed", NULL),
	add_parse(STRING, allow_qos, "qos/allowed", NULL),
	add_parse(STRING, alternate, "alternate", NULL),
	add_parse(STRING, billing_weights_str, "tres/billing_weights", NULL),
	add_parse(STRING, cluster_name, "cluster", NULL),
	add_parse(CR_TYPE, cr_type, "select_type", "Scheduler consumable resource selection types"),
	add_parse(UINT32, cpu_bind, "cpus/task_binding", NULL),
	add_parse_overload(UINT64, def_mem_per_cpu, 2, "defaults/memory_per_cpu", NULL),
	add_parse_overload(MEM_PER_CPUS, def_mem_per_cpu, 2, "defaults/partition_memory_per_cpu", NULL),
	add_parse_overload(MEM_PER_NODE, def_mem_per_cpu, 2, "defaults/partition_memory_per_node", NULL),
	add_parse(UINT32_NO_VAL, default_time, "defaults/time", NULL),
	add_parse(STRING, deny_accounts, "accounts/deny", NULL),
	add_parse(STRING, deny_qos, "qos/deny", NULL),
	add_skip(flags), //FIXME
	add_parse(UINT32, grace_time, "grace_time", NULL),
	add_skip(job_defaults_list), //FIXME - is this even packed?
	add_parse(STRING, job_defaults_str, "defaults/job", NULL),
	add_parse(UINT32_NO_VAL, max_cpus_per_node, "maximums/cpus_per_node", NULL),
	add_parse(UINT32_NO_VAL, max_cpus_per_socket, "maximums/cpus_per_socket", NULL),
	add_parse_overload(UINT64, max_mem_per_cpu, 2, "maximums/memory_per_cpu", NULL),
	add_parse_overload(MEM_PER_CPUS, max_mem_per_cpu, 2, "maximums/partition_memory_per_cpu", NULL),
	add_parse_overload(MEM_PER_NODE, max_mem_per_cpu, 2, "maximums/partition_memory_per_node", NULL),
	add_parse(UINT32_NO_VAL, max_nodes, "maximums/nodes", NULL),
	add_parse_overload(UINT16, max_share, 2, "maximums/shares", NULL),
	add_parse_overload(OVERSUBSCRIBE_JOBS, max_share, 2, "maximums/oversubscribe/jobs", NULL),
	add_parse_overload(OVERSUBSCRIBE_FLAGS, max_share, 2, "maximums/oversubscribe/flags", NULL),
	add_parse(UINT32_NO_VAL, max_time, "maximums/time", NULL),
	add_parse(UINT32, min_nodes, "minimums/nodes", NULL),
	add_parse(STRING, name, "name", NULL),
	add_skip(node_inx),
	add_parse(STRING, nodes, "nodes/configured", NULL),
	add_parse(STRING, nodesets, "node_sets", NULL),
	add_parse(UINT16_NO_VAL, over_time_limit, "maximums/over_time_limit", NULL),
	add_skip(preempt_mode), // FIXME
	add_parse(UINT16, priority_job_factor, "priority/job_factor", NULL),
	add_parse(UINT16, priority_tier, "priority/tier", NULL),
	add_parse(STRING, qos_char, "qos/assigned", NULL),
	add_parse(UINT16_NO_VAL, resume_timeout, "timeouts/resume", NULL),
	add_parse_bit_flag_array(partition_info_t, PARTITION_STATES, false, state_up, "partition/state", NULL),
	add_parse(UINT32_NO_VAL, suspend_time, "suspend_time", NULL),
	add_parse(UINT16_NO_VAL, suspend_timeout, "timeouts/suspend", NULL),
	add_parse(UINT32, total_cpus, "cpus/total", NULL),
	add_parse(UINT32, total_nodes, "nodes/total", NULL),
	add_parse(STRING, tres_fmt_str, "tres/configured", NULL),
};
#undef add_parse
#undef add_skip
#undef add_parse_overload

#define add_parse(mtype, field, path, desc) \
	add_parser(sinfo_data_t, mtype, false, field, 0, path, desc)
#define add_skip(field) \
	add_parser_skip(sinfo_data_t, field)
static const parser_t PARSER_ARRAY(SINFO_DATA)[] = {
	add_parse(UINT16, port, "port", NULL),
	add_parse_bit_flag_array(sinfo_data_t, NODE_STATES, false, node_state, "node/state", NULL),
	add_parse(UINT32, nodes_alloc, "nodes/allocated", NULL),
	add_parse(UINT32, nodes_idle, "nodes/idle", NULL),
	add_parse(UINT32, nodes_other, "nodes/other", NULL),
	add_parse(UINT32, nodes_total, "nodes/total", NULL),
	add_parse(UINT32, cpus_alloc, "cpus/allocated", NULL),
	add_parse(UINT32, cpus_idle, "cpus/idle", NULL),
	add_parse(UINT32, cpus_other, "cpus/other", NULL),
	add_parse(UINT32, cpus_total, "cpus/total", NULL),
	add_parse(UINT32, min_cpus, "cpus/minimum", NULL),
	add_parse(UINT32, max_cpus, "cpus/maximum", NULL),
	add_parse(UINT32, min_sockets, "sockets/minimum", NULL),
	add_parse(UINT32, max_sockets, "sockets/maximum", NULL),
	add_parse(UINT32, min_cores, "cores/minimum", NULL),
	add_parse(UINT32, max_cores, "cores/maximum", NULL),
	add_parse(UINT32, min_threads, "threads/minimum", NULL),
	add_parse(UINT32, max_threads, "threads/maximum", NULL),
	add_parse(UINT32, min_disk, "disk/minimum", NULL),
	add_parse(UINT32, max_disk, "disk/maximum", NULL),
	add_parse(UINT64, min_mem, "memory/minimum", NULL),
	add_parse(UINT64, max_mem, "memory/maximum", NULL),
	add_parse(UINT32, min_weight, "weight/minimum", NULL),
	add_parse(UINT32, max_weight, "weight/maximum", NULL),
	add_parse(UINT32, min_cpu_load, "cpus/load/minimum", NULL),
	add_parse(UINT32, max_cpu_load, "cpus/load/maximum", NULL),
	add_parse(UINT64_NO_VAL, min_free_mem, "memory/free/minimum", NULL),
	add_parse(UINT64_NO_VAL, max_free_mem, "memory/free/maximum", NULL),
	add_parse(UINT32_NO_VAL, max_cpus_per_node, "cpus/per_node/max", NULL),
	add_parse(UINT64, alloc_memory, "memory/allocated", NULL),
	add_parse(STRING, features, "features/total", NULL),
	add_parse(STRING, features_act, "features/active", NULL),
	add_parse(STRING, gres, "gres/total", NULL),
	add_parse(STRING, gres_used, "gres/used", NULL),
	add_parse(STRING, cluster_name, "cluster", NULL),
	add_parse(STRING, comment, "comment", NULL),
	add_parse(STRING, extra, "extra", NULL),
	add_parse(STRING, reason, "reason/description", NULL),
	add_parse(UINT64, reason_time, "reason/time", NULL),
	add_parse(STRING, resv_name, "reservation", NULL),
	add_parse(USER_ID, reason_uid, "reason/user", NULL),
	add_skip(version), /* already in meta */
	add_parse(HOSTLIST, hostnames, "nodes/hostnames", NULL),
	add_parse(HOSTLIST, node_addr, "nodes/addresses", NULL),
	add_parse(HOSTLIST, nodes, "nodes/nodes", NULL),
	add_parse(PARTITION_INFO_PTR, part_info, "partition", NULL),
	add_skip(part_inx),
};
#undef add_parse
#undef add_skip

#define add_parse(mtype, field, path, desc) \
	add_parser(acct_gather_energy_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(ACCT_GATHER_ENERGY)[] = {
	add_parse(UINT32, ave_watts, "average_watts", NULL),
	add_parse(UINT64, base_consumed_energy, "base_consumed_energy", NULL),
	add_parse(UINT64, consumed_energy, "consumed_energy", NULL),
	add_parse(UINT32_NO_VAL, current_watts, "current_watts", NULL),
	add_parse(UINT64, previous_consumed_energy, "previous_consumed_energy", NULL),
	add_parse(TIMESTAMP, poll_time, "last_collected", NULL),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(power_mgmt_data_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(POWER_MGMT_DATA)[] = {
	add_parse(UINT32_NO_VAL, cap_watts, "maximum_watts", NULL),
	add_parse(UINT32, current_watts, "current_watts", NULL),
	add_parse(UINT64, joule_counter, "total_energy", NULL),
	add_parse(UINT32, new_cap_watts, "new_maximum_watts", NULL),
	add_parse(UINT32, max_watts, "peak_watts", NULL),
	add_parse(UINT32, min_watts, "lowest_watts", NULL),
	add_parse(TIMESTAMP_NO_VAL, new_job_time, "new_job_time", NULL),
	add_parse(UINT16, state, "state", NULL),
	add_parse(TIMESTAMP, time_usec, "time_start_day", NULL),
};
#undef add_parse

static const flag_bit_t PARSER_FLAG_ARRAY(RESERVATION_FLAGS)[] = {
	add_flag_bit(RESERVE_FLAG_MAINT, "MAINT"),
	add_flag_bit(RESERVE_FLAG_NO_MAINT, "NO_MAINT"),
	add_flag_bit(RESERVE_FLAG_DAILY, "DAILY"),
	add_flag_bit(RESERVE_FLAG_NO_DAILY, "NO_DAILY"),
	add_flag_bit(RESERVE_FLAG_WEEKLY, "WEEKLY"),
	add_flag_bit(RESERVE_FLAG_NO_WEEKLY, "NO_WEEKLY"),
	add_flag_bit(RESERVE_FLAG_IGN_JOBS, "IGNORE_JOBS"),
	add_flag_bit(RESERVE_FLAG_NO_IGN_JOB, "NO_IGNORE_JOBS"),
	add_flag_bit(RESERVE_FLAG_ANY_NODES, "ANY_NODES"),
	add_flag_bit(RESERVE_FLAG_STATIC, "STATIC"),
	add_flag_bit(RESERVE_FLAG_NO_STATIC, "NO_STATIC"),
	add_flag_bit(RESERVE_FLAG_PART_NODES, "PART_NODES"),
	add_flag_bit(RESERVE_FLAG_NO_PART_NODES, "NO_PART_NODES"),
	add_flag_bit(RESERVE_FLAG_OVERLAP, "OVERLAP"),
	add_flag_bit(RESERVE_FLAG_SPEC_NODES, "SPEC_NODES"),
	add_flag_bit(RESERVE_FLAG_TIME_FLOAT, "TIME_FLOAT"),
	add_flag_bit(RESERVE_FLAG_REPLACE, "REPLACE"),
	add_flag_bit(RESERVE_FLAG_ALL_NODES, "ALL_NODES"),
	add_flag_bit(RESERVE_FLAG_PURGE_COMP, "PURGE_COMP"),
	add_flag_bit(RESERVE_FLAG_WEEKDAY, "WEEKDAY"),
	add_flag_bit(RESERVE_FLAG_NO_WEEKDAY, "NO_WEEKDAY"),
	add_flag_bit(RESERVE_FLAG_WEEKEND, "WEEKEND"),
	add_flag_bit(RESERVE_FLAG_NO_WEEKEND, "NO_WEEKEND"),
	add_flag_bit(RESERVE_FLAG_FLEX, "FLEX"),
	add_flag_bit(RESERVE_FLAG_NO_FLEX, "NO_FLEX"),
	add_flag_bit(RESERVE_FLAG_DUR_PLUS, "DURATION_PLUS"),
	add_flag_bit(RESERVE_FLAG_DUR_MINUS, "DURATION_MINUS"),
	add_flag_bit(RESERVE_FLAG_NO_HOLD_JOBS, "NO_HOLD_JOBS_AFTER_END"),
	add_flag_bit(RESERVE_FLAG_NO_PURGE_COMP, "NO_PURGE_COMP"),
	add_flag_bit(RESERVE_FLAG_MAGNETIC, "MAGNETIC"),
	add_flag_bit(RESERVE_FLAG_SKIP, "SKIP"),
	add_flag_bit(RESERVE_FLAG_HOURLY, "HOURLY"),
	add_flag_bit(RESERVE_FLAG_NO_HOURLY, "NO_HOURLY"),
	add_flag_bit(RESERVE_REOCCURRING, "REOCCURRING"),
};

#define add_parse(mtype, field, path, desc) \
	add_parser(resv_core_spec_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(RESERVATION_CORE_SPEC)[] = {
	add_parse(STRING, node_name, "node", NULL),
	add_parse(STRING, core_id, "core", NULL),
};
#undef add_parse

#define add_cparse(mtype, path, desc) \
	add_complex_parser(reserve_info_t, mtype, false, path, desc)
#define add_parse(mtype, field, path, desc) \
	add_parser(reserve_info_t, mtype, false, field, 0, path, desc)
#define add_skip(field) \
	add_parser_skip(reserve_info_t, field)
static const parser_t PARSER_ARRAY(RESERVATION_INFO)[] = {
	add_parse(STRING, accounts, "accounts", NULL),
	add_parse(STRING, burst_buffer, "burst_buffer", NULL),
	add_parse(UINT32, core_cnt, "core_count", NULL),
	add_skip(core_spec_cnt), /* parsed by INFO_CORE_SPEC */
	add_skip(core_spec), /* parsed by INFO_CORE_SPEC */
	add_cparse(RESERVATION_INFO_CORE_SPEC, "core_specializations", NULL),
	add_parse(TIMESTAMP_NO_VAL, end_time, "end_time", NULL),
	add_parse(STRING, features, "features", NULL),
	add_parse_bit_flag_array(reserve_info_t, RESERVATION_FLAGS, false, flags, "flags", NULL),
	add_parse(STRING, groups, "groups", NULL),
	add_parse(STRING, licenses, "licenses", NULL),
	add_parse(UINT32, max_start_delay, "max_start_delay", NULL),
	add_parse(STRING, name, "name", NULL),
	add_parse(UINT32, node_cnt, "node_count", NULL),
	add_skip(node_inx),
	add_parse(STRING, node_list, "node_list", NULL),
	add_parse(STRING, partition, "partition", NULL),
	add_parse(UINT32_NO_VAL, purge_comp_time, "purge_completed/time", NULL),
	add_parse(TIMESTAMP_NO_VAL, start_time, "start_time", NULL),
	add_parse(UINT32_NO_VAL, resv_watts, "watts", NULL),
	add_parse(STRING, tres_str, "tres", NULL),
	add_parse(STRING, users, "users", NULL),
};
#undef add_parse
#undef add_cparse
#undef add_skip
#undef add_removed

#define add_parse(mtype, field, path, desc) \
	add_parser(submit_response_msg_t, mtype, false, field, 0, path, desc)
#define add_parse_overload(mtype, field, overloads, path, desc) \
	add_parser(submit_response_msg_t, mtype, false, field, overloads, path, desc)
static const parser_t PARSER_ARRAY(JOB_SUBMIT_RESPONSE_MSG)[] = {
	add_parse(UINT32, job_id, "job_id", NULL),
	add_parse(STEP_ID, step_id, "step_id", NULL),
	add_parse_overload(UINT32, error_code, 1, "error_code", NULL),
	add_parse_overload(ERROR, error_code, 1, "error", NULL),
	add_parse(STRING, job_submit_user_msg, "job_submit_user_msg", NULL),
};
#undef add_parse_overload
#undef add_parse

/* flag values based on output of slurm_sprint_cpu_bind_type() */
static const flag_bit_t PARSER_FLAG_ARRAY(CPU_BINDING_FLAGS)[] = {
	add_flag_equal(CPU_BIND_TO_THREADS, CPU_BIND_T_TO_MASK, "CPU_BIND_TO_THREADS"),
	add_flag_equal(CPU_BIND_TO_CORES, CPU_BIND_T_TO_MASK, "CPU_BIND_TO_CORES"),
	add_flag_equal(CPU_BIND_TO_SOCKETS, CPU_BIND_T_TO_MASK, "CPU_BIND_TO_SOCKETS"),
	add_flag_equal(CPU_BIND_TO_LDOMS, CPU_BIND_T_TO_MASK, "CPU_BIND_TO_LDOMS"),
	add_flag_equal(CPU_BIND_NONE, CPU_BIND_T_MASK, "CPU_BIND_NONE"),
	add_flag_equal(CPU_BIND_RANK, CPU_BIND_T_MASK, "CPU_BIND_RANK"),
	add_flag_equal(CPU_BIND_MAP, CPU_BIND_T_MASK, "CPU_BIND_MAP"),
	add_flag_equal(CPU_BIND_MASK, CPU_BIND_T_MASK, "CPU_BIND_MASK"),
	add_flag_equal(CPU_BIND_LDRANK, CPU_BIND_T_MASK, "CPU_BIND_LDRANK"),
	add_flag_equal(CPU_BIND_LDMAP, CPU_BIND_T_MASK, "CPU_BIND_LDMAP"),
	add_flag_equal(CPU_BIND_LDMASK, CPU_BIND_T_MASK, "CPU_BIND_LDMASK"),
	add_flag_masked_bit(CPU_BIND_VERBOSE, CPU_BIND_VERBOSE, "VERBOSE"),
	add_flag_masked_bit(CPU_BIND_ONE_THREAD_PER_CORE, CPU_BIND_ONE_THREAD_PER_CORE, "CPU_BIND_ONE_THREAD_PER_CORE"),
};

static const flag_bit_t PARSER_FLAG_ARRAY(CRON_ENTRY_FLAGS)[] = {
	add_flag_bit(CRON_WILD_MINUTE, "WILD_MINUTE"),
	add_flag_bit(CRON_WILD_HOUR, "WILD_HOUR"),
	add_flag_bit(CRON_WILD_DOM, "WILD_DAY_OF_MONTH"),
	add_flag_bit(CRON_WILD_MONTH, "WILD_MONTH"),
	add_flag_bit(CRON_WILD_DOW, "WILD_DAY_OF_WEEK"),
};

#define add_parse(mtype, field, path, desc) \
	add_parser(cron_entry_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(CRON_ENTRY)[] = {
	add_parse_bit_flag_array(cron_entry_t, CRON_ENTRY_FLAGS, false, flags, "flags", NULL),
	add_parse(BITSTR, minute, "minute", NULL),
	add_parse(BITSTR, hour, "hour", NULL),
	add_parse(BITSTR, day_of_month, "day_of_month", NULL),
	add_parse(BITSTR, month, "month", NULL),
	add_parse(BITSTR, day_of_week, "day_of_week", NULL),
	add_parse(STRING, cronspec, "specification", NULL),
	add_parse(STRING, command, "command", NULL),
	add_parse(UINT32, line_start, "line/start", NULL),
	add_parse(UINT32, line_end, "line/end", NULL),
};
#undef add_parse

static const flag_bit_t PARSER_FLAG_ARRAY(MEMORY_BINDING_TYPE)[] = {
	add_flag_equal(MEM_BIND_NONE, MEM_BIND_TYPE_MASK, "NONE"),
	add_flag_equal(MEM_BIND_RANK, MEM_BIND_TYPE_MASK, "RANK"),
	add_flag_equal(MEM_BIND_MAP, MEM_BIND_TYPE_MASK, "MAP"),
	add_flag_equal(MEM_BIND_MASK, MEM_BIND_TYPE_MASK, "MASK"),
	add_flag_equal(MEM_BIND_LOCAL, MEM_BIND_TYPE_MASK, "LOCAL"),
	add_flag_masked_bit(MEM_BIND_VERBOSE, MEM_BIND_VERBOSE, "VERBOSE"),
	add_flag_masked_bit(MEM_BIND_SORT, MEM_BIND_TYPE_FLAGS_MASK, "SORT"),
	add_flag_masked_bit(MEM_BIND_PREFER, MEM_BIND_TYPE_FLAGS_MASK, "PREFER"),
};

static const flag_bit_t PARSER_FLAG_ARRAY(OPEN_MODE)[] = {
	add_flag_bit(OPEN_MODE_APPEND, "APPEND"),
	add_flag_bit(OPEN_MODE_TRUNCATE, "TRUNCATE"),
};

static const flag_bit_t PARSER_FLAG_ARRAY(WARN_FLAGS)[] = {
	add_flag_bit(KILL_JOB_BATCH, "BATCH_JOB"),
	add_flag_bit(KILL_ARRAY_TASK, "ARRAY_TASK"),
	add_flag_bit(KILL_STEPS_ONLY, "FULL_STEPS_ONLY"),
	add_flag_bit(KILL_FULL_JOB, "FULL_JOB"),
	add_flag_bit(KILL_FED_REQUEUE, "FEDERATION_REQUEUE"),
	add_flag_bit(KILL_HURRY, "HURRY"),
	add_flag_bit(KILL_OOM, "OUT_OF_MEMORY"),
	add_flag_bit(KILL_NO_SIBS, "NO_SIBLING_JOBS"),
	add_flag_bit(KILL_JOB_RESV, "RESERVATION_JOB"),
	add_flag_bit(WARN_SENT, "WARNING_SENT"),
};

static const flag_bit_t PARSER_FLAG_ARRAY(X11_FLAGS)[] = {
	add_flag_bit(X11_FORWARD_ALL, "FORWARD_ALL_NODES"),
	add_flag_bit(X11_FORWARD_BATCH, "BATCH_NODE"),
	add_flag_bit(X11_FORWARD_FIRST, "FIRST_NODE"),
	add_flag_bit(X11_FORWARD_LAST, "LAST_NODE"),
};

#define add_cparse(mtype, path, desc) \
	add_complex_parser(job_desc_msg_t, mtype, false, path, desc)
#define add_parse(mtype, field, path, desc) \
	add_parser(job_desc_msg_t, mtype, false, field, 0, path, desc)
#define add_parse_overload(mtype, field, overloads, path, desc) \
	add_parser(job_desc_msg_t, mtype, false, field, overloads, path, desc)
#define add_parse_deprec(mtype, field, overloads, path, desc, deprec) \
	add_parser_deprec(job_desc_msg_t, mtype, false, field, overloads, path, desc, deprec)
#define add_skip(field) \
	add_parser_skip(job_desc_msg_t, field)
#define add_flags(mtype, field, path, desc) \
	add_parse_bit_flag_array(job_desc_msg_t, mtype, false, field, path, desc)
#define add_removed(mtype, path, desc, deprec) \
	add_parser_removed(job_desc_msg_t, mtype, false, path, desc, deprec)
static const parser_t PARSER_ARRAY(JOB_DESC_MSG)[] = {
	add_parse(STRING, account, "account", NULL),
	add_parse(STRING, acctg_freq, "account_gather_frequency", NULL),
	add_parse(STRING, admin_comment, "admin_comment", NULL),
	add_parse(STRING, alloc_node, "allocation_node_list", NULL),
	add_parse(UINT16, alloc_resp_port, "allocation_node_port", NULL),
	add_skip(alloc_sid),
	add_skip(argc),
	add_skip(argv),
	add_cparse(JOB_DESC_MSG_ARGV, "argv", NULL),
	add_parse(STRING, array_inx, "array", NULL),
	add_skip(array_bitmap),
	add_parse(STRING, batch_features, "batch_features", NULL),
	add_parse(TIMESTAMP_NO_VAL, begin_time, "begin_time", NULL),
	add_flags(JOB_FLAGS, bitflags, "flags", NULL),
	add_parse(STRING, burst_buffer, "burst_buffer", NULL),
	add_parse(STRING, clusters, "clusters", NULL),
	add_parse(STRING, cluster_features, "cluster_constraint", NULL),
	add_parse(STRING, comment, "comment", NULL),
	add_parse(BOOL16, contiguous, "contiguous", NULL),
	add_parse(STRING, container, "container", NULL),
	add_parse(STRING, container_id, "container_id", NULL),
	add_parse_overload(CORE_SPEC, core_spec, 1, "core_specification", NULL),
	add_parse_overload(THREAD_SPEC, core_spec, 1, "thread_specification", NULL),
	add_parse(STRING, cpu_bind, "cpu_binding", NULL),
	add_flags(CPU_BINDING_FLAGS, cpu_bind_type, "cpu_binding_flags", NULL),
	add_cparse(JOB_DESC_MSG_CPU_FREQ, "cpu_frequency", NULL),
	add_skip(cpu_freq_min),
	add_skip(cpu_freq_max),
	add_skip(cpu_freq_gov),
	add_parse(STRING, cpus_per_tres, "cpus_per_tres", NULL),
	add_parse(CRON_ENTRY_PTR, crontab_entry, "crontab", NULL),
	add_parse(TIMESTAMP, deadline, "deadline", NULL),
	add_parse(UINT32, delay_boot, "delay_boot", NULL),
	add_parse(STRING, dependency, "dependency", NULL),
	add_parse(TIMESTAMP, end_time, "end_time", NULL),
	add_cparse(JOB_DESC_MSG_ENV, "environment", NULL),
	add_skip(environment),
	add_cparse(JOB_DESC_MSG_RLIMIT_CPU, "rlimits/cpu", "Per-process CPU limit, in seconds."),
	add_cparse(JOB_DESC_MSG_RLIMIT_FSIZE, "rlimits/fsize", "Largest file that can be created, in bytes."),
	add_cparse(JOB_DESC_MSG_RLIMIT_DATA, "rlimits/data", "Maximum size of data segment, in bytes. "),
	add_cparse(JOB_DESC_MSG_RLIMIT_STACK, "rlimits/stack", "Maximum size of stack segment, in bytes."),
	add_cparse(JOB_DESC_MSG_RLIMIT_CORE, "rlimits/core", "Largest core file that can be created, in bytes."),
	add_cparse(JOB_DESC_MSG_RLIMIT_RSS, "rlimits/rss", "Largest resident set size, in bytes. This affects swapping; processes that are exceeding their resident set size will be more likely to have physical memory taken from them."),
	add_cparse(JOB_DESC_MSG_RLIMIT_NPROC, "rlimits/nproc", "Number of processes."),
	add_cparse(JOB_DESC_MSG_RLIMIT_NOFILE, "rlimits/nofile", "Number of open files."),
	add_cparse(JOB_DESC_MSG_RLIMIT_MEMLOCK, "rlimits/memlock", "Locked-in-memory address space"),
	add_cparse(JOB_DESC_MSG_RLIMIT_AS, "rlimits/as", "Address space limit."),
	add_skip(env_hash),
	add_skip(env_size),
	add_parse(CSV_STRING, exc_nodes, "excluded_nodes", NULL),
	add_parse(STRING, extra, "extra", NULL),
	add_parse(STRING, features, "constraints", NULL),
	add_skip(fed_siblings_active),
	add_skip(fed_siblings_viable),
	add_parse(GROUP_ID, group_id, "group_id", NULL),
	add_parse(UINT32, het_job_offset, "hetjob_group", NULL),
	add_parse(BOOL16, immediate, "immediate", NULL),
	add_parse(UINT32, job_id, "job_id", NULL),
	add_skip(job_id_str),
	add_parse(BOOL16, kill_on_node_fail, "kill_on_node_fail", NULL),
	add_parse(STRING, licenses, "licenses", NULL),
	add_skip(licenses_tot),
	add_flags(JOB_MAIL_FLAGS, mail_type, "mail_type", NULL),
	add_parse(STRING, mail_user, "mail_user", NULL),
	add_parse(STRING, mcs_label, "mcs_label", NULL),
	add_parse(STRING, mem_bind, "memory_binding", NULL),
	add_flags(MEMORY_BINDING_TYPE, mem_bind_type, "memory_binding_type", NULL),
	add_parse(STRING, mem_per_tres, "memory_per_tres", NULL),
	add_parse(STRING, name, "name", NULL),
	add_parse(STRING, network, "network", NULL),
	add_parse(NICE, nice, "nice", NULL),
	add_parse(UINT32, num_tasks, "tasks", NULL),
	add_flags(OPEN_MODE, open_mode, "open_mode", NULL),
	add_skip(origin_cluster),
	add_parse(UINT16, other_port, "reserve_ports", NULL),
	add_parse(BOOL, overcommit, "overcommit", NULL),
	add_parse(STRING, partition, "partition", NULL),
	add_parse(UINT16, plane_size, "distribution_plane_size", NULL),
	add_removed(POWER_FLAGS, "power_flags", NULL, SLURM_24_05_PROTOCOL_VERSION),
	add_parse(STRING, prefer, "prefer", NULL),
	add_parse_overload(HOLD, priority, 1, "hold", "Hold (true) or release (false) job"),
	add_parse_overload(UINT32_NO_VAL, priority, 1, "priority", "Request specific job priority"),
	add_parse(ACCT_GATHER_PROFILE, profile, "profile", NULL),
	add_parse(STRING, qos, "qos", NULL),
	add_parse(BOOL16, reboot, "reboot", NULL),
	add_skip(resp_host),
	add_skip(restart_cnt),
	add_parse(CSV_STRING, req_nodes, "required_nodes", NULL),
	add_parse(BOOL16, requeue, "requeue", NULL),
	add_parse(STRING, reservation, "reservation", NULL),
	add_parse(STRING, script, "script", NULL),
	add_skip(script_buf),
	add_skip(script_hash),
	add_parse_overload(JOB_SHARED, shared, 2, "shared", NULL),
	add_parse_deprec(JOB_EXCLUSIVE, shared, 2, "exclusive", NULL, SLURM_23_11_PROTOCOL_VERSION),
	add_parse_deprec(BOOL16, shared, 2, "oversubscribe", NULL, SLURM_23_11_PROTOCOL_VERSION),
	add_parse(UINT32, site_factor, "site_factor", NULL),
	add_cparse(JOB_DESC_MSG_SPANK_ENV, "spank_environment", NULL),
	add_skip(spank_job_env),
	add_skip(spank_job_env_size),
	add_skip(submit_line),
	add_parse(TASK_DISTRIBUTION, task_dist, "distribution", NULL),
	add_parse(UINT32_NO_VAL, time_limit, "time_limit", NULL),
	add_parse(UINT32_NO_VAL, time_min, "time_minimum", NULL),
	add_parse(STRING, tres_bind, "tres_bind", NULL),
	add_parse(STRING, tres_freq, "tres_freq", NULL),
	add_parse(STRING, tres_per_job, "tres_per_job", NULL),
	add_parse(STRING, tres_per_node, "tres_per_node", NULL),
	add_parse(STRING, tres_per_socket, "tres_per_socket", NULL),
	add_parse(STRING, tres_per_task, "tres_per_task", NULL),
	add_parse(USER_ID, user_id, "user_id", NULL),
	add_parse(BOOL16_NO_VAL, wait_all_nodes, "wait_all_nodes", NULL),
	add_flags(WARN_FLAGS, warn_flags, "kill_warning_flags", NULL),
	add_parse(SIGNAL, warn_signal, "kill_warning_signal", NULL),
	add_parse(UINT16_NO_VAL, warn_time, "kill_warning_delay", NULL),
	add_parse(STRING, work_dir, "current_working_directory", NULL),
	add_parse(UINT16, cpus_per_task, "cpus_per_task", NULL),
	add_parse(UINT32, min_cpus, "minimum_cpus", NULL),
	add_parse(UINT32, max_cpus, "maximum_cpus", NULL),
	add_cparse(JOB_DESC_MSG_NODES, "nodes", NULL),
	add_parse(UINT32, min_nodes, "minimum_nodes", NULL),
	add_parse(UINT32, max_nodes, "maximum_nodes", NULL),
	add_parse(UINT16, boards_per_node, "minimum_boards_per_node", NULL),
	add_parse(UINT16, sockets_per_board, "minimum_sockets_per_board", NULL),
	add_parse(UINT16, sockets_per_node, "sockets_per_node", NULL),
	add_parse(UINT16, threads_per_core, "threads_per_core", NULL),
	add_parse(UINT16, ntasks_per_node, "tasks_per_node", NULL),
	add_parse(UINT16, ntasks_per_socket, "tasks_per_socket", NULL),
	add_parse(UINT16, ntasks_per_core, "tasks_per_core", NULL),
	add_parse(UINT16, ntasks_per_board, "tasks_per_board", NULL),
	add_parse(UINT16, ntasks_per_tres, "ntasks_per_tres", NULL),
	add_parse(UINT16, pn_min_cpus, "minimum_cpus_per_node", NULL),
	add_parse_overload(MEM_PER_CPUS, pn_min_memory, 1, "memory_per_cpu", NULL),
	add_parse_overload(MEM_PER_NODE, pn_min_memory, 1, "memory_per_node", NULL),
	add_parse(UINT32, pn_min_tmp_disk, "temporary_disk_per_node", NULL),
	add_parse(STRING, req_context, "selinux_context", NULL),
	add_parse(UINT32_NO_VAL, req_switch, "required_switches", NULL),
	add_parse(STRING, std_err, "standard_error", NULL),
	add_parse(STRING, std_in, "standard_input", NULL),
	add_parse(STRING, std_out, "standard_output", NULL),
	add_skip(tres_req_cnt),
	add_parse(UINT32, wait4switch, "wait_for_switch", NULL),
	add_parse(STRING, wckey, "wckey", NULL),
	add_flags(X11_FLAGS, x11, "x11", NULL),
	add_parse(STRING, x11_magic_cookie, "x11_magic_cookie", NULL),
	add_parse(STRING, x11_target, "x11_target_host", NULL),
	add_parse(UINT16, x11_target_port, "x11_target_port", NULL),
};
#undef add_parse
#undef add_parse_overload
#undef add_parse_deprec
#undef add_cparse
#undef add_skip
#undef add_flags
#undef add_removed

#define add_parse(mtype, field, path, desc) \
	add_parser(update_node_msg_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(UPDATE_NODE_MSG)[] = {
	add_parse(STRING, comment, "comment", "arbitrary comment"),
	add_parse(UINT32, cpu_bind, "cpu_bind", "default CPU binding type"),
	add_parse(STRING, extra, "extra", "arbitrary string"),
	add_parse(CSV_STRING, features, "features", "new available feature for node"),
	add_parse(CSV_STRING, features_act, "features_act", "new active feature for node"),
	add_parse(STRING, gres, "gres", "new generic resources for node"),
	add_parse(HOSTLIST_STRING, node_addr, "address", "communication name"),
	add_parse(HOSTLIST_STRING, node_hostname, "hostname", "node's hostname"),
	add_parse(HOSTLIST_STRING, node_names, "name", "node to update"),
	add_parse(NODE_STATES, node_state, "state", "assign new node state"),
	add_parse(STRING, reason, "reason", "reason for node being DOWN or DRAINING"),
	add_parse(USER_ID, reason_uid, "reason_uid", "user ID of sending (needed if user root is sending message)"),
	add_parse(UINT32_NO_VAL, resume_after, "resume_after", "automatically resume DOWN or DRAINED node after this amount of seconds"),
	add_parse(UINT32_NO_VAL, weight, "weight", "new weight for node"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_resp_meta_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_META)[] = {
	add_parse(STRING, plugin.type, "plugin/type", "Slurm plugin type (if applicable)"),
	add_parse(STRING, plugin.name, "plugin/name", "Slurm plugin name (if applicable)"),
	add_parse(STRING, plugin.data_parser, "plugin/data_parser", "Slurm data_parser plugin"),
	add_parse(STRING, plugin.accounting_storage, "plugin/accounting_storage", "Slurm accounting plugin"),
	add_parse(STRING, client.source, "client/source", "Client source description"),
	add_parse(USER_ID, client.uid, "client/user", "Client user (if known)"),
	add_parse(GROUP_ID, client.gid, "client/group", "Client group (if known)"),
	add_parse(STRING_ARRAY, command, "command", "CLI command (if applicable)"),
	add_parse(STRING, slurm.version.major, "slurm/version/major", "Slurm release major version"),
	add_parse(STRING, slurm.version.micro, "slurm/version/micro", "Slurm release micro version"),
	add_parse(STRING, slurm.version.minor, "slurm/version/minor", "Slurm release minor version"),
	add_parse(STRING, slurm.release, "slurm/release", "Slurm release string"),
	add_parse(STRING, slurm.cluster, "slurm/cluster", "Slurm cluster name"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_resp_error_t, mtype, false, field, 0, path, desc)
#define add_parse_overload(mtype, field, overloads, path, desc) \
	add_parser(openapi_resp_error_t, mtype, false, field, overloads, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_ERROR)[] = {
	add_parse(STRING, description, "description", "Long form error description"),
	add_parse_overload(INT32, num, 1, "error_number", "Slurm numeric error identifier"),
	add_parse_overload(ERROR, num, 1, "error", "Short form error description"),
	add_parse(STRING, source, "source", "Source of error or where error was first detected"),
};
#undef add_parse
#undef add_parse_overload

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_resp_warning_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_WARNING)[] = {
	add_parse(STRING, description, "description", "Long form warning description"),
	add_parse(STRING, source, "source", "Source of warning or where warning was first detected"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_instance_cond_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(INSTANCE_CONDITION)[] = {
	add_parse(CSV_STRING_LIST, cluster_list, "cluster", "CSV clusters list"),
	add_parse(CSV_STRING_LIST, extra_list, "extra", "CSV extra list"),
	add_parse(CSV_STRING_LIST, format_list, "format", "CSV format list"),
	add_parse(CSV_STRING_LIST, instance_id_list, "instance_id", "CSV instance_id list"),
	add_parse(CSV_STRING_LIST, instance_type_list, "instance_type", "CSV instance_type list"),
	add_parse(STRING, node_list, "node_list", "ranged node string"),
	add_parse(TIMESTAMP, time_end, "time_end", "time end UNIX timestamp"),
	add_parse(TIMESTAMP, time_start, "time_start", "time start UNIX timestamp"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_job_submit_request_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(JOB_SUBMIT_REQ)[] = {
	add_parse(STRING, script, "script", "batch job script"),
	add_parse(JOB_DESC_MSG_LIST, jobs, "jobs", "HetJob description"),
	add_parse(JOB_DESC_MSG_PTR, job, "job", "Job description"),
};
#undef add_parse

#define add_flag(flag_value, flag_string, hidden, desc)               \
	add_flag_bit_entry(FLAG_BIT_TYPE_BIT, XSTRINGIFY(flag_value), \
			   flag_value, INFINITE64,                    \
			   XSTRINGIFY(INFINITE64), flag_string,       \
			   hidden, desc)
static const flag_bit_t PARSER_FLAG_ARRAY(JOB_CONDITION_FLAGS)[] = {
	add_flag(JOBCOND_FLAG_DUP, "show_duplicates", false, NULL),
	add_flag(JOBCOND_FLAG_NO_STEP, "skip_steps", false, NULL),
	add_flag(JOBCOND_FLAG_NO_TRUNC, "disable_truncate_usage_time", false, NULL),
	add_flag(JOBCOND_FLAG_RUNAWAY, "run_away_jobs", true, NULL),
	add_flag(JOBCOND_FLAG_WHOLE_HETJOB, "whole_hetjob", false, NULL),
	add_flag(JOBCOND_FLAG_NO_WHOLE_HETJOB, "disable_whole_hetjob", false, NULL),
	add_flag(JOBCOND_FLAG_NO_WAIT, "disable_wait_for_result", false, NULL),
	add_flag(JOBCOND_FLAG_NO_DEFAULT_USAGE, "usage_time_as_submit_time", false, NULL),
	add_flag(JOBCOND_FLAG_SCRIPT, "show_batch_script", false, NULL),
	add_flag(JOBCOND_FLAG_ENV, "show_job_environment", false, NULL),
};
#undef add_flag

#define add_flag(flag_value, flag_string, hidden, desc)               \
	add_flag_bit_entry(FLAG_BIT_TYPE_BIT, XSTRINGIFY(flag_value), \
			   flag_value, INFINITE64,                    \
			   XSTRINGIFY(INFINITE64), flag_string,       \
			   hidden, desc)
#define add_flag_eq(flag_value, flag_string, hidden, desc)            \
	add_flag_bit_entry(FLAG_BIT_TYPE_EQUAL,                       \
			   XSTRINGIFY(flag_value), flag_value,        \
			   INFINITE, XSTRINGIFY(INFINITE),            \
			   flag_string, hidden, desc)
static const flag_bit_t PARSER_FLAG_ARRAY(JOB_CONDITION_DB_FLAGS)[] = {
	add_flag_eq(SLURMDB_JOB_FLAG_NONE, "none", true, NULL),
	add_flag_eq(SLURMDB_JOB_CLEAR_SCHED, "clear_scheduling", true, NULL),
	add_flag(SLURMDB_JOB_FLAG_NOTSET, "scheduler_unset", false, NULL),
	add_flag(SLURMDB_JOB_FLAG_SUBMIT, "scheduled_on_submit", false, NULL),
	add_flag(SLURMDB_JOB_FLAG_SCHED, "scheduled_by_main", false, NULL),
	add_flag(SLURMDB_JOB_FLAG_BACKFILL, "scheduled_by_backfill", false, NULL),
	add_flag(SLURMDB_JOB_FLAG_START_R, "job_started", false, NULL),
};
#undef add_flag
#undef add_flag_eq

#define add_cparse(mtype, path, desc) \
	add_complex_parser(slurmdb_job_cond_t, mtype, false, path, desc)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_job_cond_t, mtype, false, field, 0, path, desc)
#define add_flags(mtype, field) \
	add_parse_bit_eflag_array(slurmdb_job_cond_t, mtype, field, NULL)
static const parser_t PARSER_ARRAY(JOB_CONDITION)[] = {
	add_parse(CSV_STRING_LIST, acct_list, "account", "CSV account list"),
	add_parse(CSV_STRING_LIST, associd_list, "association", "CSV association list"),
	add_parse(CSV_STRING_LIST, cluster_list, "cluster", "CSV cluster list"),
	add_parse(CSV_STRING_LIST, constraint_list, "constraints", "CSV constraint list"),
	add_parse(UINT32_NO_VAL, cpus_max, "cpus_max", "number of cpus high range"),
	add_parse(UINT32_NO_VAL, cpus_min, "cpus_min", "number of cpus low range"),
	add_flags(JOB_CONDITION_DB_FLAGS, db_flags),
	add_parse(INT32, exitcode, "exit_code", "job exit code (numeric)"),
	add_flags(JOB_CONDITION_FLAGS, flags),
	add_parse(CSV_STRING_LIST, format_list, "format", "CSV format list"),
	add_parse(GROUP_ID_STRING_LIST, groupid_list, "groups", "CSV group list"),
	add_parse(CSV_STRING_LIST, jobname_list, "job_name", "CSV job name list"),
	add_parse(UINT32_NO_VAL, nodes_max, "nodes_max", "number of nodes high range"),
	add_parse(UINT32_NO_VAL, nodes_min, "nodes_min", "number of nodes low range"),
	add_parse(CSV_STRING_LIST, partition_list, "partition", "CSV partition name list"),
	add_parse(CSV_STRING_LIST, qos_list, "qos", "CSV QOS name list"),
	add_parse(CSV_STRING_LIST, reason_list, "reason", "CSV reason list"),
	add_parse(CSV_STRING_LIST, resv_list, "reservation", "CSV reservation name list"),
	add_parse(CSV_STRING_LIST, resvid_list, "reservation_id", "CSV reservation ID list"),
	add_parse(JOB_STATE_ID_STRING_LIST, state_list, "state", "CSV state list"),
	add_parse(SELECTED_STEP_LIST, step_list, "step", "CSV step id list"),
	add_parse(UINT32_NO_VAL, timelimit_max, "timelimit_max", "maximum timelimit (seconds)"),
	add_parse(UINT32_NO_VAL, timelimit_min, "timelimit_min", "minimum timelimit (seconds)"),
	add_parse(TIMESTAMP, usage_end, "end_time", "usage end timestamp"),
	add_parse(TIMESTAMP, usage_start, "start_time", "usage start timestamp"),
	add_cparse(JOB_CONDITION_SUBMIT_TIME, "submit_time", "submit time timestamp"),
	add_parse(STRING, used_nodes, "node", "ranged node string where jobs ran"),
	add_parse(USER_ID_STRING_LIST, userid_list, "users", "CSV user name list"),
	add_parse(CSV_STRING_LIST, wckey_list, "wckey", "CSV wckey list"),
};
#undef add_parse
#undef add_cparse
#undef add_flags

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_qos_cond_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(QOS_CONDITION)[] = {
	add_parse(CSV_STRING_LIST, description_list, "description", "CSV description list"),
	add_parse(QOS_ID_STRING_CSV_LIST, id_list, "id", "CSV QOS id list"),
	add_parse(CSV_STRING_LIST, format_list, "format", "CSV format list"),
	add_parse(QOS_NAME_CSV_LIST, name_list, "name", "CSV QOS name list"),
	add_parse_bit_flag_array(slurmdb_qos_cond_t, QOS_PREEMPT_MODES, false, preempt_mode, "preempt_mode", NULL),
	add_parse(BOOL16, with_deleted, "with_deleted", "Include deleted QOS"),
};
#undef add_parse

#define add_skip(field) \
	add_parser_skip(slurmdb_add_assoc_cond_t, field)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_add_assoc_cond_t, mtype, true, field, 0, path, desc)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_add_assoc_cond_t, mtype, false, field, 0, path, desc)
/*
 * Should mirror the structure of slurmdb_add_assoc_cond_t,
 * intended for use with slurmdb_accounts_add_cond().
 */
static const parser_t PARSER_ARRAY(ACCOUNTS_ADD_COND)[] = {
	add_parse_req(CSV_STRING_LIST, acct_list, "accounts", "CSV accounts list"),
	add_parse(ASSOC_REC_SET, assoc, "association", "Association limits and options"),
	add_parse(CSV_STRING_LIST, cluster_list, "clusters", "CSV clusters list"),
	add_skip(default_acct),
	add_skip(partition_list),
	add_skip(user_list),
	add_skip(wckey_list),
};
#undef add_parse
#undef add_parse_req
#undef add_skip

#define add_skip(field) \
	add_parser_skip(slurmdb_add_assoc_cond_t, field)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_add_assoc_cond_t, mtype, true, field, 0, path, desc)
#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_add_assoc_cond_t, mtype, false, field, 0, path, desc)
/*
 * Should mirror the structure of slurmdb_add_assoc_cond_t,
 * intended for use with slurmdb_users_add_cond().
 */
static const parser_t PARSER_ARRAY(USERS_ADD_COND)[] = {
	add_parse(CSV_STRING_LIST, acct_list, "accounts", "CSV accounts list"),
	add_parse(ASSOC_REC_SET, assoc, "association", "Association limits and options"),
	add_parse(CSV_STRING_LIST, cluster_list, "clusters", "CSV clusters list"),
	add_skip(default_acct),
	add_parse(CSV_STRING_LIST, partition_list, "partitions", "CSV partitions list"),
	add_parse_req(CSV_STRING_LIST, user_list, "users", "CSV users list"),
	add_parse(CSV_STRING_LIST, wckey_list, "wckeys", "CSV WCKeys list"),
};
#undef add_parse
#undef add_parse_req
#undef add_skip

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_assoc_cond_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(ASSOC_CONDITION)[] = {
	add_parse(CSV_STRING_LIST, acct_list, "account", "CSV accounts list"),
	add_parse(CSV_STRING_LIST, cluster_list, "cluster", "CSV clusters list"),
	add_parse(QOS_ID_STRING_CSV_LIST, def_qos_id_list, "default_qos", "CSV QOS list"),
	add_parse(CSV_STRING_LIST, format_list, "format", "CSV format list"),
	add_parse(ASSOC_ID_STRING_CSV_LIST, id_list, "id", "CSV id list"),
	add_parse(BOOL16, only_defs, "only_defaults", "filter to only defaults"),
	add_parse(CSV_STRING_LIST, parent_acct_list, "parent_account", "CSV names of parent account"),
	add_parse(CSV_STRING_LIST, partition_list, "partition", "CSV partition name list"),
	add_parse(QOS_ID_STRING_CSV_LIST, qos_list, "qos", "CSV QOS list"),
	add_parse(TIMESTAMP, usage_end, "usage_end", "usage end UNIX timestamp"),
	add_parse(TIMESTAMP, usage_start, "usage_start", "usage start UNIX timestamp"),
	add_parse(CSV_STRING_LIST, user_list, "user", "CSV user list"),
	add_parse(BOOL16, with_usage, "with_usage", "fill in usage"),
	add_parse(BOOL16, with_deleted, "with_deleted", "return deleted associations"),
	add_parse(BOOL16, with_raw_qos, "with_raw_qos", "return a raw qos or delta_qos"),
	add_parse(BOOL16, with_sub_accts, "with_sub_accts", "return sub acct information also"),
	add_parse(BOOL16, without_parent_info, "without_parent_info", "don't give me parent id/name"),
	add_parse(BOOL16, without_parent_limits, "without_parent_limits", "don't give me limits from parents"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_user_cond_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(USER_CONDITION)[] = {
	add_parse(ADMIN_LVL, admin_level, "admin_level", "Administrator level"),
	add_parse(ASSOC_CONDITION_PTR, assoc_cond, "association", "Association filter"),
	add_parse(CSV_STRING_LIST, def_acct_list, "default_account", "CSV default account list"),
	add_parse(CSV_STRING_LIST, def_wckey_list, "default_wckey", "CSV default wckey list"),
	add_parse(BOOL16, with_assocs, "with_assocs", "With associations"),
	add_parse(BOOL16, with_coords, "with_coords", "With coordinators"),
	add_parse(BOOL16, with_deleted, "with_deleted", "With deleted"),
	add_parse(BOOL16, with_wckeys, "with_wckeys", "With wckeys"),
	add_parse(BOOL16, without_defaults, "without_defaults", "Exclude defaults"),
};
#undef add_parse

#define add_parse_req(mtype, field, path, desc) \
	add_parser(openapi_job_param_t, mtype, true, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_SLURMDBD_JOB_PARAM)[] = {
	add_parse_req(SELECTED_STEP_PTR, id, "job_id", "Job id"),
};
#undef add_parse_req

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_user_param_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_USER_PARAM)[] = {
	add_parse(STRING, name, "name", "User name"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_user_query_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_USER_QUERY)[] = {
	add_parse(BOOL, with_deleted, "with_deleted", "Include deleted users"),
	add_parse(BOOL, with_assocs, "with_assocs", "Include assocations"),
	add_parse(BOOL, with_coords, "with_coords", "Include coordinators"),
	add_parse(BOOL, with_wckeys, "with_wckeys", "Include wckeys"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_wckey_param_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_WCKEY_PARAM)[] = {
	add_parse(STRING, wckey, "id", "wckey id"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_wckey_cond_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(WCKEY_CONDITION)[] = {
	add_parse(CSV_STRING_LIST, cluster_list, "cluster", "CSV cluster name list"),
	add_parse(CSV_STRING_LIST, format_list, "format", "CSV format name list"),
	add_parse(CSV_STRING_LIST, id_list, "id", "CSV id list"),
	add_parse(CSV_STRING_LIST, name_list, "name", "CSV name list"),
	add_parse(BOOL16, only_defs, "only_defaults", "only query defaults"),
	add_parse(TIMESTAMP, usage_end, "usage_end", "usage end UNIX timestamp (seconds)"),
	add_parse(TIMESTAMP, usage_start, "usage_start", "usage start UNIX timestamp (seconds)"),
	add_parse(CSV_STRING_LIST, user_list, "user", "CSV user list"),
	add_parse(BOOL16, with_usage, "with_usage", "include usage with query"),
	add_parse(BOOL16, with_deleted, "with_deleted", "include deleted wckeys with query"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_account_param_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_ACCOUNT_PARAM)[] = {
	add_parse(STRING, name, "account_name", "Account name"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_account_query_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_ACCOUNT_QUERY)[] = {
	add_parse(BOOL, with_assocs, "with_assocs", "include associations"),
	add_parse(BOOL, with_coords, "with_coords", "include coordinators"),
	add_parse(BOOL, with_deleted, "with_deleted", "include deleted"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_account_cond_t , mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(ACCOUNT_CONDITION)[] = {
	add_parse(ASSOC_CONDITION_PTR, assoc_cond, "assocation", "assocation filter"),
	add_parse(STRING_LIST, description_list, "description", "CSV description list"),
	add_parse(BOOL16, with_assocs, "with_assocs", "include associations"),
	add_parse(BOOL16, with_coords, "with_coords", "include coordinators"),
	add_parse(BOOL16, with_deleted, "with_deleted", "include deleted accounts"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_cluster_param_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_CLUSTER_PARAM)[] = {
	add_parse(STRING, name, "cluster_name", "Cluster name"),
};
#undef add_parse

static const flag_bit_t PARSER_FLAG_ARRAY(CLUSTER_CLASSIFICATION)[] = {
	add_flag_equal(SLURMDB_CLASS_NONE, INFINITE16, "UNCLASSIFIED"),
	add_flag_bit(SLURMDB_CLASS_CAPABILITY, "CAPABILITY"),
	add_flag_bit(SLURMDB_CLASS_CAPACITY, "CAPACITY"),
	add_flag_bit(SLURMDB_CLASS_CAPAPACITY, "CAPAPACITY"),
};

#define add_parse(mtype, field, path, desc) \
	add_parser(slurmdb_cluster_cond_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(CLUSTER_CONDITION)[] = {
	add_parse_bit_flag_array(slurmdb_cluster_cond_t, CLUSTER_CLASSIFICATION, false, classification, "classification", NULL),
	add_parse(STRING_LIST, cluster_list, "cluster", "CSV cluster list"),
	add_parse(STRING_LIST, federation_list, "federation", "CSV federation list"),
	add_parse_bit_flag_array(slurmdb_cluster_cond_t, CLUSTER_REC_FLAGS, false, flags, "flags", NULL),
	add_parse(STRING_LIST, format_list, "format", "CSV format list"),
	add_parse(STRING_LIST, rpc_version_list, "rpc_version", "CSV RPC version list"),
	add_parse(TIMESTAMP, usage_end, "usage_end", "Usage end UNIX timestamp (seconds)"),
	add_parse(TIMESTAMP, usage_start, "usage_start", "Usage start UNIX timestamp (seconds)"),
	add_parse(BOOL16, with_deleted, "with_deleted", "include deleted clusters"),
	add_parse(BOOL16, with_usage, "with_usage", "query usage"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_job_info_param_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_JOB_INFO_PARAM)[] = {
	add_parse(SELECTED_STEP, job_id, "job_id", "JobId"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_job_info_delete_query_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_JOB_INFO_DELETE_QUERY)[] = {
	add_parse(SIGNAL, signal, "signal", "Signal to send to Job"),
	add_parse_bit_flag_array(openapi_job_info_delete_query_t, WARN_FLAGS, false, flags, "flags", "Signalling flags"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_job_info_query_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_JOB_INFO_QUERY)[] = {
	add_parse(TIMESTAMP, update_time, "update_time", "Filter jobs since update timestamp"),
	add_parse_bit_flag_array(openapi_job_info_query_t, JOB_SHOW_FLAGS, false, show_flags, "flags", "Query flags"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_node_param_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_NODE_PARAM)[] = {
	add_parse(STRING, node_name, "node_name", "Node name"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_partitions_query_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_PARTITIONS_QUERY)[] = {
	add_parse(TIMESTAMP, update_time, "update_time", "Filter partitions since update timestamp"),
	add_parse_bit_flag_array(openapi_partitions_query_t, JOB_SHOW_FLAGS, false, show_flags, "flags", "Query flags"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_partition_param_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_PARTITION_PARAM)[] = {
	add_parse(STRING, partition_name, "partition_name", "Partition name"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_nodes_query_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_NODES_QUERY)[] = {
	add_parse(TIMESTAMP, update_time, "update_time", "Filter jobs since update timestamp"),
	add_parse_bit_flag_array(openapi_nodes_query_t, JOB_SHOW_FLAGS, false, show_flags, "flags", "Query flags"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_reservation_param_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_RESERVATION_PARAM)[] = {
	add_parse(STRING, reservation_name, "reservation_name", "Reservation name"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_reservation_query_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_RESERVATION_QUERY)[] = {
	add_parse(TIMESTAMP, update_time, "update_time", "Filter reservations since update timestamp"),
};
#undef add_parse

static const flag_bit_t PARSER_FLAG_ARRAY(FLAGS)[] = {
	add_flag_equal(FLAG_NONE , INFINITE, "NONE"),
	add_flag_bit(FLAG_SPEC_ONLY, "SPEC_ONLY"),
	add_flag_bit(FLAG_FAST, "FAST"),
	add_flag_bit(FLAG_COMPLEX_VALUES, "COMPLEX"),
};

#define add_flag(flag_value, mask, flag_string, hidden, desc)               \
	add_flag_bit_entry(FLAG_BIT_TYPE_BIT, XSTRINGIFY(flag_value),       \
			   flag_value, mask, XSTRINGIFY(mask), flag_string, \
			   hidden, desc)
#define add_flag_eq(flag_value, mask, flag_string, hidden, desc)            \
	add_flag_bit_entry(FLAG_BIT_TYPE_EQUAL, XSTRINGIFY(flag_value),     \
			   flag_value, mask, XSTRINGIFY(mask), flag_string, \
			   hidden, desc)
static const flag_bit_t PARSER_FLAG_ARRAY(JOB_STATE)[] = {
	add_flag_eq(JOB_PENDING, JOB_STATE_BASE, "PENDING", false, "queued waiting for initiation"),
	add_flag_eq(JOB_RUNNING, JOB_STATE_BASE, "RUNNING", false, "allocated resources and executing"),
	add_flag_eq(JOB_SUSPENDED, JOB_STATE_BASE, "SUSPENDED", false, "allocated resources, execution suspended"),
	add_flag_eq(JOB_COMPLETE, JOB_STATE_BASE, "COMPLETED", false, "completed execution successfully"),
	add_flag_eq(JOB_CANCELLED, JOB_STATE_BASE, "CANCELLED", false, "cancelled by user"),
	add_flag_eq(JOB_FAILED, JOB_STATE_BASE, "FAILED", false, "completed execution unsuccessfully"),
	add_flag_eq(JOB_TIMEOUT, JOB_STATE_BASE, "TIMEOUT", false, "terminated on reaching time limit"),
	add_flag_eq(JOB_NODE_FAIL, JOB_STATE_BASE, "NODE_FAIL", false, "terminated on node failure"),
	add_flag_eq(JOB_PREEMPTED, JOB_STATE_BASE, "PREEMPTED", false, "terminated due to preemption"),
	add_flag_eq(JOB_BOOT_FAIL, JOB_STATE_BASE, "BOOT_FAIL", false, "terminated due to node boot failure"),
	add_flag_eq(JOB_DEADLINE, JOB_STATE_BASE, "DEADLINE", false, "terminated on deadline"),
	add_flag_eq(JOB_OOM, JOB_STATE_BASE, "OUT_OF_MEMORY", false, "experienced out of memory error"),
	add_flag_eq(JOB_END, JOB_STATE_BASE, "invalid-placeholder", true, NULL),
	add_flag(JOB_LAUNCH_FAILED, JOB_STATE_FLAGS, "LAUNCH_FAILED", false, "job launch failed"),
	add_flag(JOB_UPDATE_DB, JOB_STATE_FLAGS, "UPDATE_DB", false, "Send job start to database again"),
	add_flag(JOB_REQUEUE, JOB_STATE_FLAGS, "REQUEUED", false, "Requeue job in completing state"),
	add_flag(JOB_REQUEUE_HOLD, JOB_STATE_FLAGS, "REQUEUE_HOLD", false, "Requeue any job in hold"),
	add_flag(JOB_SPECIAL_EXIT, JOB_STATE_FLAGS, "SPECIAL_EXIT", false, "Requeue an exit job in hold"),
	add_flag(JOB_RESIZING, JOB_STATE_FLAGS, "RESIZING", false, "Size of job about to change, flag set before calling accounting functions immediately before job changes size"),
	add_flag(JOB_CONFIGURING, JOB_STATE_FLAGS, "CONFIGURING", false, "Allocated nodes booting"),
	add_flag(JOB_COMPLETING, JOB_STATE_FLAGS, "COMPLETING", false, "Waiting for epilog completion"),
	add_flag(JOB_STOPPED, JOB_STATE_FLAGS, "STOPPED", false, "Job is stopped state (holding resources, but sent SIGSTOP)"),
	add_flag(JOB_RECONFIG_FAIL, JOB_STATE_FLAGS, "RECONFIG_FAIL", false, "Node configuration for job failed, not job state, just job requeue flag"),
	add_flag(JOB_POWER_UP_NODE, JOB_STATE_FLAGS, "POWER_UP_NODE", false, "Allocated powered down nodes, waiting for reboot"),
	add_flag(JOB_REVOKED, JOB_STATE_FLAGS, "REVOKED", false, "Sibling job revoked"),
	add_flag(JOB_REQUEUE_FED, JOB_STATE_FLAGS, "REQUEUE_FED", false, "Job being requeued by federation"),
	add_flag(JOB_RESV_DEL_HOLD, JOB_STATE_FLAGS, "RESV_DEL_HOLD", false, "Job is being held"),
	add_flag(JOB_SIGNALING, JOB_STATE_FLAGS, "SIGNALING", false, "Outgoing signal is pending"),
	add_flag(JOB_STAGE_OUT, JOB_STATE_FLAGS, "STAGE_OUT", false, "Staging out data (burst buffer)"),
};
#undef add_flag
#undef add_flag_eq

#define add_flag_eq(flag_value, flag_string, hidden, desc)            \
	add_flag_bit_entry(FLAG_BIT_TYPE_EQUAL,                       \
			   XSTRINGIFY(flag_value), flag_value,        \
			   INFINITE, XSTRINGIFY(INFINITE),            \
			   flag_string, hidden, desc)
static const flag_bit_t PARSER_FLAG_ARRAY(PROCESS_EXIT_CODE_STATUS)[] = {
	add_flag_eq(PROC_EXIT_CODE_INVALID, "INVALID", false, "Process return code invalid"),
	add_flag_eq(PROC_EXIT_CODE_PENDING, "PENDING", false, "Process has not started or completed yet"),
	add_flag_eq(PROC_EXIT_CODE_SUCCESS, "SUCCESS", false, "Process exited with return code 0 to signify success"),
	add_flag_eq(PROC_EXIT_CODE_ERROR, "ERROR", false, "Process exited with nonzero return code"),
	add_flag_eq(PROC_EXIT_CODE_SIGNALED, "SIGNALED", false, "Process terminated due to signal"),
	add_flag_eq(PROC_EXIT_CODE_CORE_DUMPED, "CORE_DUMPED", false, "Process terminated due to signal"),
	add_flag_eq(PROC_EXIT_CODE_INVALID_MAX, "INVALID2", true, NULL),
};
#undef add_flag_eq

#define add_parse(mtype, field, path, desc) \
	add_parser(proc_exit_code_verbose_t, mtype, false, field, 0, path, desc)
#define add_parse_overload(mtype, field, overloads, path, desc) \
	add_parser(proc_exit_code_verbose_t, mtype, false, field, overloads, path, desc)
#define add_flag(mtype, field, path, desc) \
	add_parse_bit_flag_string(proc_exit_code_verbose_t, mtype, false, field, path, desc)
static const parser_t PARSER_ARRAY(PROCESS_EXIT_CODE_VERBOSE)[] = {
	add_flag(PROCESS_EXIT_CODE_STATUS, status, "status", "Status given by return code"),
	add_parse(UINT32_NO_VAL, return_code, "return_code", "Process return code (numeric)"),
	add_parse_overload(UINT16_NO_VAL, signal, 1, "signal/id", "Signal sent to process (numeric)"),
	add_parse_overload(SIGNAL, signal, 1, "signal/name", "Signal sent to process"),
};
#undef add_parse
#undef add_parse_overload
#undef add_flag

#define add_parse(mtype, field, path, desc) \
	add_parser(slurm_step_id_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(SLURM_STEP_ID)[] = {
	add_parse(UINT32_NO_VAL, job_id, "job_id", "Job identifier"),
	add_parse(UINT32_NO_VAL, step_het_comp, "step_het_component", "HetJob Component"),
	add_parse(STEP_ID, step_id, "step_id", "Job StepId"),
};
#undef add_parse

#define add_flag_eq(flag_value, mask, flag_string, hidden, desc)            \
	add_flag_bit_entry(FLAG_BIT_TYPE_EQUAL, XSTRINGIFY(flag_value),     \
			   flag_value, mask, XSTRINGIFY(mask), flag_string, \
			   hidden, desc)
static const flag_bit_t PARSER_FLAG_ARRAY(STEP_NAMES)[] = {
	add_flag_eq(SLURM_PENDING_STEP, INFINITE, "TBD", false, "StepId not yet assigned"),
	add_flag_eq(SLURM_EXTERN_CONT, INFINITE, "extern", false, "External Step"),
	add_flag_eq(SLURM_BATCH_SCRIPT, INFINITE, "batch", false, "Batch Step"),
	add_flag_eq(SLURM_INTERACTIVE_STEP, INFINITE, "interactive", false, "Interactive Step"),
};
#undef add_flag_eq

#define add_cparse(mtype, path, desc) \
	add_complex_parser(shares_response_msg_t, mtype, false, path, desc)
#define add_parse(mtype, field, path, desc) \
	add_parser(shares_response_msg_t, mtype, false, field, 0, path, desc)
#define add_skip(field) add_parser_skip(shares_response_msg_t, field)
static const parser_t PARSER_ARRAY(SHARES_RESP_MSG)[] = {
	add_cparse(ASSOC_SHARES_OBJ_LIST, "shares", "Assocation shares"),
	add_parse(UINT64, tot_shares, "total_shares", "Total number of shares"),
	add_skip(tres_cnt),
	add_skip(tres_names),
};
#undef add_parse
#undef add_cparse
#undef add_skip

static const flag_bit_t PARSER_FLAG_ARRAY(ASSOC_SHARES_OBJ_WRAP_TYPE)[] = {
	add_flag_equal(1, INFINITE16, "USER"),
	add_flag_equal(0, INFINITE16, "ASSOCIATION"),
};

#define add_cparse(mtype, path, desc) \
	add_complex_parser(assoc_shares_object_wrap_t, mtype, false, path, desc)
#define add_parse(mtype, field, path, desc) \
	add_parser(assoc_shares_object_wrap_t, mtype, false, field, 0, path, desc)
#define add_skip(field) add_parser_skip(assoc_shares_object_wrap_t, field)
static const parser_t PARSER_ARRAY(ASSOC_SHARES_OBJ_WRAP)[] = {
	add_parse(UINT32, obj.assoc_id, "id", "assocation id"),
	add_parse(STRING, obj.cluster, "cluster", "cluster name"),
	add_parse(STRING, obj.name, "name", "share name"),
	add_parse(STRING, obj.parent, "parent", "parent name"),
	add_parse(STRING, obj.partition, "partition", "partition name"),
	add_parse(FLOAT64_NO_VAL, obj.shares_norm, "shares_normalized", "normalized shares"),
	add_parse(UINT32_NO_VAL, obj.shares_raw, "shares", "number of shares allocated"),
	add_cparse(ASSOC_SHARES_OBJ_WRAP_TRES_RUN_SECS, "tres/run_seconds", "currently running tres-secs = grp_used_tres_run_secs"),
	add_cparse(ASSOC_SHARES_OBJ_WRAP_TRES_GRP_MINS, "tres/group_minutes", "tres-minute limit"),
	add_parse(FLOAT64, obj.usage_efctv, "effective_usage", "effective, normalized usage"),
	add_parse(FLOAT64_NO_VAL, obj.usage_norm, "usage_normalized", "normalized usage"),
	add_parse(UINT64, obj.usage_raw, "usage", "measure of tresbillableunits usage"),
	add_cparse(ASSOC_SHARES_OBJ_WRAP_TRES_USAGE_RAW, "tres/usage", "measure of each tres usage"),
	add_parse(FLOAT64, obj.fs_factor, "fairshare/factor", "fairshare factor"),
	add_parse(FLOAT64, obj.level_fs, "fairshare/level", "fairshare factor at this level. stored on an assoc as a long double, but that is not needed for display in sshare"),
	add_parse_bit_flag_array(assoc_shares_object_wrap_t, ASSOC_SHARES_OBJ_WRAP_TYPE, false, obj.user, "type", "user or account association"),
	add_skip(tot_shares),
	add_skip(tres_cnt),
	add_skip(tres_names),
};
#undef add_parse
#undef add_cparse
#undef add_skip

#define add_parse(mtype, field, path, desc) \
	add_parser(SHARES_UINT64_TRES_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(SHARES_UINT64_TRES)[] = {
	add_parse(STRING, name, "name", "TRES name"),
	add_parse(UINT64_NO_VAL, value, "value", "TRES value"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(SHARES_FLOAT128_TRES_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(SHARES_FLOAT128_TRES)[] = {
	add_parse(STRING, name, "name", "TRES name"),
	add_parse(FLOAT128, value, "value", "TRES value"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(shares_request_msg_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(SHARES_REQ_MSG)[] = {
	add_parse(CSV_STRING_LIST, acct_list, "accounts", "Accounts to query"),
	add_parse(CSV_STRING_LIST, user_list, "users", "Users to query"),
};
#undef add_parse

#define add_parse_req(mtype, field, path, desc) \
	add_parser(openapi_qos_param_t, mtype, true, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_SLURMDBD_QOS_PARAM)[] = {
	add_parse_req(QOS_NAME, name, "qos", "QOS name"),
};
#undef add_parse_req

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_qos_query_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_SLURMDBD_QOS_QUERY)[] = {
	add_parse(BOOL, with_deleted, "with_deleted", "Query includes deleted QOS"),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(JOB_ARRAY_RESPONSE_MSG_entry_t, mtype, false, field, 0, path, desc)
#define add_parse_overload(mtype, field, overloads, path, desc) \
	add_parser(JOB_ARRAY_RESPONSE_MSG_entry_t, mtype, false, field, overloads, path, desc)
static const parser_t PARSER_ARRAY(JOB_ARRAY_RESPONSE_MSG_ENTRY)[] = {
	add_parse(UINT32, step.step_id.job_id, "job_id", "JobId for updated Job"),
	add_parse(SELECTED_STEP, step, "step_id", "StepId for updated Job"),
	add_parse_overload(ERROR, rc, 1, "error", "Verbose update status or error"),
	add_parse_overload(INT32, rc, 1, "error_code", "Verbose update status or error"),
	add_parse(STRING, msg, "why", "Update response message"),
};
#undef add_parse
#undef add_parse_overload

static const flag_bit_t PARSER_FLAG_ARRAY(WCKEY_TAG_FLAGS)[] = {
	add_flag_bit(WCKEY_TAG_FLAGS_ASSIGNED_DEFAULT, "ASSIGNED_DEFAULT"),
};

#define add_parse_req(mtype, field, path, desc) \
	add_parser(WCKEY_TAG_STRUCT_t, mtype, true, field, 0, path, desc)
static const parser_t PARSER_ARRAY(WCKEY_TAG_STRUCT)[] = {
	add_parse_req(STRING, wckey, "wckey", "WCKey name"),
	add_parse_req(WCKEY_TAG_FLAGS, flags, "flags", "Active flags"),
};
#undef add_parse_req

static const flag_bit_t PARSER_FLAG_ARRAY(NEED_PREREQS_FLAGS)[] = {
	add_flag_equal(NEED_NONE, INFINITE16, "NONE"),
	add_flag_bit(NEED_AUTH, "AUTH"),
	add_flag_bit(NEED_TRES, "TRES"),
	add_flag_bit(NEED_QOS, "QOS"),
	add_flag_bit(NEED_ASSOC, "ASSOC"),
};

static const flag_bit_t PARSER_FLAG_ARRAY(CR_TYPE)[] = {
	add_flag_equal(CR_CPU, (CR_CPU|CR_SOCKET|CR_CORE), "CPU"),
	add_flag_equal(CR_SOCKET, (CR_CPU|CR_SOCKET|CR_CORE), "SOCKET"),
	add_flag_equal(CR_CORE, (CR_CPU|CR_SOCKET|CR_CORE), "CORE"),
	add_flag_bit(CR_BOARD, "BOARD"),
	add_flag_bit(CR_MEMORY, "MEMORY"),
	add_flag_bit(CR_ONE_TASK_PER_CORE, "ONE_TASK_PER_CORE"),
	add_flag_bit(CR_PACK_NODES, "PACK_NODES"),
	add_flag_bit(CR_CORE_DEFAULT_DIST_BLOCK, "CORE_DEFAULT_DIST_BLOCK"),
	add_flag_bit(CR_LLN, "LLN"),
	add_flag_bit(CR_LINEAR, "LINEAR"),
};

/* Descriptions from _node_state_str() */
static const flag_bit_t PARSER_FLAG_ARRAY(NODE_CR_TYPE)[] = {
	add_flag_equal(NODE_CR_AVAILABLE, INFINITE, "AVAILABLE"),
	add_flag_equal(NODE_CR_ONE_ROW, INFINITE, "ONE_ROW"),
	add_flag_equal(NODE_CR_RESERVED , INFINITE, "RESERVED"),
};

#define add_parse(mtype, field, path, desc) \
	add_parser(FLOAT64_NO_VAL_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(FLOAT64_NO_VAL_STRUCT)[] = {
	add_parse(BOOL, set, "set", "True if number has been set. False if number is unset"),
	add_parse(BOOL, infinite, "infinite", "True if number has been set to infinite. \"set\" and \"number\" will be ignored."),
	add_parse(FLOAT64, number, "number", "If set is True the number will be set with value. Otherwise ignore number contents."),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(UINT64_NO_VAL_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(UINT64_NO_VAL_STRUCT)[] = {
	add_parse(BOOL, set, "set", "True if number has been set. False if number is unset"),
	add_parse(BOOL, infinite, "infinite", "True if number has been set to infinite. \"set\" and \"number\" will be ignored."),
	add_parse(UINT64, number, "number", "If set is True the number will be set with value. Otherwise ignore number contents."),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(UINT32_NO_VAL_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(UINT32_NO_VAL_STRUCT)[] = {
	add_parse(BOOL, set, "set", "True if number has been set. False if number is unset"),
	add_parse(BOOL, infinite, "infinite", "True if number has been set to infinite. \"set\" and \"number\" will be ignored."),
	add_parse(UINT32, number, "number", "If set is True the number will be set with value. Otherwise ignore number contents."),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(UINT16_NO_VAL_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(UINT16_NO_VAL_STRUCT)[] = {
	add_parse(BOOL, set, "set", "True if number has been set. False if number is unset"),
	add_parse(BOOL, infinite, "infinite", "True if number has been set to infinite. \"set\" and \"number\" will be ignored."),
	add_parse(UINT16, number, "number", "If set is True the number will be set with value. Otherwise ignore number contents."),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(INT64_NO_VAL_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(INT64_NO_VAL_STRUCT)[] = {
	add_parse(BOOL, set, "set", "True if number has been set. False if number is unset"),
	add_parse(BOOL, infinite, "infinite", "True if number has been set to infinite. \"set\" and \"number\" will be ignored."),
	add_parse(INT64, number, "number", "If set is True the number will be set with value. Otherwise ignore number contents."),
};
#undef add_parse

#define add_skip(field) \
	add_parser_skip(slurmdb_rollup_stats_t, field)
#define add_parse_req(mtype, field, path, desc) \
	add_parser(slurmdb_rollup_stats_t, mtype, true, field, 0, path, desc)
static const parser_t PARSER_ARRAY(ROLLUP_STATS)[] = {
	add_skip(cluster_name), /* not packed */
	add_parse_req(UINT16, count[DBD_ROLLUP_HOUR], "hourly/count", "number of hourly rollups since last_run"),
	add_parse_req(UINT16, count[DBD_ROLLUP_DAY], "daily/count", "number of daily rollups since last_run"),
	add_parse_req(UINT16, count[DBD_ROLLUP_MONTH], "monthly/count", "number of monthly rollups since last_run"),
	add_parse_req(TIMESTAMP, timestamp[DBD_ROLLUP_HOUR], "hourly/last_run", "Last time hourly rollup ran (UNIX timestamp)"),
	add_parse_req(TIMESTAMP, timestamp[DBD_ROLLUP_DAY], "daily/last_run", "Last time daily rollup ran (UNIX timestamp)"),
	add_parse_req(TIMESTAMP, timestamp[DBD_ROLLUP_MONTH], "monthly/last_run", "Last time monthly rollup ran (UNIX timestamp)"),
	add_parse_req(UINT64, time_last[DBD_ROLLUP_HOUR], "hourly/duration/last", "total time spent doing last daily rollup (seconds)"),
	add_parse_req(UINT64, time_last[DBD_ROLLUP_DAY], "daily/duration/last", "total time spent doing daily daily rollup (seconds)"),
	add_parse_req(UINT64, time_last[DBD_ROLLUP_MONTH], "monthly/duration/last", "total time spent doing monthly daily rollup (seconds)"),
	add_parse_req(UINT64, time_max[DBD_ROLLUP_HOUR], "hourly/duration/max", "longest hourly rollup time (seconds)"),
	add_parse_req(UINT64, time_max[DBD_ROLLUP_DAY], "daily/duration/max", "longest daily rollup time (seconds)"),
	add_parse_req(UINT64, time_max[DBD_ROLLUP_MONTH], "monthly/duration/max", "longest monthly rollup time (seconds)"),
	add_parse_req(UINT64, time_total[DBD_ROLLUP_HOUR], "hourly/duration/time", "total time spent doing hourly rollups (seconds)"),
	add_parse_req(UINT64, time_total[DBD_ROLLUP_DAY], "daily/duration/time", "total time spent doing daily rollups (seconds)"),
	add_parse_req(UINT64, time_total[DBD_ROLLUP_MONTH], "monthly/duration/time", "total time spent doing monthly rollups (seconds)"),
};
#undef add_parse_req
#undef add_skip

#define add_parse_req(mtype, field, path, desc) \
	add_parser(STATS_MSG_RPC_TYPE_t, mtype, true, field, 0, path, desc)
#define add_parse_req_overload(mtype, field, overloads, path, desc) \
	add_parser(STATS_MSG_RPC_TYPE_t, mtype, true, field, overloads, path, desc)
static const parser_t PARSER_ARRAY(STATS_MSG_RPC_TYPE)[] = {
	add_parse_req_overload(UINT16, id, 1, "type_id", "Message type as integer"),
	add_parse_req_overload(RPC_ID, id, 1, "message_type", "Message type as string"),
	add_parse_req(UINT32, count, "count", "Number of RPCs received"),
	add_parse_req(UINT64, time, "total_time", "Total time spent processing RPC in seconds"),
	add_parse_req(UINT64_NO_VAL, average_time, "average_time", "Average time spent processing RPC in seconds"),
};
#undef add_parse_req
#undef add_parse_req_overload

#define add_parse_req(mtype, field, path, desc) \
	add_parser(STATS_MSG_RPC_USER_t, mtype, true, field, 0, path, desc)
#define add_parse_req_overload(mtype, field, overloads, path, desc) \
	add_parser(STATS_MSG_RPC_USER_t, mtype, true, field, overloads, path, desc)
static const parser_t PARSER_ARRAY(STATS_MSG_RPC_USER)[] = {
	add_parse_req_overload(UINT32, id, 1, "user_id", "user id (numeric)"),
	add_parse_req_overload(USER_ID, id, 1, "user", "user name"),
	add_parse_req(UINT32, count, "count", "Number of RPCs received"),
	add_parse_req(UINT64, time, "total_time", "Total time spent processing RPC in seconds"),
	add_parse_req(UINT64_NO_VAL, average_time, "average_time", "Average time spent processing RPC in seconds"),
};
#undef add_parse_req
#undef add_parse_req_overload

#define add_parse_req(mtype, field, path, desc) \
	add_parser(STATS_MSG_RPC_QUEUE_t, mtype, true, field, 0, path, desc)
#define add_parse_req_overload(mtype, field, overloads, path, desc) \
	add_parser(STATS_MSG_RPC_QUEUE_t, mtype, true, field, overloads, path, desc)
static const parser_t PARSER_ARRAY(STATS_MSG_RPC_QUEUE)[] = {
	add_parse_req_overload(UINT16, id, 1, "type_id", "Message type as integer"),
	add_parse_req_overload(RPC_ID, id, 1, "message_type", "Message type as string"),
	add_parse_req(UINT32, count, "count", "Number of pending RPCs queued"),
};
#undef add_parse_req
#undef add_parse_req_overload

#define add_parse_req(mtype, field, path, desc) \
	add_parser(STATS_MSG_RPC_DUMP_t, mtype, true, field, 0, path, desc)
#define add_parse_req_overload(mtype, field, overloads, path, desc) \
	add_parser(STATS_MSG_RPC_DUMP_t, mtype, true, field, overloads, path, desc)
static const parser_t PARSER_ARRAY(STATS_MSG_RPC_DUMP)[] = {
	add_parse_req_overload(UINT16, id, 1, "type_id", "Message type as integer"),
	add_parse_req_overload(RPC_ID, id, 1, "message_type", "Message type as string"),
	add_parse_req(HOSTLIST_STRING, hostlist, "count", "Number of RPCs received"),
};
#undef add_parse_req
#undef add_parse_req_overload

#define add_parse_req(mtype, field, path, desc) \
	add_parser(job_state_response_job_t, mtype, true, field, 0, path, desc)
#define add_cparse_req(mtype, path, desc) \
	add_complex_parser(job_state_response_job_t, mtype, true, path, desc)
#define add_skip(field) \
	add_parser_skip(job_state_response_job_t, field)
static const parser_t PARSER_ARRAY(JOB_STATE_RESP_JOB)[] = {
	add_cparse_req(JOB_STATE_RESP_JOB_JOB_ID, "job_id", "JobId"),
	add_skip(job_id),
	add_skip(array_task_id),
	add_skip(array_task_id_bitmap),
	add_parse_req(JOB_STATE, state, "state", "Job state"),
};
#undef add_parse_req
#undef add_cparse_req
#undef add_skip

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_job_state_query_t , mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_JOB_STATE_QUERY)[] = {
	add_parse(SELECTED_STEP_LIST, job_id_list, "job_id", "Search for CSV list of JobIds"),
};
#undef add_parse

#define add_openapi_response_meta(rtype) \
	add_parser(rtype, OPENAPI_META_PTR, false, meta, 0, XSTRINGIFY(OPENAPI_RESP_STRUCT_META_FIELD_NAME), "Slurm meta values")
#define add_openapi_response_errors(rtype) \
	add_parser(rtype, OPENAPI_ERRORS, false, errors, 0, XSTRINGIFY(OPENAPI_RESP_STRUCT_ERRORS_FIELD_NAME), "Query errors")
#define add_openapi_response_warnings(rtype) \
	add_parser(rtype, OPENAPI_WARNINGS, false, warnings, 0, XSTRINGIFY(OPENAPI_RESP_STRUCT_WARNINGS_FIELD_NAME), "Query warnings")

/*
 * Generic response when there is only possiblity of warnings/errors
 * and HTTP status code.
 */
static const parser_t PARSER_ARRAY(OPENAPI_RESP)[] = {                            \
	add_openapi_response_meta(openapi_resp_single_t),                         \
	add_openapi_response_errors(openapi_resp_single_t),                       \
	add_openapi_response_warnings(openapi_resp_single_t),                     \
};

/* add parser array for an OpenAPI response with a single field */
#define add_openapi_response_single(stype, mtype, path, desc)                             \
	static const parser_t PARSER_ARRAY(stype)[] = {                                   \
		add_parser(openapi_resp_single_t, mtype, true, response, 0, path, desc),  \
		add_openapi_response_meta(openapi_resp_single_t),                         \
		add_openapi_response_errors(openapi_resp_single_t),                       \
		add_openapi_response_warnings(openapi_resp_single_t),                     \
	}

add_openapi_response_single(OPENAPI_DIAG_RESP, STATS_MSG_PTR, "statistics", "statistics");
add_openapi_response_single(OPENAPI_PING_ARRAY_RESP, CONTROLLER_PING_ARRAY, "pings", "pings");
add_openapi_response_single(OPENAPI_ACCOUNTS_RESP, ACCOUNT_LIST, "accounts", "accounts");
add_openapi_response_single(OPENAPI_ACCOUNTS_REMOVED_RESP, STRING_LIST, "removed_accounts", "removed_accounts");
add_openapi_response_single(OPENAPI_ACCOUNTS_ADD_COND_RESP_STR, STRING, "added_accounts", "added_accounts");
add_openapi_response_single(OPENAPI_ASSOCS_RESP, ASSOC_LIST, "associations", "associations");
add_openapi_response_single(OPENAPI_ASSOCS_REMOVED_RESP, STRING_LIST, "removed_associations", "removed_associations");
add_openapi_response_single(OPENAPI_CLUSTERS_RESP, CLUSTER_REC_LIST, "clusters", "clusters");
add_openapi_response_single(OPENAPI_CLUSTERS_REMOVED_RESP, STRING_LIST, "deleted_clusters", "deleted_clusters");
add_openapi_response_single(OPENAPI_INSTANCES_RESP, INSTANCE_LIST, "instances", "instances");
add_openapi_response_single(OPENAPI_SLURMDBD_STATS_RESP, STATS_REC_PTR, "statistics", "statistics");
add_openapi_response_single(OPENAPI_SLURMDBD_JOBS_RESP, JOB_LIST, "jobs", "jobs");
add_openapi_response_single(OPENAPI_SLURMDBD_QOS_RESP, QOS_LIST, "qos", "List of QOS");
add_openapi_response_single(OPENAPI_SLURMDBD_QOS_REMOVED_RESP, STRING_LIST, "removed_qos", "removed QOS");
add_openapi_response_single(OPENAPI_TRES_RESP, TRES_LIST, "TRES", "TRES");
add_openapi_response_single(OPENAPI_USERS_ADD_COND_RESP_STR, STRING, "added_users", "added_users");
add_openapi_response_single(OPENAPI_USERS_RESP, USER_LIST, "users", "users");
add_openapi_response_single(OPENAPI_USERS_REMOVED_RESP, STRING_LIST, "removed_users", "removed_users");
add_openapi_response_single(OPENAPI_WCKEY_RESP, WCKEY_LIST, "wckeys", "wckeys");
add_openapi_response_single(OPENAPI_WCKEY_REMOVED_RESP, STRING_LIST, "deleted_wckeys", "deleted wckeys");
add_openapi_response_single(OPENAPI_SHARES_RESP, SHARES_RESP_MSG_PTR, "shares", "fairshare info");
add_openapi_response_single(OPENAPI_SINFO_RESP, SINFO_DATA_LIST, "sinfo", "node and partition info");

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_job_post_response_t, mtype, false, field, 0, path, desc)
#define add_parse_deprec(mtype, field, overloads, path, desc, deprec) \
	add_parser_deprec(openapi_job_post_response_t, mtype, false, field, overloads, path, desc, deprec)
static const parser_t PARSER_ARRAY(OPENAPI_JOB_POST_RESPONSE)[] = {
	add_parse(JOB_ARRAY_RESPONSE_MSG_PTR, results, "results", "Job update results"),
	add_parse_deprec(STRING, job_id, 0, "job_id", "First updated JobId - Use results instead", SLURM_23_11_PROTOCOL_VERSION),
	add_parse_deprec(STRING, step_id, 0, "step_id", "First updated StepID - Use results instead", SLURM_23_11_PROTOCOL_VERSION),
	add_parse_deprec(STRING, job_submit_user_msg, 0, "job_submit_user_msg", "First updated Job submision user message - Use results instead", SLURM_23_11_PROTOCOL_VERSION),
	add_openapi_response_meta(openapi_job_post_response_t),
	add_openapi_response_errors(openapi_job_post_response_t),
	add_openapi_response_warnings(openapi_job_post_response_t),
};
#undef add_parse
#undef add_parse_deprec

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_job_submit_response_t, mtype, false, field, 0, path, desc)
#define add_parse_deprec(mtype, field, overloads, path, desc, deprec) \
	add_parser_deprec(openapi_job_submit_response_t, mtype, false, field, overloads, path, desc, deprec)
static const parser_t PARSER_ARRAY(OPENAPI_JOB_SUBMIT_RESPONSE)[] = {
	add_parse_deprec(JOB_SUBMIT_RESPONSE_MSG, resp, 0, "result", "Job submission", SLURM_23_11_PROTOCOL_VERSION),
	add_parse(UINT32, resp.job_id, "job_id", "submited JobId"),
	add_parse(STEP_ID, resp.step_id, "step_id", "submited StepID"),
	add_parse(STRING, resp.job_submit_user_msg, "job_submit_user_msg", "job submision user message"),
	add_openapi_response_meta(openapi_job_submit_response_t),
	add_openapi_response_errors(openapi_job_submit_response_t),
	add_openapi_response_warnings(openapi_job_submit_response_t),
};
#undef add_parse
#undef add_parse_deprec

#define add_parse_req(mtype, field, path, desc) \
	add_parser(openapi_resp_job_info_msg_t, mtype, true, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_JOB_INFO_RESP)[] = {
	add_parse_req(JOB_INFO_MSG_PTR, jobs, "jobs", "list of jobs"),
	add_parse_req(TIMESTAMP_NO_VAL, last_backfill, "last_backfill", "time of last backfill scheduler run (UNIX timestamp)"),
	add_parse_req(TIMESTAMP_NO_VAL, last_update, "last_update", "time of last job change (UNIX timestamp)"),
	add_openapi_response_meta(openapi_resp_slurmdbd_config_t),
	add_openapi_response_errors(openapi_resp_slurmdbd_config_t),
	add_openapi_response_warnings(openapi_resp_slurmdbd_config_t),
};
#undef add_parse_req

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_resp_slurmdbd_config_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_SLURMDBD_CONFIG_RESP)[] = {
	add_parse(CLUSTER_REC_LIST, clusters, "clusters", "clusters"),
	add_parse(TRES_LIST, tres, "tres", "tres"),
	add_parse(ACCOUNT_LIST, accounts, "accounts", "accounts"),
	add_parse(USER_LIST, users, "users", "users"),
	add_parse(QOS_LIST, qos, "qos", "qos"),
	add_parse(WCKEY_LIST, wckeys, "wckeys", "wckeys"),
	add_parse(ASSOC_LIST, associations, "associations", "associations"),
	add_parse(INSTANCE_LIST, instances, "instances", "instances"),
	add_openapi_response_meta(openapi_resp_slurmdbd_config_t),
	add_openapi_response_errors(openapi_resp_slurmdbd_config_t),
	add_openapi_response_warnings(openapi_resp_slurmdbd_config_t),
};
#undef add_parse

#define add_parse_req(mtype, field, path, desc) \
	add_parser(openapi_resp_node_info_msg_t, mtype, true, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_NODES_RESP)[] = {
	add_parse_req(NODES_PTR, nodes, "nodes", "list of nodes"),
	add_parse_req(TIMESTAMP_NO_VAL, last_update, "last_update", "time of last node change (UNIX timestamp)"),
	add_openapi_response_meta(openapi_resp_slurmdbd_config_t),
	add_openapi_response_errors(openapi_resp_slurmdbd_config_t),
	add_openapi_response_warnings(openapi_resp_slurmdbd_config_t),
};
#undef add_parse_req

#define add_parse_req(mtype, field, path, desc) \
	add_parser(openapi_resp_partitions_info_msg_t, mtype, true, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_PARTITION_RESP)[] = {
	add_parse_req(PARTITION_INFO_MSG_PTR, partitions, "partitions", "list of partitions"),
	add_parse_req(TIMESTAMP_NO_VAL, last_update, "last_update", "time of last partition change (UNIX timestamp)"),
	add_openapi_response_meta(openapi_resp_slurmdbd_config_t),
	add_openapi_response_errors(openapi_resp_slurmdbd_config_t),
	add_openapi_response_warnings(openapi_resp_slurmdbd_config_t),
};
#undef add_parse_req

#define add_parse_req(mtype, field, path, desc) \
	add_parser(openapi_resp_reserve_info_msg_t, mtype, true, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_RESERVATION_RESP)[] = {
	add_parse_req(RESERVATION_INFO_MSG_PTR, reservations, "reservations", "list of reservations"),
	add_parse_req(TIMESTAMP_NO_VAL, last_update, "last_update", "time of last reservation change (UNIX timestamp)"),
	add_openapi_response_meta(openapi_resp_slurmdbd_config_t),
	add_openapi_response_errors(openapi_resp_slurmdbd_config_t),
	add_openapi_response_warnings(openapi_resp_slurmdbd_config_t),
};
#undef add_parse_req

#define add_parse_req(mtype, field, path, desc) \
	add_parser(openapi_resp_license_info_msg_t, mtype, true, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_LICENSES_RESP)[] = {
	add_parse_req(LICENSES_PTR, licenses, "licenses", "list of licenses"),
	add_parse_req(TIMESTAMP_NO_VAL, last_update, "last_update", "time of last licenses change (UNIX timestamp)"),
	add_openapi_response_meta(openapi_resp_slurmdbd_config_t),
	add_openapi_response_errors(openapi_resp_slurmdbd_config_t),
	add_openapi_response_warnings(openapi_resp_slurmdbd_config_t),
};
#undef add_parse_req

#define add_parse_req(mtype, field, path, desc) \
	add_parser(openapi_resp_job_step_info_msg_t, mtype, true, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_STEP_INFO_MSG)[] = {
	add_parse_req(STEP_INFO_MSG_PTR, steps, "steps", "list of steps"),
	add_parse_req(TIMESTAMP_NO_VAL, last_update, "last_update", "time of last licenses change (UNIX timestamp)"),
	add_openapi_response_meta(openapi_resp_slurmdbd_config_t),
	add_openapi_response_errors(openapi_resp_slurmdbd_config_t),
	add_openapi_response_warnings(openapi_resp_slurmdbd_config_t),
};
#undef add_parse_req

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_resp_accounts_add_cond_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_ACCOUNTS_ADD_COND_RESP)[] = {
	add_parse(ACCOUNTS_ADD_COND_PTR, add_assoc, "association_condition", "CSV list of accounts, association limits and options, CSV list of clusters"),
	add_parse(ACCOUNT_SHORT_PTR, acct, "account", "Account organization and description"),
	add_openapi_response_meta(openapi_resp_accounts_add_cond_t),
	add_openapi_response_errors(openapi_resp_accounts_add_cond_t),
	add_openapi_response_warnings(openapi_resp_accounts_add_cond_t),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_resp_users_add_cond_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_USERS_ADD_COND_RESP)[] = {
	add_parse(USERS_ADD_COND_PTR, add_assoc, "association_condition", "Filters to select associations for users"),
	add_parse(USER_SHORT_PTR, user, "user", "Admin level of user, DefaultAccount, DefaultWCKey"),
	add_openapi_response_meta(openapi_resp_users_add_cond_t),
	add_openapi_response_errors(openapi_resp_users_add_cond_t),
	add_openapi_response_warnings(openapi_resp_users_add_cond_t),
};
#undef add_parse

#define add_parse(mtype, field, path, desc) \
	add_parser(openapi_resp_job_state_t, mtype, false, field, 0, path, desc)
static const parser_t PARSER_ARRAY(OPENAPI_JOB_STATE_RESP)[] = {
	add_parse(JOB_STATE_RESP_MSG_PTR, jobs, "jobs", "List of job states"),
	add_openapi_response_meta(openapi_resp_job_state_t),
	add_openapi_response_errors(openapi_resp_job_state_t),
	add_openapi_response_warnings(openapi_resp_job_state_t),
};
#undef add_parse

#undef add_parser
#undef add_parser_skip
#undef add_complex_parser
#undef add_parse_bool
#undef add_openapi_response_single

/* add parser for a pointer */
#define addpp(typev, typet, typep, allow_null, newf, freef)                    \
	{                                                                      \
		.magic = MAGIC_PARSER,                                         \
		.model = PARSER_MODEL_PTR,                                     \
		.type = DATA_PARSER_##typev,                                   \
		.type_string = XSTRINGIFY(DATA_PARSER_ ## typev),              \
		.obj_type_string = XSTRINGIFY(typet),                          \
		.size = sizeof(typet),                                         \
		.needs = NEED_NONE,                                            \
		.ptr_offset = NO_VAL,                                          \
		.pointer_type = DATA_PARSER_##typep,                           \
		.allow_null_pointer = allow_null,                              \
		.new = newf,                                                   \
		.free = freef,                                                 \
	}
/* add parser array for struct */
#define addpa(typev, typet)                                       \
	{                                                                      \
		.magic = MAGIC_PARSER,                                         \
		.model = PARSER_MODEL_ARRAY,                                   \
		.type = DATA_PARSER_##typev,                                   \
		.type_string = XSTRINGIFY(DATA_PARSER_ ## typev),              \
		.obj_type_string = XSTRINGIFY(typet),                          \
		.size = sizeof(typet),                                         \
		.needs = NEED_NONE,                                            \
		.fields = PARSER_ARRAY(typev),                                 \
		.field_count = ARRAY_SIZE(PARSER_ARRAY(typev)),                \
		.ptr_offset = NO_VAL,                                          \
	}
/* add parser array (for struct) and pointer for parser array */
#define addpap(typev, typet, newf, freef)                                      \
	addpa(typev, typet),                                      \
	addpp(typev ## _PTR, typet *, typev, false, newf, freef)
/* add parser array (for struct) and nullable pointer for parser array */
#define addpanp(typev, typet, newf, freef)                                     \
	addpa(typev, typet),                                                   \
	addpp(typev ## _PTR, typet *, typev, true, newf, freef)
/* add parser for NULL terminated array of (sequential) objects */
#define addnt(typev, typea)                                                    \
	{                                                                      \
		.magic = MAGIC_PARSER,                                         \
		.model = PARSER_MODEL_NT_ARRAY,                                \
		.type = DATA_PARSER_##typev,                                   \
		.type_string = XSTRINGIFY(DATA_PARSER_ ## typev),              \
		.obj_type_string = "void **",                                  \
		.size = sizeof(void **),                                       \
		.needs = NEED_NONE,                                            \
		.ptr_offset = NO_VAL,                                          \
		.array_type = DATA_PARSER_##typea,                             \
	}
/* add parser for NULL terminated array of pointers to objects */
#define addntp(typev, typea)                                                   \
	{                                                                      \
		.magic = MAGIC_PARSER,                                         \
		.model = PARSER_MODEL_NT_PTR_ARRAY,                            \
		.type = DATA_PARSER_##typev,                                   \
		.type_string = XSTRINGIFY(DATA_PARSER_ ## typev),              \
		.obj_type_string = "void **",                                  \
		.size = sizeof(void **),                                       \
		.needs = NEED_NONE,                                            \
		.ptr_offset = NO_VAL,                                          \
		.array_type = DATA_PARSER_##typea,                             \
	}
/* add parser for List */
#define addpl(typev, typel, need)                                              \
	{                                                                      \
		.magic = MAGIC_PARSER,                                         \
		.model = PARSER_MODEL_LIST,                                    \
		.type = DATA_PARSER_##typev,                                   \
		.type_string = XSTRINGIFY(DATA_PARSER_ ## typev),              \
		.obj_type_string = XSTRINGIFY(list_t *),                       \
		.list_type = DATA_PARSER_##typel,                              \
		.size = sizeof(list_t *),                                      \
		.needs = need,                                                 \
		.ptr_offset = NO_VAL,                                          \
	}
/* add parser for simple type */
#define addps(typev, stype, need, typeo, newf, freef, desc)                    \
	{                                                                      \
		.magic = MAGIC_PARSER,                                         \
		.type = DATA_PARSER_##typev,                                   \
		.model = PARSER_MODEL_SIMPLE,                                  \
		.type_string = XSTRINGIFY(DATA_PARSER_ ## typev),              \
		.obj_desc = desc,                                              \
		.obj_type_string = XSTRINGIFY(stype),                          \
		.obj_openapi = OPENAPI_FORMAT_ ## typeo,                       \
		.size = sizeof(stype),                                         \
		.new = newf,                                                   \
		.free = freef,                                                 \
		.needs = need,                                                 \
		.parse = PARSE_FUNC(typev),                                    \
		.dump = DUMP_FUNC(typev),                                      \
		.ptr_offset = NO_VAL,                                          \
	}
/* add parser for simple type which creates an array*/
#define addpsa(typev, typea, stype, need, desc)                                \
	{                                                                      \
		.magic = MAGIC_PARSER,                                         \
		.type = DATA_PARSER_##typev,                                   \
		.array_type = DATA_PARSER_##typea,                             \
		.model = PARSER_MODEL_SIMPLE,                                  \
		.type_string = XSTRINGIFY(DATA_PARSER_ ## typev),              \
		.obj_desc = desc,                                              \
		.obj_type_string = XSTRINGIFY(stype),                          \
		.obj_openapi = OPENAPI_FORMAT_ARRAY,                           \
		.size = sizeof(stype),                                         \
		.needs = need,                                                 \
		.parse = PARSE_FUNC(typev),                                    \
		.dump = DUMP_FUNC(typev),                                      \
		.ptr_offset = NO_VAL,                                          \
	}
/* add parser for simple type which becomes another type */
#define addpsp(typev, typea, stype, need, desc)                                \
	{                                                                      \
		.magic = MAGIC_PARSER,                                         \
		.type = DATA_PARSER_##typev,                                   \
		.pointer_type = DATA_PARSER_##typea,                           \
		.model = PARSER_MODEL_SIMPLE,                                  \
		.type_string = XSTRINGIFY(DATA_PARSER_ ## typev),              \
		.obj_desc = desc,                                              \
		.obj_type_string = XSTRINGIFY(stype),                          \
		.obj_openapi = OPENAPI_FORMAT_INVALID,                         \
		.size = sizeof(stype),                                         \
		.needs = need,                                                 \
		.parse = PARSE_FUNC(typev),                                    \
		.dump = DUMP_FUNC(typev),                                      \
		.ptr_offset = NO_VAL,                                          \
	}
/* add parser for simple type with spec generator */
#define addpss(typev, stype, need, typeo, desc, newf, freef)                   \
	{                                                                      \
		.magic = MAGIC_PARSER,                                         \
		.type = DATA_PARSER_##typev,                                   \
		.model = PARSER_MODEL_SIMPLE,                                  \
		.type_string = XSTRINGIFY(DATA_PARSER_ ## typev),              \
		.obj_desc = desc,                                              \
		.obj_type_string = XSTRINGIFY(stype),                          \
		.obj_openapi = OPENAPI_FORMAT_ ## typeo,                       \
		.size = sizeof(stype),                                         \
		.new = newf,                                                   \
		.free = freef,                                                 \
		.needs = need,                                                 \
		.parse = PARSE_FUNC(typev),                                    \
		.dump = DUMP_FUNC(typev),                                      \
		.ptr_offset = NO_VAL,                                          \
	}
/* add parser for complex type */
#define addpc(typev, typet, need, typeo, desc)                                 \
	{                                                                      \
		.magic = MAGIC_PARSER,                                         \
		.type = DATA_PARSER_##typev,                                   \
		.model = PARSER_MODEL_COMPLEX,                                 \
		.type_string = XSTRINGIFY(DATA_PARSER_ ## typev),              \
		.obj_desc = desc,                                              \
		.obj_type_string = XSTRINGIFY(typet),                          \
		.obj_openapi = OPENAPI_FORMAT_ ## typeo,                       \
		.size = sizeof(typet),                                         \
		.needs = need,                                                 \
		.parse = PARSE_FUNC(typev),                                    \
		.dump = DUMP_FUNC(typev),                                      \
		.ptr_offset = NO_VAL,                                          \
	}
/* add parser for complex type which creates an array*/
#define addpca(typev, typea, typet, need, desc)                                \
	{                                                                      \
		.magic = MAGIC_PARSER,                                         \
		.type = DATA_PARSER_##typev,                                   \
		.array_type = DATA_PARSER_##typea,                             \
		.model = PARSER_MODEL_COMPLEX,                                 \
		.type_string = XSTRINGIFY(DATA_PARSER_ ## typev),              \
		.obj_desc = desc,                                              \
		.obj_type_string = XSTRINGIFY(typet),                          \
		.obj_openapi = OPENAPI_FORMAT_INVALID,                         \
		.size = sizeof(typet),                                         \
		.needs = need,                                                 \
		.parse = PARSE_FUNC(typev),                                    \
		.dump = DUMP_FUNC(typev),                                      \
		.ptr_offset = NO_VAL,                                          \
	}
/* add parser for complex type which becomes another type */
#define addpcp(typev, typea, typet, need, desc)                                \
	{                                                                      \
		.magic = MAGIC_PARSER,                                         \
		.type = DATA_PARSER_##typev,                                   \
		.pointer_type = DATA_PARSER_##typea,                           \
		.model = PARSER_MODEL_COMPLEX,                                 \
		.type_string = XSTRINGIFY(DATA_PARSER_ ## typev),              \
		.obj_desc = desc,                                              \
		.obj_type_string = XSTRINGIFY(typet),                          \
		.obj_openapi = OPENAPI_FORMAT_INVALID,                         \
		.size = sizeof(typet),                                         \
		.needs = need,                                                 \
		.parse = PARSE_FUNC(typev),                                    \
		.dump = DUMP_FUNC(typev),                                      \
		.ptr_offset = NO_VAL,                                          \
	}
/* add parser for complex type with spec generator */
#define addpcs(typev, typet, need, typeo, desc)                                 \
	{                                                                      \
		.magic = MAGIC_PARSER,                                         \
		.type = DATA_PARSER_##typev,                                   \
		.model = PARSER_MODEL_COMPLEX,                                 \
		.type_string = XSTRINGIFY(DATA_PARSER_ ## typev),              \
		.obj_desc = desc,                                              \
		.obj_type_string = XSTRINGIFY(typet),                          \
		.obj_openapi = OPENAPI_FORMAT_ ## typeo,                       \
		.size = sizeof(typet),                                         \
		.needs = need,                                                 \
		.parse = PARSE_FUNC(typev),                                    \
		.dump = DUMP_FUNC(typev),                                      \
		.ptr_offset = NO_VAL,                                          \
	}
#define addfa(typev, typet)                                                    \
	{                                                                      \
		.magic = MAGIC_PARSER,                                         \
		.model = PARSER_MODEL_FLAG_ARRAY,                              \
		.type = DATA_PARSER_##typev,                                   \
		.type_string = XSTRINGIFY(DATA_PARSER_ ## typev),              \
		.obj_type_string = XSTRINGIFY(typet),                          \
		.size = sizeof(typet),                                         \
		.needs = NEED_NONE,                                            \
		.flag_bit_array = PARSER_FLAG_ARRAY(typev),                    \
		.flag_bit_array_count = ARRAY_SIZE(PARSER_FLAG_ARRAY(typev)),  \
		.ptr_offset = NO_VAL,                                          \
	}
/* add OpenAPI singular response */
#define addoar(mtype) addpap(mtype, openapi_resp_single_t, NULL, NULL)
static const parser_t parsers[] = {
	/* Simple type parsers */
	addps(STRING, char *, NEED_NONE, STRING, NULL, NULL, NULL),
	addps(UINT32, uint32_t, NEED_NONE, INT32, NULL, NULL, NULL),
	addpsp(UINT32_NO_VAL, UINT32_NO_VAL_STRUCT, uint32_t, NEED_NONE, "32 bit integer number with flags"),
	addps(UINT64, uint64_t, NEED_NONE, INT64, NULL, NULL, NULL),
	addpsp(UINT64_NO_VAL, UINT64_NO_VAL_STRUCT, uint64_t, NEED_NONE, "64 bit integer number with flags"),
	addps(UINT16, uint16_t, NEED_NONE, INT32, NULL, NULL, NULL),
	addpsp(UINT16_NO_VAL, UINT16_NO_VAL_STRUCT, uint16_t, NEED_NONE, "16 bit integer number with flags"),
	addps(INT32, int32_t, NEED_NONE, INT32, NULL, NULL, NULL),
	addps(INT64, int64_t, NEED_NONE, INT64, NULL, NULL, NULL),
	addpsp(INT64_NO_VAL, INT64_NO_VAL_STRUCT, int64_t, NEED_NONE, "64 bit signed integer number with flags"),
	addps(FLOAT128, long double, NEED_NONE, NUMBER, NULL, NULL, NULL),
	addps(FLOAT64, double, NEED_NONE, DOUBLE, NULL, NULL, NULL),
	addpsp(FLOAT64_NO_VAL, FLOAT64_NO_VAL_STRUCT, double, NEED_NONE, "64 bit floating point number with flags"),
	addps(BOOL, uint8_t, NEED_NONE, BOOL, NULL, NULL, NULL),
	addps(BOOL16, uint16_t, NEED_NONE, BOOL, NULL, NULL, NULL),
	addps(BOOL16_NO_VAL, uint16_t, NEED_NONE, BOOL, NULL, NULL, NULL),
	addps(QOS_NAME, char *, NEED_QOS, STRING, NULL, NULL, NULL),
	addps(QOS_ID, uint32_t, NEED_QOS, STRING, NULL, NULL, NULL),
	addpsa(QOS_STRING_ID_LIST, STRING, list_t *, NEED_QOS, "List of QOS names"),
	addps(SLURMDB_RPC_ID, slurmdbd_msg_type_t, NEED_NONE, STRING, NULL, NULL, NULL),
	addps(SELECT_PLUGIN_ID, int, NEED_NONE, STRING, NULL, NULL, NULL),
	addps(TASK_DISTRIBUTION, uint32_t, NEED_NONE, STRING, NULL, NULL, NULL),
	addps(STEP_ID, uint32_t, NEED_NONE, STRING, NULL, NULL, NULL),
	addpsp(WCKEY_TAG, WCKEY_TAG_STRUCT, char *, NEED_NONE, "WCKey ID with tagging"),
	addps(GROUP_ID, gid_t, NEED_NONE, STRING, NULL, NULL, NULL),
	addps(JOB_REASON, uint32_t, NEED_NONE, STRING, NULL, NULL, NULL),
	addps(OVERSUBSCRIBE_JOBS, uint16_t, NEED_NONE, INT32, NULL, NULL, NULL),
	addps(USER_ID, uid_t, NEED_NONE, STRING, NULL, NULL, NULL),
	addpsp(TRES_STR, TRES_LIST, char *, NEED_TRES, NULL),
	addpsa(CSV_STRING, STRING, char *, NEED_NONE, NULL),
	addpsp(CSV_STRING_LIST, STRING_LIST, list_t *, NEED_NONE, NULL),
	addpsa(LICENSES, LICENSE, license_info_msg_t, NEED_NONE, NULL),
	addps(CORE_SPEC, uint16_t, NEED_NONE, INT32, NULL, NULL, NULL),
	addps(THREAD_SPEC, uint16_t, NEED_NONE, INT32, NULL, NULL, NULL),
	addps(NICE, uint32_t, NEED_NONE, INT32, NULL, NULL, NULL),
	addpsp(MEM_PER_CPUS, UINT64_NO_VAL, uint64_t, NEED_NONE, NULL),
	addpsp(MEM_PER_NODE, UINT64_NO_VAL, uint64_t, NEED_NONE, NULL),
	addps(CONTROLLER_PING_MODE, int, NEED_NONE, STRING, NULL, NULL, NULL),
	addps(CONTROLLER_PING_RESULT, bool, NEED_NONE, STRING, NULL, NULL, NULL),
	addpsa(HOSTLIST, STRING, hostlist_t *, NEED_NONE, NULL),
	addpsa(HOSTLIST_STRING, STRING, char *, NEED_NONE, NULL),
	addps(CPU_FREQ_FLAGS, uint32_t, NEED_NONE, STRING, NULL, NULL, NULL),
	addps(ERROR, int, NEED_NONE, STRING, NULL, NULL, NULL),
	addpsa(JOB_INFO_MSG, JOB_INFO, job_info_msg_t, NEED_NONE, NULL),
	addpsa(STRING_ARRAY, STRING, char **, NEED_NONE, NULL),
	addps(SIGNAL, uint16_t, NEED_NONE, STRING, NULL, NULL, NULL),
	addps(BITSTR, bitstr_t, NEED_NONE, STRING, NULL, NULL, NULL),
	addpsp(JOB_ARRAY_RESPONSE_MSG, JOB_ARRAY_RESPONSE_ARRAY, job_array_resp_msg_t, NEED_NONE, "Job update results"),
	addpsp(JOB_EXCLUSIVE, JOB_EXCLUSIVE_FLAGS, uint16_t, NEED_NONE, NULL),
	addps(HOLD, uint32_t, NEED_NONE, BOOL, NULL, NULL, "Job held"),
	addpsp(TIMESTAMP, UINT64, time_t, NEED_NONE, NULL),
	addpsp(TIMESTAMP_NO_VAL, UINT64_NO_VAL, time_t, NEED_NONE, NULL),
	addps(SELECTED_STEP, slurm_selected_step_t, NEED_NONE, STRING, NULL, NULL, NULL),
	addps(GROUP_ID_STRING, char *, NEED_NONE, STRING, NULL, NULL, NULL),
	addps(USER_ID_STRING, char *, NEED_NONE, STRING, NULL, NULL, NULL),
	addpsp(JOB_STATE_ID_STRING, JOB_STATE, char *, NEED_NONE, NULL),
	addpsp(QOS_NAME_CSV_LIST, STRING, list_t *, NEED_NONE, NULL),
	addpsp(QOS_ID_STRING, STRING, char *, NEED_NONE, NULL),
	addpsp(QOS_ID_STRING_CSV_LIST, STRING, list_t *, NEED_NONE, NULL),
	addpsp(ASSOC_ID_STRING, STRING, char *, NEED_NONE, NULL),
	addpsp(ASSOC_ID_STRING_CSV_LIST, STRING_LIST, list_t *, NEED_NONE, NULL),
	addpsp(PROCESS_EXIT_CODE, PROCESS_EXIT_CODE_VERBOSE, uint32_t, NEED_NONE, "return code returned by process"),
	addpsp(SLURM_STEP_ID_STRING, SELECTED_STEP, slurm_step_id_t, NEED_NONE, "Slurm Job StepId"),
	addps(RPC_ID, uint16_t, NEED_NONE, STRING, NULL, NULL, "Slurm RPC message type"),
	addpsa(JOB_STATE_RESP_MSG, JOB_STATE_RESP_JOB, job_state_response_msg_t, NEED_NONE, "List of jobs"),
	addps(EXT_SENSORS_DATA, void *, NEED_NONE, OBJECT, NULL, NULL, NULL),
	addps(POWER_FLAGS, uint8_t, NEED_NONE, ARRAY, NULL, NULL, NULL),

	/* Complex type parsers */
	addpcp(ASSOC_ID, UINT32, slurmdb_assoc_rec_t, NEED_ASSOC, "Association ID"),
	addpcp(JOB_ASSOC_ID, ASSOC_SHORT_PTR, slurmdb_job_rec_t, NEED_ASSOC, NULL),
	addpca(QOS_PREEMPT_LIST, STRING, slurmdb_qos_rec_t, NEED_QOS, NULL),
	addpcp(STEP_NODES, HOSTLIST, slurmdb_step_rec_t, NEED_TRES, NULL),
	addpca(STEP_TRES_REQ_MAX, TRES, slurmdb_step_rec_t, NEED_TRES, NULL),
	addpca(STEP_TRES_REQ_MIN, TRES, slurmdb_step_rec_t, NEED_TRES, NULL),
	addpca(STEP_TRES_USAGE_MAX, TRES, slurmdb_step_rec_t, NEED_TRES, NULL),
	addpca(STEP_TRES_USAGE_MIN, TRES, slurmdb_step_rec_t, NEED_TRES, NULL),
	addpcp(JOB_PLANNED_TIME, UINT64_NO_VAL, slurmdb_job_rec_t, NEED_NONE, NULL),
	addpc(STATS_MSG_CYCLE_MEAN, stats_info_response_msg_t, NEED_NONE, INT64, NULL),
	addpc(STATS_MSG_CYCLE_MEAN_DEPTH, stats_info_response_msg_t, NEED_NONE, INT64, NULL),
	addpc(STATS_MSG_CYCLE_PER_MIN, stats_info_response_msg_t, NEED_NONE, INT64, NULL),
	addpcp(STATS_MSG_SCHEDULE_EXIT, SCHEDULE_EXIT_FIELDS, stats_info_response_msg_t, NEED_NONE, NULL),
	addpc(STATS_MSG_BF_CYCLE_MEAN, stats_info_response_msg_t, NEED_NONE, INT64, NULL),
	addpc(STATS_MSG_BF_DEPTH_MEAN, stats_info_response_msg_t, NEED_NONE, INT64, NULL),
	addpc(STATS_MSG_BF_DEPTH_MEAN_TRY, stats_info_response_msg_t, NEED_NONE, INT64, NULL),
	addpc(STATS_MSG_BF_QUEUE_LEN_MEAN, stats_info_response_msg_t, NEED_NONE, INT64, NULL),
	addpc(STATS_MSG_BF_TABLE_SIZE_MEAN, stats_info_response_msg_t, NEED_NONE, INT64, NULL),
	addpc(STATS_MSG_BF_ACTIVE, stats_info_response_msg_t, NEED_NONE, BOOL, NULL),
	addpcp(STATS_MSG_BF_EXIT, BF_EXIT_FIELDS, stats_info_response_msg_t, NEED_NONE, NULL),
	addpca(STATS_MSG_RPCS_BY_TYPE, STATS_MSG_RPC_TYPE, stats_info_response_msg_t, NEED_NONE, "RPCs by type"),
	addpca(STATS_MSG_RPCS_BY_USER, STATS_MSG_RPC_USER, stats_info_response_msg_t, NEED_NONE, "RPCs by user"),
	addpca(STATS_MSG_RPCS_QUEUE, STATS_MSG_RPC_QUEUE, stats_info_response_msg_t, NEED_NONE, "Pending RPCs"),
	addpca(STATS_MSG_RPCS_DUMP, STATS_MSG_RPC_DUMP, stats_info_response_msg_t, NEED_NONE, "Pending RPCs by hostlist"),
	addpc(NODE_SELECT_ALLOC_MEMORY, node_info_t, NEED_NONE, INT64, NULL),
	addpc(NODE_SELECT_ALLOC_CPUS, node_info_t, NEED_NONE, INT32, NULL),
	addpc(NODE_SELECT_ALLOC_IDLE_CPUS, node_info_t, NEED_NONE, INT32, NULL),
	addpc(NODE_SELECT_TRES_USED, node_info_t, NEED_NONE, STRING, NULL),
	addpc(NODE_SELECT_TRES_WEIGHTED, node_info_t, NEED_NONE, DOUBLE, NULL),
	addpca(NODES, NODE, node_info_msg_t, NEED_NONE, NULL),
	addpca(JOB_INFO_GRES_DETAIL, STRING, slurm_job_info_t, NEED_NONE, NULL),
	addpca(JOB_RES_NODES, JOB_RES_NODE, JOB_RES_NODE_t, NEED_NONE, "Job resources for a node"),
	addpca(STEP_INFO_MSG, STEP_INFO, job_step_info_response_msg_t, NEED_TRES, NULL),
	addpca(PARTITION_INFO_MSG, PARTITION_INFO, partition_info_msg_t, NEED_TRES, NULL),
	addpca(RESERVATION_INFO_MSG, RESERVATION_INFO, reserve_info_msg_t, NEED_NONE, NULL),
	addpca(RESERVATION_INFO_CORE_SPEC, RESERVATION_CORE_SPEC, reserve_info_t, NEED_NONE, NULL),
	addpcp(JOB_DESC_MSG_ARGV, STRING_ARRAY, job_desc_msg_t, NEED_NONE, NULL),
	addpc(JOB_DESC_MSG_CPU_FREQ, job_desc_msg_t, NEED_NONE, STRING, NULL),
	addpcp(JOB_DESC_MSG_ENV, STRING_ARRAY, job_desc_msg_t, NEED_NONE, NULL),
	addpcp(JOB_DESC_MSG_SPANK_ENV, STRING_ARRAY, job_desc_msg_t, NEED_NONE, NULL),
	addpc(JOB_DESC_MSG_NODES, job_desc_msg_t, NEED_NONE, STRING, NULL),
	addpc(JOB_INFO_STDIN, slurm_job_info_t, NEED_NONE, STRING, NULL),
	addpc(JOB_INFO_STDOUT, slurm_job_info_t, NEED_NONE, STRING, NULL),
	addpc(JOB_INFO_STDERR, slurm_job_info_t, NEED_NONE, STRING, NULL),
	addpc(JOB_USER, slurmdb_job_rec_t, NEED_NONE, STRING, NULL),
	addpcp(JOB_CONDITION_SUBMIT_TIME, TIMESTAMP_NO_VAL, slurmdb_job_cond_t, NEED_NONE, NULL),
	addpcp(JOB_DESC_MSG_RLIMIT_CPU, UINT64_NO_VAL, job_desc_msg_t, NEED_NONE, "Per-process CPU limit, in seconds."),
	addpcp(JOB_DESC_MSG_RLIMIT_FSIZE, UINT64_NO_VAL, job_desc_msg_t, NEED_NONE, "Largest file that can be created, in bytes."),
	addpcp(JOB_DESC_MSG_RLIMIT_DATA, UINT64_NO_VAL, job_desc_msg_t, NEED_NONE, "Maximum size of data segment, in bytes. "),
	addpcp(JOB_DESC_MSG_RLIMIT_STACK, UINT64_NO_VAL, job_desc_msg_t, NEED_NONE, "Maximum size of stack segment, in bytes."),
	addpcp(JOB_DESC_MSG_RLIMIT_CORE, UINT64_NO_VAL, job_desc_msg_t, NEED_NONE, "Largest core file that can be created, in bytes."),
	addpcp(JOB_DESC_MSG_RLIMIT_RSS, UINT64_NO_VAL, job_desc_msg_t, NEED_NONE, "Largest resident set size, in bytes. This affects swapping; processes that are exceeding their resident set size will be more likely to have physical memory taken from them."),
	addpcp(JOB_DESC_MSG_RLIMIT_NPROC, UINT64_NO_VAL, job_desc_msg_t, NEED_NONE, "Number of processes."),
	addpcp(JOB_DESC_MSG_RLIMIT_NOFILE, UINT64_NO_VAL, job_desc_msg_t, NEED_NONE, "Number of open files."),
	addpcp(JOB_DESC_MSG_RLIMIT_MEMLOCK, UINT64_NO_VAL, job_desc_msg_t, NEED_NONE, "Locked-in-memory address space"),
	addpcp(JOB_DESC_MSG_RLIMIT_AS, UINT64_NO_VAL, job_desc_msg_t, NEED_NONE, "Address space limit."),
	addpca(ASSOC_SHARES_OBJ_LIST, ASSOC_SHARES_OBJ_WRAP, shares_response_msg_t, NEED_NONE, NULL),
	addpcp(ASSOC_SHARES_OBJ_WRAP_TRES_RUN_SECS, SHARES_UINT64_TRES_LIST, assoc_shares_object_wrap_t, NEED_NONE, NULL),
	addpcp(ASSOC_SHARES_OBJ_WRAP_TRES_GRP_MINS, SHARES_UINT64_TRES_LIST, assoc_shares_object_wrap_t, NEED_NONE, NULL),
	addpcp(ASSOC_SHARES_OBJ_WRAP_TRES_USAGE_RAW, SHARES_FLOAT128_TRES_LIST, assoc_shares_object_wrap_t, NEED_NONE, NULL),
	addpcp(JOB_STATE_RESP_JOB_JOB_ID, STRING, job_state_response_job_t, NEED_NONE, NULL),

	/* NULL terminated model parsers */
	addnt(CONTROLLER_PING_ARRAY, CONTROLLER_PING),
	addntp(NODE_ARRAY, NODE),
	addntp(PARTITION_INFO_ARRAY, PARTITION_INFO),
	addntp(STEP_INFO_ARRAY, STEP_INFO),
	addntp(RESERVATION_INFO_ARRAY, RESERVATION_INFO),
	addntp(JOB_ARRAY_RESPONSE_ARRAY, JOB_ARRAY_RESPONSE_MSG_ENTRY),
	addnt(JOB_RES_SOCKET_ARRAY, JOB_RES_SOCKET),
	addnt(JOB_RES_CORE_ARRAY, JOB_RES_CORE),

	/* Pointer model parsers */
	addpp(JOB_ARRAY_RESPONSE_MSG_PTR, job_array_resp_msg_t *, JOB_ARRAY_RESPONSE_MSG, false, NULL, NULL),
	addpp(NODES_PTR, node_info_msg_t *, NODES, false, NULL, NULL),
	addpp(LICENSES_PTR, license_info_msg_t *, LICENSES, false, NULL, NULL),
	addpp(JOB_INFO_MSG_PTR, job_info_msg_t *, JOB_INFO_MSG, false, NULL, NULL),
	addpp(PARTITION_INFO_MSG_PTR, partition_info_msg_t *, PARTITION_INFO_MSG, false, NULL, NULL),
	addpp(RESERVATION_INFO_MSG_PTR, reserve_info_msg_t *, RESERVATION_INFO_MSG, false, NULL, NULL),
	addpp(SELECTED_STEP_PTR, slurm_selected_step_t *, SELECTED_STEP, false, NULL, NULL),
	addpp(SLURM_STEP_ID_STRING_PTR, slurm_step_id_t *, SLURM_STEP_ID_STRING, false, NULL, NULL),
	addpp(STEP_INFO_MSG_PTR, job_step_info_response_msg_t *, STEP_INFO_MSG, false, NULL, NULL),
	addpp(BITSTR_PTR, bitstr_t *, BITSTR, false, NULL, NULL),
	addpp(JOB_STATE_RESP_MSG_PTR, job_state_response_msg_t *, JOB_STATE_RESP_MSG, false, NULL, NULL),
	addpp(EXT_SENSORS_DATA_PTR, void *, EXT_SENSORS_DATA, true, NULL, NULL),

	/* Array of parsers */
	addpap(ASSOC_SHORT, slurmdb_assoc_rec_t, NEW_FUNC(ASSOC), slurmdb_destroy_assoc_rec),
	addpap(ASSOC, slurmdb_assoc_rec_t, NEW_FUNC(ASSOC), slurmdb_destroy_assoc_rec),
	addpap(ASSOC_REC_SET, slurmdb_assoc_rec_t, NEW_FUNC(ASSOC), slurmdb_destroy_assoc_rec),
	addpap(INSTANCE, slurmdb_instance_rec_t, NEW_FUNC(INSTANCE), slurmdb_destroy_instance_rec),
	addpap(USER, slurmdb_user_rec_t, NEW_FUNC(USER), slurmdb_destroy_user_rec),
	addpap(USER_SHORT, slurmdb_user_rec_t, NULL, slurmdb_destroy_user_rec),
	addpap(JOB, slurmdb_job_rec_t, (parser_new_func_t) slurmdb_create_job_rec, slurmdb_destroy_job_rec),
	addpap(STEP, slurmdb_step_rec_t, (parser_new_func_t) slurmdb_create_step_rec, slurmdb_destroy_step_rec),
	addpap(ACCOUNT, slurmdb_account_rec_t, NEW_FUNC(ACCOUNT), slurmdb_destroy_account_rec),
	addpap(ACCOUNT_SHORT, slurmdb_account_rec_t, NULL, slurmdb_destroy_account_rec),
	addpap(ACCOUNTING, slurmdb_accounting_rec_t, NULL, slurmdb_destroy_accounting_rec),
	addpap(ACCOUNTS_ADD_COND, slurmdb_add_assoc_cond_t, NEW_FUNC(ACCOUNTS_ADD_COND), slurmdb_destroy_add_assoc_cond),
	/* Re-use already existing NEW_FUNC */
	addpap(USERS_ADD_COND, slurmdb_add_assoc_cond_t, NEW_FUNC(ACCOUNTS_ADD_COND), slurmdb_destroy_add_assoc_cond),
	addpap(COORD, slurmdb_coord_rec_t, NULL, slurmdb_destroy_coord_rec),
	addpap(WCKEY, slurmdb_wckey_rec_t, NEW_FUNC(WCKEY), slurmdb_destroy_wckey_rec),
	addpap(TRES, slurmdb_tres_rec_t, NULL, slurmdb_destroy_tres_rec),
	addpap(TRES_NCT, slurmdb_tres_nct_rec_t, NULL, FREE_FUNC(TRES_NCT)),
	addpap(QOS, slurmdb_qos_rec_t, NEW_FUNC(QOS), slurmdb_destroy_qos_rec),
	addpap(STATS_REC, slurmdb_stats_rec_t, NULL, NULL),
	addpap(CLUSTER_REC, slurmdb_cluster_rec_t, NEW_FUNC(CLUSTER_REC), slurmdb_destroy_cluster_rec),
	addpap(CLUSTER_ACCT_REC, slurmdb_cluster_accounting_rec_t, NULL, slurmdb_destroy_clus_res_rec),
	addpap(ASSOC_USAGE, slurmdb_assoc_usage_t, NULL, NULL),
	addpap(STATS_RPC, slurmdb_rpc_obj_t, NULL, NULL),
	addpap(STATS_USER, slurmdb_rpc_obj_t, NULL, NULL),
	addpap(STATS_MSG, stats_info_response_msg_t, NULL, NULL),
	addpap(NODE, node_info_t, NULL, NULL),
	addpap(LICENSE, slurm_license_info_t, NULL, NULL),
	addpap(JOB_INFO, slurm_job_info_t, NULL, NULL),
	addpap(JOB_RES, job_resources_t, NULL, NULL),
	addpap(CONTROLLER_PING, controller_ping_t, NULL, NULL),
	addpap(STEP_INFO, job_step_info_t, NULL, NULL),
	addpap(PARTITION_INFO, partition_info_t, NULL, NULL),
	addpap(SINFO_DATA, sinfo_data_t, NULL, NULL),
	addpap(ACCT_GATHER_ENERGY, acct_gather_energy_t, NULL, NULL),
	addpap(POWER_MGMT_DATA, power_mgmt_data_t, NULL, NULL),
	addpap(RESERVATION_INFO, reserve_info_t, NULL, NULL),
	addpap(RESERVATION_CORE_SPEC, resv_core_spec_t, NULL, NULL),
	addpap(JOB_SUBMIT_RESPONSE_MSG, submit_response_msg_t, NULL, NULL),
	addpap(JOB_DESC_MSG, job_desc_msg_t, NEW_FUNC(JOB_DESC_MSG), (parser_free_func_t) slurm_free_job_desc_msg),
	addpap(CRON_ENTRY, cron_entry_t, NULL, NULL),
	addpap(UPDATE_NODE_MSG, update_node_msg_t, NULL, NULL),
	addpanp(OPENAPI_META, openapi_resp_meta_t, NULL, free_openapi_resp_meta),
	addpap(OPENAPI_ERROR, openapi_resp_error_t, NULL, free_openapi_resp_error),
	addpap(OPENAPI_WARNING, openapi_resp_warning_t, NULL, free_openapi_resp_warning),
	addpap(INSTANCE_CONDITION, slurmdb_instance_cond_t, NULL, slurmdb_destroy_instance_cond),
	addpap(JOB_SUBMIT_REQ, openapi_job_submit_request_t, NULL, NULL),
	addpap(JOB_CONDITION, slurmdb_job_cond_t, NULL, slurmdb_destroy_job_cond),
	addpap(QOS_CONDITION, slurmdb_qos_cond_t, NULL, slurmdb_destroy_qos_cond),
	addpap(ASSOC_CONDITION, slurmdb_assoc_cond_t, NULL, slurmdb_destroy_assoc_cond),
	addpap(USER_CONDITION, slurmdb_user_cond_t, NULL, slurmdb_destroy_user_cond),
	addpap(OPENAPI_SLURMDBD_JOB_PARAM, openapi_job_param_t, NULL, NULL),
	addpap(OPENAPI_USER_PARAM, openapi_user_param_t, NULL, NULL),
	addpap(OPENAPI_USER_QUERY, openapi_user_query_t, NULL, NULL),
	addpap(OPENAPI_WCKEY_PARAM, openapi_wckey_param_t, NULL, NULL),
	addpap(WCKEY_CONDITION, slurmdb_wckey_cond_t, NULL, slurmdb_destroy_wckey_rec),
	addpap(OPENAPI_ACCOUNT_PARAM, openapi_account_param_t, NULL, NULL),
	addpap(OPENAPI_ACCOUNT_QUERY, openapi_account_query_t, NULL, NULL),
	addpap(ACCOUNT_CONDITION, slurmdb_account_cond_t, NULL, slurmdb_destroy_account_cond),
	addpap(OPENAPI_CLUSTER_PARAM, openapi_cluster_param_t, NULL, slurmdb_destroy_cluster_cond),
	addpap(CLUSTER_CONDITION, slurmdb_cluster_cond_t, NEW_FUNC(CLUSTER_CONDITION), slurmdb_destroy_cluster_cond),
	addpap(OPENAPI_JOB_INFO_PARAM, openapi_job_info_param_t, NULL, NULL),
	addpap(OPENAPI_JOB_INFO_DELETE_QUERY, openapi_job_info_delete_query_t, NULL, NULL),
	addpap(OPENAPI_JOB_INFO_QUERY, openapi_job_info_query_t, NULL, NULL),
	addpap(OPENAPI_NODE_PARAM, openapi_node_param_t, NULL, NULL),
	addpap(OPENAPI_NODES_QUERY, openapi_nodes_query_t, NULL, NULL),
	addpap(OPENAPI_PARTITION_PARAM, openapi_partition_param_t, NULL, NULL),
	addpap(OPENAPI_PARTITIONS_QUERY, openapi_partitions_query_t, NULL, NULL),
	addpap(OPENAPI_RESERVATION_PARAM, openapi_reservation_param_t, NULL, NULL),
	addpap(OPENAPI_RESERVATION_QUERY, openapi_reservation_query_t, NULL, NULL),
	addpap(PROCESS_EXIT_CODE_VERBOSE, proc_exit_code_verbose_t, NULL, NULL),
	addpap(SLURM_STEP_ID, slurm_step_id_t, NULL, NULL),
	addpap(SHARES_REQ_MSG, shares_request_msg_t, NEW_FUNC(SHARES_REQ_MSG), FREE_FUNC(SHARES_REQ_MSG)),
	addpap(SHARES_RESP_MSG, shares_response_msg_t, NULL, NULL),
	addpap(ASSOC_SHARES_OBJ_WRAP, assoc_shares_object_wrap_t, NULL, NULL),
	addpap(SHARES_UINT64_TRES, SHARES_UINT64_TRES_t, NULL, NULL),
	addpap(SHARES_FLOAT128_TRES, SHARES_FLOAT128_TRES_t, NULL, NULL),
	addpap(OPENAPI_SLURMDBD_QOS_PARAM, openapi_qos_param_t, NULL, NULL),
	addpap(OPENAPI_SLURMDBD_QOS_QUERY, openapi_qos_query_t, NULL, NULL),
	addpap(JOB_ARRAY_RESPONSE_MSG_ENTRY, JOB_ARRAY_RESPONSE_MSG_entry_t, NULL, NULL),
	addpap(WCKEY_TAG_STRUCT, WCKEY_TAG_STRUCT_t, NULL, NULL),
	addpap(OPENAPI_ACCOUNTS_ADD_COND_RESP, openapi_resp_accounts_add_cond_t, NULL, NULL),
	addpap(OPENAPI_USERS_ADD_COND_RESP, openapi_resp_users_add_cond_t, NULL, NULL),
	addpap(SCHEDULE_EXIT_FIELDS, schedule_exit_fields_t, NULL, NULL),
	addpap(BF_EXIT_FIELDS, bf_exit_fields_t, NULL, NULL),
	addpap(FLOAT64_NO_VAL_STRUCT, FLOAT64_NO_VAL_t, NULL, NULL),
	addpap(UINT64_NO_VAL_STRUCT, UINT64_NO_VAL_t, NULL, NULL),
	addpap(UINT32_NO_VAL_STRUCT, UINT32_NO_VAL_t, NULL, NULL),
	addpap(UINT16_NO_VAL_STRUCT, UINT16_NO_VAL_t, NULL, NULL),
	addpap(INT64_NO_VAL_STRUCT, UINT64_NO_VAL_t, NULL, NULL),
	addpap(JOB_RES_NODE, JOB_RES_NODE_t, NULL, NULL),
	addpap(JOB_RES_SOCKET, JOB_RES_SOCKET_t, NULL, NULL),
	addpap(JOB_RES_CORE, JOB_RES_CORE_t, NULL, NULL),
	addpap(ROLLUP_STATS, slurmdb_rollup_stats_t, NULL, NULL),
	addpap(STATS_MSG_RPC_TYPE, STATS_MSG_RPC_TYPE_t, NULL, NULL),
	addpap(STATS_MSG_RPC_USER, STATS_MSG_RPC_USER_t, NULL, NULL),
	addpap(STATS_MSG_RPC_QUEUE, STATS_MSG_RPC_QUEUE_t, NULL, NULL),
	addpap(STATS_MSG_RPC_DUMP, STATS_MSG_RPC_DUMP_t, NULL, NULL),
	addpap(JOB_STATE_RESP_JOB, job_state_response_job_t, NULL, NULL),
	addpap(OPENAPI_JOB_STATE_QUERY, openapi_job_state_query_t, NULL, NULL),

	/* OpenAPI responses */
	addoar(OPENAPI_RESP),
	addoar(OPENAPI_DIAG_RESP),
	addoar(OPENAPI_PING_ARRAY_RESP),
	addpap(OPENAPI_LICENSES_RESP, openapi_resp_license_info_msg_t, NULL, NULL),
	addpap(OPENAPI_JOB_INFO_RESP, openapi_resp_job_info_msg_t, NULL, NULL),
	addpap(OPENAPI_JOB_POST_RESPONSE, openapi_job_post_response_t, NULL, NULL),
	addpap(OPENAPI_JOB_SUBMIT_RESPONSE, openapi_job_submit_response_t, NULL, NULL),
	addpap(OPENAPI_NODES_RESP, openapi_resp_node_info_msg_t, NULL, NULL),
	addpap(OPENAPI_PARTITION_RESP, openapi_resp_partitions_info_msg_t, NULL, NULL),
	addpap(OPENAPI_RESERVATION_RESP, openapi_resp_reserve_info_msg_t, NULL, NULL),
	addoar(OPENAPI_ACCOUNTS_ADD_COND_RESP_STR),
	addoar(OPENAPI_ACCOUNTS_RESP),
	addoar(OPENAPI_ACCOUNTS_REMOVED_RESP),
	addoar(OPENAPI_ASSOCS_RESP),
	addoar(OPENAPI_ASSOCS_REMOVED_RESP),
	addoar(OPENAPI_CLUSTERS_RESP),
	addoar(OPENAPI_CLUSTERS_REMOVED_RESP),
	addoar(OPENAPI_INSTANCES_RESP),
	addpap(OPENAPI_SLURMDBD_CONFIG_RESP, openapi_resp_slurmdbd_config_t, NULL, NULL),
	addoar(OPENAPI_SLURMDBD_STATS_RESP),
	addoar(OPENAPI_SLURMDBD_JOBS_RESP),
	addoar(OPENAPI_SLURMDBD_QOS_RESP),
	addoar(OPENAPI_SLURMDBD_QOS_REMOVED_RESP),
	addoar(OPENAPI_TRES_RESP),
	addoar(OPENAPI_USERS_ADD_COND_RESP_STR),
	addoar(OPENAPI_USERS_RESP),
	addoar(OPENAPI_USERS_REMOVED_RESP),
	addoar(OPENAPI_WCKEY_RESP),
	addoar(OPENAPI_WCKEY_REMOVED_RESP),
	addoar(OPENAPI_SHARES_RESP),
	addoar(OPENAPI_SINFO_RESP),
	addpap(OPENAPI_STEP_INFO_MSG, openapi_resp_job_step_info_msg_t, NULL, NULL),
	addpap(OPENAPI_JOB_STATE_RESP, openapi_resp_job_state_t, NULL, NULL),

	/* Flag bit arrays */
	addfa(ASSOC_FLAGS, uint16_t),
	addfa(USER_FLAGS, uint32_t),
	addfa(SLURMDB_JOB_FLAGS, uint32_t),
	addfa(ACCOUNT_FLAGS, uint32_t),
	addfa(WCKEY_FLAGS, uint32_t),
	addfa(QOS_FLAGS, uint32_t),
	addfa(QOS_PREEMPT_MODES, uint16_t),
	addfa(CLUSTER_REC_FLAGS, uint32_t),
	addfa(NODE_STATES, uint32_t),
	addfa(PARTITION_STATES, uint16_t),
	addfa(JOB_FLAGS, uint64_t),
	addfa(JOB_SHOW_FLAGS, uint16_t),
	addfa(JOB_MAIL_FLAGS, uint16_t),
	addfa(RESERVATION_FLAGS, uint64_t),
	addfa(CPU_BINDING_FLAGS, uint16_t), /* cpu_bind_type_t */
	addfa(CRON_ENTRY_FLAGS, uint32_t),
	addfa(MEMORY_BINDING_TYPE, uint16_t), /* mem_bind_type_t */
	addfa(WARN_FLAGS, uint16_t),
	addfa(X11_FLAGS, uint16_t),
	addfa(OPEN_MODE, uint8_t),
	addfa(ACCT_GATHER_PROFILE, uint32_t),
	addfa(ADMIN_LVL, uint16_t), /* slurmdb_admin_level_t */
	addfa(JOB_SHARED, uint16_t),
	addfa(JOB_EXCLUSIVE_FLAGS, uint16_t),
	addfa(OVERSUBSCRIBE_FLAGS, uint16_t),
	addfa(JOB_CONDITION_FLAGS, uint32_t),
	addfa(JOB_CONDITION_DB_FLAGS, uint32_t),
	addfa(CLUSTER_CLASSIFICATION, uint16_t), /* slurmdb_classification_type_t */
	addfa(FLAGS, data_parser_flags_t),
	addfa(JOB_STATE, uint32_t), /* enum job_states */
	addfa(PROCESS_EXIT_CODE_STATUS, uint32_t),
	addfa(STEP_NAMES, uint32_t),
	addfa(ASSOC_SHARES_OBJ_WRAP_TYPE, uint16_t),
	addfa(WCKEY_TAG_FLAGS, WCKEY_TAG_FLAGS_t),
	addfa(NEED_PREREQS_FLAGS, need_t),
	addfa(CR_TYPE, uint16_t),
	addfa(NODE_CR_TYPE, uint32_t),
	addfa(JOB_RES_CORE_STATUS, JOB_RES_CORE_status_t),

	/* List parsers */
	addpl(QOS_LIST, QOS_PTR, NEED_QOS),
	addpl(QOS_NAME_LIST, QOS_NAME, NEED_QOS),
	addpl(QOS_ID_LIST, QOS_ID, NEED_QOS),
	addpl(QOS_STRING_ID_LIST, STRING, NEED_QOS),
	addpl(USER_LIST, USER_PTR, NEED_NONE),
	addpl(WCKEY_LIST, WCKEY_PTR, NEED_NONE),
	addpl(ACCOUNT_LIST, ACCOUNT_PTR, NEED_NONE),
	addpl(ACCOUNTING_LIST, ACCOUNTING_PTR, NEED_NONE),
	addpl(CLUSTER_REC_LIST, CLUSTER_REC_PTR, NEED_NONE),
	addpl(ASSOC_LIST, ASSOC_PTR, NEED_NONE),
	addpl(ASSOC_SHORT_LIST, ASSOC_SHORT_PTR, NEED_NONE),
	addpl(COORD_LIST, COORD_PTR, NEED_NONE),
	addpl(CLUSTER_ACCT_REC_LIST, CLUSTER_ACCT_REC_PTR, NEED_NONE),
	addpl(INSTANCE_LIST, INSTANCE_PTR, NEED_NONE),
	addpl(JOB_LIST, JOB_PTR, NEED_NONE),
	addpl(STEP_LIST, STEP_PTR, NEED_NONE),
	addpl(STATS_RPC_LIST, STATS_RPC_PTR, NEED_NONE),
	addpl(STATS_USER_LIST, STATS_USER_PTR, NEED_NONE),
	addpl(TRES_LIST, TRES_PTR, NEED_NONE),
	addpl(SINFO_DATA_LIST, SINFO_DATA_PTR, NEED_NONE),
	addpl(JOB_DESC_MSG_LIST, JOB_DESC_MSG_PTR, NEED_NONE),
	addpl(OPENAPI_ERRORS, OPENAPI_ERROR_PTR, NEED_NONE),
	addpl(OPENAPI_WARNINGS, OPENAPI_WARNING_PTR, NEED_NONE),
	addpl(STRING_LIST, STRING, NEED_NONE),
	addpl(SELECTED_STEP_LIST, SELECTED_STEP_PTR, NEED_NONE),
	addpl(GROUP_ID_STRING_LIST, GROUP_ID_STRING, NEED_NONE),
	addpl(USER_ID_STRING_LIST, USER_ID_STRING, NEED_NONE),
	addpl(JOB_STATE_ID_STRING_LIST, JOB_STATE_ID_STRING, NEED_NONE),
	addpl(SHARES_UINT64_TRES_LIST, SHARES_UINT64_TRES_PTR, NEED_NONE),
	addpl(SHARES_FLOAT128_TRES_LIST, SHARES_FLOAT128_TRES_PTR, NEED_NONE),
	addpl(SLURM_STEP_ID_STRING_LIST, SLURM_STEP_ID_STRING_PTR, NEED_NONE),
};
#undef addpl
#undef addps
#undef addpc
#undef addpa
#undef addoar

// clang-format on

extern void get_parsers(const parser_t **parsers_ptr, int *count_ptr)
{
	*count_ptr = ARRAY_SIZE(parsers);
	*parsers_ptr = parsers;
}

extern const parser_t *const find_parser_by_type(type_t type)
{
	for (int i = 0; i < ARRAY_SIZE(parsers); i++)
		if (parsers[i].type == type)
			return &parsers[i];

	return NULL;
}

extern void parsers_init(void)
{
#ifndef NDEBUG
	/* sanity check the parsers */
	for (int i = 0; i < ARRAY_SIZE(parsers); i++)
		check_parser(&parsers[i]);
#endif /* !NDEBUG */
}

#ifndef NDEBUG

extern void verify_parser_not_sliced_funcname(const parser_t *const parser,
					      const char *func,
					      const char *file, int line)
{
	for (int i = 0; i < ARRAY_SIZE(parsers); i++) {
		if (parsers[i].fields) {
			const parser_t *const fparser = parsers[i].fields;
			for (int j = 0; j < parsers[i].field_count; j++)
				if (&fparser[j] == parser)
					fatal_abort("%s: direct reference of linking parser %s(0x%" PRIxPTR ") inside of parser array %s(0x%" PRIxPTR ")[%d]=%s(0x%" PRIxPTR ") detected as %s:%d",
						func, parser->type_string,
						(uintptr_t) parser,
						fparser->type_string,
						(uintptr_t) fparser, j,
						fparser[j].type_string,
						(uintptr_t) &fparser[j], file,
						line);
		}
	}
}

extern void verify_parser_sliced_funcname(const parser_t *const parser,
					  const char *func, const char *file,
					  int line)
{
	for (int i = 0; i < ARRAY_SIZE(parsers); i++) {
		if (&parsers[i] == parser) {
			fatal_abort("%s: expected linking parser %s(0x%" PRIxPTR ") inside of parser array %s:%d",
				    func, parser->type_string,
				    (uintptr_t) parser, file, line);
		}

		if (parsers[i].fields) {
			const parser_t *const fparser = parsers[i].fields;
			for (int j = 0; j < parsers[i].field_count; j++)
				if (&fparser[j] == parser)
					return;
		}
	}

	fatal_abort("%s: orphan parser %s(0x%" PRIxPTR ") detected", func,
		    parser->type_string, (uintptr_t) parser);
}

#endif /* !NDEBUG */
