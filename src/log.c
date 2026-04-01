/* Copyright (c) 2006-2026 Jonas Fonseca <jonas.fonseca@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "tig/refdb.h"
#include "tig/display.h"
#include "tig/draw.h"
#include "tig/log.h"
#include "tig/diff.h"
#include "tig/parse.h"
#include "tig/pager.h"

struct log_state {
	/* Used for tracking when we need to recalculate the previous
	 * commit, for example when the user scrolls up or uses the page
	 * up/down in the log view. */
	int last_lineno;
	size_t graph_indent;
	enum line_type last_type;
	bool commit_title_read;
	bool after_commit_header;
	bool reading_diff_stat;
};

static inline void
log_copy_rev(struct view *view, struct line *line)
{
	const char *text = box_text(line);
	size_t offset = get_graph_indent(text);

	string_copy_rev_from_commit_line(view->ref, text + offset);
	view->env->blob[0] = 0;
}

static const struct ident *
log_parse_pretty_ident(const char *line, const char *prefix)
{
	char text[SIZEOF_STR];
	char *ident = text;
	char *email = NULL;
	char *name_end;
	char *email_end;

	string_ncopy(text, line, strlen(line));

	if (!prefixcmp(ident, prefix))
		return NULL;

	ident = string_trim(ident + strlen(prefix));
	email = strchr(ident, '<');
	if (!email)
		return get_author(ident, "");

	name_end = email;
	while (name_end > ident && isspace((unsigned char) name_end[-1]))
		name_end--;
	*name_end = 0;

	email++;
	email_end = strchr(email, '>');
	if (!email_end)
		return get_author(ident, "");
	*email_end = 0;

	return get_author(ident, email);
}

static void
log_set_authors(struct view *view, struct line *line)
{
	const struct ident *author = NULL;
	const struct ident *committer = NULL;
	struct line *commit_line = find_prev_line_by_type(view, line, LINE_COMMIT);
	struct line *entry;

	if (!commit_line)
		return;

	for (entry = commit_line + 1; entry < view->line + view->lines; entry++) {
		const char *text = box_text(entry);
		size_t offset;
		char *trimmed;

		if (entry->type == LINE_COMMIT)
			break;

		offset = get_graph_indent(text);
		trimmed = (char *) text + offset;

		if (!*trimmed)
			break;

		if (entry->type == LINE_AUTHOR) {
			struct time author_time;
			parse_author_line(trimmed + STRING_SIZE("author "), &author, &author_time);
			continue;
		}

		if (entry->type == LINE_COMMITTER) {
			struct time commit_time;
			parse_author_line(trimmed + STRING_SIZE("committer "), &committer, &commit_time);
			continue;
		}

		if (!author)
			author = log_parse_pretty_ident(trimmed, "Author:");
		if (!committer)
			committer = log_parse_pretty_ident(trimmed, "Commit:");
	}

	argv_env_set_authors(view->env, author, committer);
}

static void
log_select(struct view *view, struct line *line)
{
	struct log_state *state = view->private;
	int last_lineno = state->last_lineno;
	const char *text = box_text(line);

	if (!last_lineno || abs(last_lineno - line->lineno) > 1
	    || (state->last_type == LINE_COMMIT && last_lineno > line->lineno)) {
		struct line *commit_line = find_prev_line_by_type(view, line, LINE_COMMIT);

		if (commit_line)
			log_copy_rev(view, commit_line);
	}

	if (line->type == LINE_COMMIT && !view_has_flags(view, VIEW_NO_REF))
		log_copy_rev(view, line);
	string_copy_rev(view->env->commit, view->ref);
	log_set_authors(view, line);
	string_ncopy(view->env->text, text, strlen(text));
	state->last_lineno = line->lineno;
	state->last_type = line->type;
}

static enum status_code
log_open(struct view *view, enum open_flags flags)
{
	const char *log_argv[] = {
		"git", "log", encoding_arg, commit_order_arg(),
			use_mailmap_arg(), "%(logargs)", "%(cmdlineargs)",
			"%(revargs)", "--no-color", "--", "%(fileargs)", NULL
	};
	enum status_code code;

	code = begin_update(view, NULL, log_argv, flags | OPEN_WITH_STDERR);
	if (code != SUCCESS)
		return code;

	watch_register(&view->watch, WATCH_HEAD | WATCH_REFS);

	return SUCCESS;
}

static enum request
log_request(struct view *view, enum request request, struct line *line)
{
	enum open_flags flags = view_is_displayed(view) ? OPEN_SPLIT : OPEN_DEFAULT;

	switch (request) {
	case REQ_REFRESH:
		load_refs(true);
		refresh_view(view);
		return REQ_NONE;

	case REQ_EDIT:
		return diff_common_edit(view, request, line);

	case REQ_ENTER:
		if (!display[1] || strcmp(display[1]->vid, view->ref))
			open_diff_view(view, flags);
		return REQ_NONE;

	default:
		return request;
	}
}

static bool
log_read(struct view *view, struct buffer *buf, bool force_stop)
{
	struct line *line = NULL;
	enum line_type type = LINE_DEFAULT;
	struct log_state *state = view->private;
	size_t len;
	char *commit;
	char *data;

	if (!buf)
		return true;

	data = buf->data;
	commit = strstr(data, "commit ");
	if (commit && get_graph_indent(data) == commit - data)
		state->graph_indent = commit - data;

	len = strlen(data);
	if (len >= state->graph_indent) {
		type = get_line_type(data + state->graph_indent);
		len -= state->graph_indent;
	}

	if (type == LINE_COMMIT)
		state->commit_title_read = true;
	else if (state->commit_title_read && len < 1) {
		state->commit_title_read = false;
		state->after_commit_header = true;
	} else if ((state->after_commit_header && len < 1) || type == LINE_DIFF_START) {
		state->after_commit_header = false;
		state->reading_diff_stat = true;
	} else if (state->reading_diff_stat) {
		line = diff_common_add_diff_stat(view, data, state->graph_indent);
		if (line) {
			if (state->graph_indent)
				line->graph_indent = 1;
			return true;
		}
		state->reading_diff_stat = false;
	}

	if (!pager_common_read(view, data, type, &line))
		return false;
	if (line && state->graph_indent)
		line->graph_indent = 1;
	return true;
}

static struct view_ops log_ops = {
	"line",
	argv_env.head,
	VIEW_ADD_PAGER_REFS | VIEW_OPEN_DIFF | VIEW_SEND_CHILD_ENTER | VIEW_LOG_LIKE | VIEW_REFRESH | VIEW_FLEX_WIDTH,
	sizeof(struct log_state),
	log_open,
	log_read,
	view_column_draw,
	log_request,
	view_column_grep,
	log_select,
	NULL,
	view_column_bit(LINE_NUMBER) | view_column_bit(TEXT),
	pager_get_column_data,
};

DEFINE_VIEW(log);

/* vim: set ts=8 sw=8 noexpandtab: */
