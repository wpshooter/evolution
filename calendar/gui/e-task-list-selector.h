/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* e-task-list-selector.h
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/* XXX This widget is nearly identical to EMemoListSelector.  If
 *     ECalendarSelector ever learns how to move selections from
 *     one source to another, perhaps these ESourceSelector sub-
 *     classes could someday be combined. */

#ifndef E_TASK_LIST_SELECTOR_H
#define E_TASK_LIST_SELECTOR_H

#include <e-util/e-util.h>
#include <shell/e-shell-view.h>

/* Standard GObject macros */
#define E_TYPE_TASK_LIST_SELECTOR \
	(e_task_list_selector_get_type ())
#define E_TASK_LIST_SELECTOR(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_TASK_LIST_SELECTOR, ETaskListSelector))
#define E_TASK_LIST_SELECTOR_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_TASK_LIST_SELECTOR, ETaskListSelectorClass))
#define E_IS_TASK_LIST_SELECTOR(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_TASK_LIST_SELECTOR))
#define E_IS_TASK_LIST_SELECTOR_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_TASK_LIST_SELECTOR))
#define E_TASK_LIST_SELECTOR_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_TASK_LIST_SELECTOR, ETaskListSelectorClass))

G_BEGIN_DECLS

typedef struct _ETaskListSelector ETaskListSelector;
typedef struct _ETaskListSelectorClass ETaskListSelectorClass;
typedef struct _ETaskListSelectorPrivate ETaskListSelectorPrivate;

struct _ETaskListSelector {
	EClientSelector parent;
	ETaskListSelectorPrivate *priv;
};

struct _ETaskListSelectorClass {
	EClientSelectorClass parent_class;
};

GType		e_task_list_selector_get_type	(void);
GtkWidget *	e_task_list_selector_new	(EClientCache *client_cache,
						 EShellView *shell_view);
EShellView *	e_task_list_selector_get_shell_view
						(ETaskListSelector *selector);

G_END_DECLS

#endif /* E_TASK_LIST_SELECTOR_H */
