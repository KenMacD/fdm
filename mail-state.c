/* $Id$ */

/*
 * Copyright (c) 2006 Nicholas Marriott <nicm@users.sourceforge.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <fnmatch.h>
#include <string.h>
#include <unistd.h>

#include "fdm.h"
#include "fetch.h"
#include "match.h"

struct users   *find_delivery_users(struct mail_ctx *, struct action *, int *);
int		fill_delivery_queue(struct mail_ctx *, struct rule *);

int		start_action(struct mail_ctx *, struct deliver_ctx *);
int		finish_action(struct deliver_ctx *, struct msg *,
		    struct msgbuf *);

#define ACTION_DONE 0
#define ACTION_ERROR 1
#define ACTION_PARENT 2

int
mail_match(struct mail_ctx *mctx, struct msg *msg, struct msgbuf *msgbuf)
{
	struct account	*a = mctx->account;
	struct mail	*m = mctx->mail;
	struct strings	*aa;
	struct expritem	*ei;
	u_int		 i;
	int		 error = MAIL_CONTINUE;
	char		*an, *tkey, *tvalue;

	set_wrapped(m, ' ');

	/*
	 * If blocked, check for msgs from parent.
	 */
	if (mctx->msgid != 0) {
		if (msg == NULL || msg->id != mctx->msgid)
			return (MAIL_BLOCKED);
		mctx->msgid = 0;

		if (msg->type != MSG_DONE)
			fatalx("child: unexpected message");
		if (msgbuf->buf != NULL && msgbuf->len != 0) {
			strb_destroy(&m->tags);
			m->tags = msgbuf->buf;
			update_tags(&m->tags);
		}

		ei = mctx->expritem;
		switch (msg->data.error) {
		case MATCH_ERROR:
			return (MAIL_ERROR);
		case MATCH_TRUE:
			if (!ei->inverted) {
				if (ei->op == OP_NONE || ei->op == OP_OR)
					mctx->result = 1;
			} else {
				if (ei->op == OP_AND)
					mctx->result = 0;
			}
			break;
		case MATCH_FALSE:
			if (!ei->inverted) {
				if (ei->op == OP_AND)
					mctx->result = 0;
			} else {
				if (ei->op == OP_NONE || ei->op == OP_OR)
					mctx->result = 1;
			}
			break;
		default:
			fatalx("child: unexpected response");
		}

		goto next_expritem;
	}

	/*
	 * Check for completion and end of ruleset.
	 */
	if (mctx->done)
		return (MAIL_DONE);
	if (mctx->rule == NULL) {
		switch (conf.impl_act) {
		case DECISION_NONE:
			log_warnx("%s: reached end of ruleset. no "
			    "unmatched-mail option; keeping mail",  a->name);
			m->decision = DECISION_KEEP;
			break;
		case DECISION_KEEP:
			log_debug2("%s: reached end of ruleset. keeping mail",
			    a->name);
			m->decision = DECISION_KEEP;
			break;
		case DECISION_DROP:
			log_debug2("%s: reached end of ruleset. dropping mail",
			    a->name);
			m->decision = DECISION_DROP;
			break;
		}
		return (MAIL_DONE);
	}

	/*
	 * Expression not started. Start it.
	 */
	if (mctx->expritem == NULL) {
		/*
		 * Check rule account list.
		 */
		aa = mctx->rule->accounts;
		if (aa != NULL && !ARRAY_EMPTY(aa)) {
			for (i = 0; i < ARRAY_LENGTH(aa); i++) {
				an = ARRAY_ITEM(aa, i, char *);
				if (name_match(an, a->name))
					break;
			}
			if (i == ARRAY_LENGTH(aa)) {
				mctx->result = 0;
				goto skip;
			}
		}

		/*
		 * No expression. Must be an "all" rule, treat it as always
		 * true.
		 */
		if (mctx->rule->expr == NULL || TAILQ_EMPTY(mctx->rule->expr)) {
			mctx->result = 1;
			goto skip;
		}

		/*
		 * Start the expression.
		 */
		mctx->result = 0;
		mctx->expritem = TAILQ_FIRST(mctx->rule->expr);
	}

	/*
	 * Check this expression item and adjust the result.
	 */
	ei = mctx->expritem;
	switch (ei->match->match(mctx, ei)) {
	case MATCH_ERROR:
		return (MAIL_ERROR);
	case MATCH_PARENT:
		return (MAIL_BLOCKED);
	case MATCH_TRUE:
		if (!ei->inverted) {
			if (ei->op == OP_NONE || ei->op == OP_OR)
				mctx->result = 1;
		} else {
			if (ei->op == OP_AND)
				mctx->result = 0;
		}
		break;
	case MATCH_FALSE:
		if (!ei->inverted) {
			if (ei->op == OP_AND)
				mctx->result = 0;
		} else {
			if (ei->op == OP_NONE || ei->op == OP_OR)
				mctx->result = 1;
		}
		break;
	}

next_expritem:
	/*
	 * Move to the next item. If there is one, then return.
	 */
	mctx->expritem = TAILQ_NEXT(mctx->expritem, entry);
	if (mctx->expritem != NULL)
		return (MAIL_CONTINUE);

skip:
	/*
	 * If the result was false, skip to find the next rule.
	 */
	if (!mctx->result)
		goto next_rule;
	mctx->matched = 1;
	log_debug2("%s: matched to rule %u", a->name, mctx->rule->idx);

	/*
	 * If this rule is stop, mark the context so when we get back after
	 * delivery we know to stop.
	 */
	if (mctx->rule->stop)
		mctx->done = 1;

	/*
	 * Handle nested rules.
	 */
	if (!TAILQ_EMPTY(&mctx->rule->rules)) {
		log_debug2("%s: entering nested rules", a->name);

		/*
		 * Stack the current rule (we are at the end of it so the
		 * the expritem must be NULL already).
		 */
		ARRAY_ADD(&mctx->stack, mctx->rule, struct rule *);

		/*
		 * Continue with the first rule of the nested list.
		 */
		mctx->rule = TAILQ_FIRST(&mctx->rule->rules);
		return (MAIL_CONTINUE);
	}

	/*
	 * Tag mail if necessary.
	 */
	if (mctx->rule->key.str != NULL) {
		tkey = replacestr(&mctx->rule->key, m->tags, m, &m->rml);
		tvalue = replacestr(&mctx->rule->value, m->tags, m, &m->rml);

		if (tkey != NULL && *tkey != '\0' && tvalue != NULL) {
			log_debug2("%s: tagging message: %s (%s)", a->name,
			    tkey, tvalue);
			add_tag(&m->tags, tkey, "%s", tvalue);
		}

		if (tkey != NULL)
			xfree(tkey);
		if (tvalue != NULL)
			xfree(tvalue);
	}

	/*
	 * Fill the delivery action queue.
	 */
	if (!ARRAY_EMPTY(mctx->rule->actions)) {
		if (fill_delivery_queue(mctx, mctx->rule) != 0)
			return (MAIL_ERROR);
		error = MAIL_DELIVER;
	}

next_rule:
	/*
	 * Move to the next rule.
	 */
	mctx->rule = TAILQ_NEXT(mctx->rule, entry);

	/*
	 * If no more rules, try to move up the stack.
	 */
	while (mctx->rule == NULL) {
		if (ARRAY_EMPTY(&mctx->stack))
			break;
		mctx->rule = ARRAY_LAST(&mctx->stack, struct rule *);
		mctx->rule = TAILQ_NEXT(mctx->rule, entry);
		ARRAY_TRUNC(&mctx->stack, 1, struct rule *);
	}

	return (error);
}

int
mail_deliver(struct mail_ctx *mctx, struct msg *msg, struct msgbuf *msgbuf)
{
	struct account		*a = mctx->account;
	struct mail		*m = mctx->mail;
	struct deliver_ctx	*dctx;

	set_wrapped(m, '\n');

	/*
	 * If blocked, check for msgs from parent.
	 */
	if (mctx->msgid != 0) {
		if (msg == NULL || msg->id != mctx->msgid)
			return (MAIL_BLOCKED);
		mctx->msgid = 0;

		/*
		 * Got message. Finish delivery.
		 */
		dctx = TAILQ_FIRST(&mctx->dqueue);
		if (finish_action(dctx, msg, msgbuf) == ACTION_ERROR)
			return (MAIL_ERROR);

		/*
		 * Move on to dequeue this delivery action.
		 */
		goto done;
	}

	/*
	 * Check if delivery is complete.
	 */
	if (TAILQ_EMPTY(&mctx->dqueue))
		return (MAIL_MATCH);

	/*
	 * Get the first delivery action and start it.
	 */
	dctx = TAILQ_FIRST(&mctx->dqueue);
	switch (start_action(mctx, dctx)) {
	case ACTION_ERROR:
		return (MAIL_ERROR);
	case ACTION_PARENT:
		return (MAIL_BLOCKED);
	}

done:
	/*
	 * Remove completed action from queue.
	 */
	TAILQ_REMOVE(&mctx->dqueue, dctx, entry);
	log_debug("%s: message %u delivered (rule %u, %s) in %.3f seconds",
	    a->name, m->idx, dctx->rule->idx,
	    dctx->action->deliver->name, get_time() - dctx->tim);
	xfree(dctx);
	return (MAIL_CONTINUE);
}

struct users *
find_delivery_users(struct mail_ctx *mctx, struct action *t, int *should_free)
{
	struct account	*a = mctx->account;
	struct mail	*m = mctx->mail;
	struct rule	*r = mctx->rule;
	struct users	*users;

	*should_free = 0;
	users = NULL;
	if (r->find_uid) {		/* rule comes first */
		*should_free = 1;
		users = find_users(m);
	} else if (r->users != NULL) {
		*should_free = 0;
		users = r->users;
	} else if (t->find_uid) {	/* then action */
		*should_free = 1;
		users = find_users(m);
	} else if (t->users != NULL) {
		*should_free = 0;
		users = t->users;
	} else if (a->find_uid) {	/* then account */
		*should_free = 1;
		users = find_users(m);
	} else if (a->users != NULL) {
		*should_free = 0;
		users = a->users;
	}
	if (users == NULL) {
		*should_free = 1;
		users = xmalloc(sizeof *users);
		ARRAY_INIT(users);
		ARRAY_ADD(users, conf.def_user, uid_t);
	}

	return (users);
}

int
fill_delivery_queue(struct mail_ctx *mctx, struct rule *r)
{
	struct account		*a = mctx->account;
	struct mail		*m = mctx->mail;
	struct action		*t;
	struct actions		*ta;
	u_int		 	 i, j, k;
	char			*s;
	struct replstr		*rs;
	struct deliver_ctx	*dctx;
	struct users		*users;
	int			 should_free;

	for (i = 0; i < ARRAY_LENGTH(r->actions); i++) {
		rs = &ARRAY_ITEM(r->actions, i, struct replstr);
		s = replacestr(rs, m->tags, m, &m->rml);

		log_debug2("%s: looking for actions matching: %s", a->name, s);
		ta = match_actions(s);
		if (ARRAY_EMPTY(ta))
			goto empty;
		xfree(s);

		log_debug2("%s: found %u actions", a->name, ARRAY_LENGTH(ta));
		for (j = 0; j < ARRAY_LENGTH(ta); j++) {
			t = ARRAY_ITEM(ta, j, struct action *);
			users = find_delivery_users(mctx, t, &should_free);

			for (k = 0; k < ARRAY_LENGTH(users); k++) {
				dctx = xcalloc(1, sizeof *dctx);
				dctx->action = t;
				dctx->account = a;
				dctx->rule = r;
				dctx->mail = m;
				dctx->uid = ARRAY_ITEM(users, k, uid_t);

				log_debug3("%s: action %s, uid %lu", a->name,
				    t->name, (u_long) dctx->uid);
				TAILQ_INSERT_TAIL(&mctx->dqueue, dctx, entry);
			}

			if (should_free)
				ARRAY_FREEALL(users);
		}

		ARRAY_FREEALL(ta);
	}

	return (0);

empty:
	xfree(s);
	ARRAY_FREEALL(ta);
	log_warnx("%s: no actions matching: %s (%s)", a->name, s, rs->str);
	return (1);

}

int
start_action(struct mail_ctx *mctx, struct deliver_ctx *dctx)
{
	struct account	*a = dctx->account;
	struct action	*t = dctx->action;
	struct mail	*m = dctx->mail;
#if 0
	struct mail	*md = &dctx->wr_mail;
#endif
	struct msg	 msg;
	struct msgbuf	 msgbuf;
#if 0
	u_int		 lines;
#endif

	dctx->tim = get_time();
 	if (t->deliver->deliver == NULL)
		return (0);

	log_debug2("%s: message %u, running action %s as user %lu",
	    a->name, m->idx, t->name, (u_long) dctx->uid);
	add_tag(&m->tags, "action", "%s", t->name);

	/* just deliver now for in-child delivery */
	if (t->deliver->type == DELIVER_INCHILD) {
		if (t->deliver->deliver(dctx, t) != DELIVER_SUCCESS)
			return (ACTION_ERROR);
		return (ACTION_DONE);
	}

#if 0
	/* if the current user is the same as the deliver user, don't bother
	   passing up either */
	if (t->deliver->type == DELIVER_ASUSER && dctx->uid == geteuid()) {
		if (t->deliver->deliver(dctx, t) != DELIVER_SUCCESS)
			return (ACTION_ERROR);
		return (ACTION_DONE);
	}
	if (t->deliver->type == DELIVER_WRBACK && dctx->uid == geteuid()) {
		if (mail_open(md, IO_BLOCKSIZE) != 0) {
			log_warn("%s: failed to create mail", a->name);
			return (ACTION_ERROR);
		}
		md->decision = m->decision;

		if (t->deliver->deliver(dctx, t) != DELIVER_SUCCESS) {
			mail_destroy(md);
			return (ACTION_ERROR);
		}

		memcpy(&msg.data.mail, md, sizeof msg.data.mail);
		cleanup_deregister(md->shm.name);
		strb_destroy(&md->tags);

		if (mail_receive(m, msg, 0) != 0) {
			log_warn("%s: can't receive mail", a->name);
			return (ACTION_ERROR);
		}
		log_debug2("%s: received modified mail: size %zu, body %zd",
		    a->name, m->size, m->body);

		/* trim from line */
		trim_from(m);

		/* and recreate the wrapped array */
		lines = fill_wrapped(m);
		log_debug2("%s: found %u wrapped lines", a->name, lines);

		return (ACTION_DONE);
	}
#endif

	memset(&msg, 0, sizeof msg);
	msg.type = MSG_ACTION;
	msg.id = m->idx;

	msg.data.account = a;
	msg.data.action = t;
	msg.data.uid = dctx->uid;

	msgbuf.buf = m->tags;
	msgbuf.len = STRB_SIZE(m->tags);

	mail_send(m, &msg);

	log_debug3("%s: sending action to parent", a->name);
	if (privsep_send(mctx->io, &msg, &msgbuf) != 0)
		fatalx("child: privsep_send error");

	mctx->msgid = msg.id;
	return (ACTION_PARENT);
}

int
finish_action(struct deliver_ctx *dctx, struct msg *msg, struct msgbuf *msgbuf)
{
	struct account	*a = dctx->account;
	struct action	*t = dctx->action;
	struct mail	*m = dctx->mail;
	u_int		 lines;

	if (msgbuf->buf != NULL && msgbuf->len != 0) {
		strb_destroy(&m->tags);
		m->tags = msgbuf->buf;
		update_tags(&m->tags);
	}

	if (msg->data.error != 0)
		return (ACTION_ERROR);

	if (t->deliver->type != DELIVER_WRBACK)
		return (ACTION_DONE);

	if (mail_receive(m, msg, 1) != 0) {
		log_warn("%s: can't receive mail", a->name);
		return (ACTION_ERROR);
	}
	log_debug2("%s: message %u, received modified mail: size %zu, body %zd",
	    a->name, m->idx, m->size, m->body);

	/* trim from line */
	trim_from(m);

	/* and recreate the wrapped array */
	lines = fill_wrapped(m);
	log_debug2("%s: found %u wrapped lines", a->name, lines);

	return (ACTION_DONE);
}