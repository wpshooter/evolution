/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * E-table.c: A graphical view of a Table.
 *
 * Author:
 *   Miguel de Icaza (miguel@helixcode.com)
 *   Chris Lahey (clahey@helixcode.com)
 *
 * Copyright 1999, Helix Code, Inc
 */
#include <config.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif
#include <stdio.h>
#include <libgnomeui/gnome-canvas.h>
#include <gtk/gtksignal.h>

#include "gal/util/e-util.h"
#include "gal/widgets/e-canvas.h"
#include "gal/widgets/e-canvas-vbox.h"
#include "e-table.h"
#include "e-table-header-item.h"
#include "e-table-subset.h"
#include "e-table-item.h"
#include "e-table-group.h"
#include "e-table-group-leaf.h"
#include "e-table-click-to-add.h"
#include "e-table-specification.h"
#include "e-table-state.h"
#include "e-table-column-specification.h"

#define COLUMN_HEADER_HEIGHT 16

#define PARENT_TYPE gtk_table_get_type ()

static GtkObjectClass *e_table_parent_class;

enum {
	CURSOR_CHANGE,
	DOUBLE_CLICK,
	RIGHT_CLICK,
	CLICK,
	KEY_PRESS,

	TABLE_DRAG_BEGIN,
	TABLE_DRAG_END,
	TABLE_DRAG_DATA_GET,
	TABLE_DRAG_DATA_DELETE,

	TABLE_DRAG_LEAVE,
	TABLE_DRAG_MOTION,
	TABLE_DRAG_DROP,
	TABLE_DRAG_DATA_RECEIVED,

	LAST_SIGNAL
};

enum {
	ARG_0,
	ARG_TABLE_DRAW_FOCUS,
	ARG_LENGTH_THRESHOLD,
};

static gint et_signals [LAST_SIGNAL] = { 0, };

static void e_table_fill_table (ETable *e_table, ETableModel *model);
static gboolean changed_idle (gpointer data);

static void et_drag_begin (GtkWidget *widget,
			   GdkDragContext *context,
			   ETable *et);
static void et_drag_end (GtkWidget *widget,
			 GdkDragContext *context,
			 ETable *et);
static void et_drag_data_get(GtkWidget *widget,
			     GdkDragContext *context,
			     GtkSelectionData *selection_data,
			     guint info,
			     guint time,
			     ETable *et);
static void et_drag_data_delete(GtkWidget *widget,
				GdkDragContext *context,
				ETable *et);

static void et_drag_leave(GtkWidget *widget,
			  GdkDragContext *context,
			  guint time,
			  ETable *et);
static gboolean et_drag_motion(GtkWidget *widget,
			       GdkDragContext *context,
			       gint x,
			       gint y,
			       guint time,
			       ETable *et);
static gboolean et_drag_drop(GtkWidget *widget,
			     GdkDragContext *context,
			     gint x,
			     gint y,
			     guint time,
			     ETable *et);
static void et_drag_data_received(GtkWidget *widget,
				  GdkDragContext *context,
				  gint x,
				  gint y,
				  GtkSelectionData *selection_data,
				  guint info,
				  guint time,
				  ETable *et);
static gint e_table_drag_source_event_cb (GtkWidget      *widget,
					  GdkEvent       *event,
					  ETable         *table);

static void
et_destroy (GtkObject *object)
{
	ETable *et = E_TABLE (object);
	

	gtk_signal_disconnect (GTK_OBJECT (et->model),
			       et->table_model_change_id);
	gtk_signal_disconnect (GTK_OBJECT (et->model),
			       et->table_row_change_id);
	gtk_signal_disconnect (GTK_OBJECT (et->model),
			       et->table_cell_change_id);
	gtk_signal_disconnect (GTK_OBJECT (et->model),
			       et->table_row_inserted_id);
	gtk_signal_disconnect (GTK_OBJECT (et->model),
			       et->table_row_deleted_id);
	if (et->group_info_change_id)
		gtk_signal_disconnect (GTK_OBJECT (et->sort_info),
				       et->group_info_change_id);

	if (et->reflow_idle_id)
		g_source_remove(et->reflow_idle_id);
	et->reflow_idle_id = 0;

	gtk_object_unref (GTK_OBJECT (et->model));
	gtk_object_unref (GTK_OBJECT (et->full_header));
	gtk_object_unref (GTK_OBJECT (et->header));
	gtk_object_unref (GTK_OBJECT (et->sort_info));
	gtk_object_unref (GTK_OBJECT (et->sorter));
	gtk_object_unref (GTK_OBJECT (et->selection));

	if (et->header_canvas != NULL)
		gtk_widget_destroy (GTK_WIDGET (et->header_canvas));

	gtk_widget_destroy (GTK_WIDGET (et->table_canvas));

	if (et->rebuild_idle_id) {
		g_source_remove (et->rebuild_idle_id);
		et->rebuild_idle_id = 0;
	}
	
	g_free(et->click_to_add_message);
	
	(*e_table_parent_class->destroy)(object);
}

static void
e_table_init (GtkObject *object)
{
	ETable *e_table = E_TABLE (object);
	GtkTable *gtk_table = GTK_TABLE (object);

	gtk_table->homogeneous = FALSE;
	
	e_table->sort_info = NULL;
	e_table->group_info_change_id = 0;
	e_table->reflow_idle_id = 0;

	e_table->draw_grid = 1;
	e_table->draw_focus = 1;
	e_table->cursor_mode = E_TABLE_CURSOR_SIMPLE;
	e_table->length_threshold = 200;
	
	e_table->need_rebuild = 0;
	e_table->rebuild_idle_id = 0;
	
	e_table->click_to_add_message = NULL;

	e_table->drag_get_data_row = -1;
	e_table->drag_get_data_col = -1;
	e_table->drop_row = -1;
	e_table->drop_col = -1;
	e_table->site = NULL;
	e_table->drag_source_button_press_event_id = 0;
	e_table->drag_source_motion_notify_event_id = 0;

	e_table->sorter = NULL;
	e_table->selection = e_table_selection_model_new();
	e_table->cursor_loc = E_TABLE_CURSOR_LOC_NONE;
}

static void
header_canvas_size_allocate (GtkWidget *widget, GtkAllocation *alloc, ETable *e_table)
{
	gnome_canvas_set_scroll_region (
		GNOME_CANVAS (e_table->header_canvas),
		0, 0, alloc->width - 1, /*  COLUMN_HEADER_HEIGHT - 1 */
		E_TABLE_HEADER_ITEM (e_table->header_item)->height - 1);

	/* When the header item is created ->height == 0,
	   as the font is only created when everything is realized.
	   So we set the usize here as well, so that the size of the 
	   header is correct */
	if (GTK_WIDGET (e_table->header_canvas)->allocation.height !=
	    E_TABLE_HEADER_ITEM (e_table->header_item)->height)
		gtk_widget_set_usize (GTK_WIDGET (e_table->header_canvas), -1, 
				      E_TABLE_HEADER_ITEM (e_table->header_item)->height);
}

static void
sort_info_changed (ETableSortInfo *info, ETable *et)
{
	et->need_rebuild = TRUE;
	if (!et->rebuild_idle_id)
		et->rebuild_idle_id = g_idle_add_full (20, changed_idle, et, NULL);
}

static void
e_table_setup_header (ETable *e_table)
{
	e_table->header_canvas = GNOME_CANVAS (e_canvas_new ());
	
	gtk_widget_show (GTK_WIDGET (e_table->header_canvas));

	e_table->header_item = gnome_canvas_item_new (
		gnome_canvas_root (e_table->header_canvas),
		e_table_header_item_get_type (),
		"ETableHeader", e_table->header,
		"full_header", e_table->full_header,
		"sort_info", e_table->sort_info,
		"dnd_code", "(unset)",
		NULL);

	gtk_signal_connect (
		GTK_OBJECT (e_table->header_canvas), "size_allocate",
		GTK_SIGNAL_FUNC (header_canvas_size_allocate), e_table);

	gtk_widget_set_usize (GTK_WIDGET (e_table->header_canvas), -1, 
			      E_TABLE_HEADER_ITEM (e_table->header_item)->height);
}

static gboolean
table_canvas_reflow_idle (ETable *e_table)
{
	gdouble height, width;
	GtkAllocation *alloc = &(GTK_WIDGET (e_table->table_canvas)->allocation);

	gtk_object_get (GTK_OBJECT (e_table->canvas_vbox),
			"height", &height,
			"width", &width,
			NULL);
	height = MAX ((int)height, alloc->height);
	width = MAX((int)width, alloc->width);
	/* I have no idea why this needs to be -1, but it works. */
	gnome_canvas_set_scroll_region (
		GNOME_CANVAS (e_table->table_canvas),
		0, 0, width - 1, height - 1);
	e_table->reflow_idle_id = 0;
	return FALSE;
}

static void
table_canvas_size_allocate (GtkWidget *widget, GtkAllocation *alloc,
			    ETable *e_table)
{
	gdouble width;
	width = alloc->width;

	gtk_object_set (GTK_OBJECT (e_table->canvas_vbox),
			"width", width,
			NULL);
	gtk_object_set (GTK_OBJECT (e_table->header),
			"width", width,
			NULL);
	if (e_table->reflow_idle_id)
		g_source_remove(e_table->reflow_idle_id);
	table_canvas_reflow_idle(e_table);
}

static void
table_canvas_reflow (GnomeCanvas *canvas, ETable *e_table)
{
	if (!e_table->reflow_idle_id)
		e_table->reflow_idle_id = g_idle_add_full (400, (GSourceFunc) table_canvas_reflow_idle, e_table, NULL);
}

static void
click_to_add_cursor_change (ETableClickToAdd *etcta, int row, int col, ETable *et)
{
	if (et->cursor_loc == E_TABLE_CURSOR_LOC_TABLE) {
		e_table_selection_model_clear(et->selection);
	}
	et->cursor_loc = E_TABLE_CURSOR_LOC_ETCTA;
}

static void
group_cursor_change (ETableGroup *etg, int row, ETable *et)
{
	if (et->cursor_loc == E_TABLE_CURSOR_LOC_ETCTA && et->click_to_add) {
		e_table_click_to_add_commit(E_TABLE_CLICK_TO_ADD(et->click_to_add));
	}
	et->cursor_loc = E_TABLE_CURSOR_LOC_TABLE;
	gtk_signal_emit (GTK_OBJECT (et),
			 et_signals [CURSOR_CHANGE],
			 row);
}

static void
group_double_click (ETableGroup *etg, int row, ETable *et)
{
	gtk_signal_emit (GTK_OBJECT (et),
			 et_signals [DOUBLE_CLICK],
			 row);
}

static gint
group_right_click (ETableGroup *etg, int row, int col, GdkEvent *event, ETable *et)
{
	int return_val = 0;
	gtk_signal_emit (GTK_OBJECT (et),
			 et_signals [RIGHT_CLICK],
			 row, col, event, &return_val);
	return return_val;
}

static gint
group_click (ETableGroup *etg, int row, int col, GdkEvent *event, ETable *et)
{
	int return_val = 0;
	gtk_signal_emit (GTK_OBJECT (et),
			 et_signals [CLICK],
			 row, col, event, &return_val);
	return return_val;
}

static gint
group_key_press (ETableGroup *etg, int row, int col, GdkEvent *event, ETable *et)
{
	int return_val = 0;
	GdkEventKey *key = (GdkEventKey *) event;
	GdkEventButton click;
	switch (key->keyval) {
	case GDK_Page_Down:
		gtk_adjustment_set_value(gtk_layout_get_vadjustment (GTK_LAYOUT (et->table_canvas)),
					 CLAMP(gtk_layout_get_vadjustment (GTK_LAYOUT (et->table_canvas))->value +
					       (gtk_layout_get_vadjustment (GTK_LAYOUT (et->table_canvas))->page_size - 20),
					       0, 
					       gtk_layout_get_vadjustment (GTK_LAYOUT (et->table_canvas))->upper - 
					       gtk_layout_get_vadjustment (GTK_LAYOUT (et->table_canvas))->page_size));
		click.type = GDK_BUTTON_PRESS;
		click.window = GTK_LAYOUT (et->table_canvas)->bin_window;
		click.send_event = key->send_event;
		click.time = key->time;
		click.x = 30;
		click.y = gtk_layout_get_vadjustment (GTK_LAYOUT (et->table_canvas))->page_size - 1;
		click.state = key->state;
		click.button = 1;
		gtk_widget_event(GTK_WIDGET(et->table_canvas),
				 (GdkEvent *) &click);
		return_val = 1;
		break;
	case GDK_Page_Up:
		gtk_adjustment_set_value(gtk_layout_get_vadjustment (GTK_LAYOUT (et->table_canvas)),
					 gtk_layout_get_vadjustment (GTK_LAYOUT (et->table_canvas))->value -
					 (gtk_layout_get_vadjustment (GTK_LAYOUT (et->table_canvas))->page_size - 20));
		click.type = GDK_BUTTON_PRESS;
		click.window = GTK_LAYOUT (et->table_canvas)->bin_window;
		click.send_event = key->send_event;
		click.time = key->time;
		click.x = 30;
		click.y = 1;
		click.state = key->state;
		click.button = 1;
		gtk_widget_event(GTK_WIDGET(et->table_canvas),
				 (GdkEvent *) &click);
		return_val = 1;
		break;
	default:
		gtk_signal_emit (GTK_OBJECT (et),
				 et_signals [KEY_PRESS],
				 row, col, event, &return_val);
		break;
	}
	return return_val;
}

static gboolean
changed_idle (gpointer data)
{
	ETable *et = E_TABLE (data);

	if (et->need_rebuild) {
		gtk_object_destroy (GTK_OBJECT (et->group));
		et->group = e_table_group_new (GNOME_CANVAS_GROUP (et->canvas_vbox),
					       et->full_header,
					       et->header,
					       et->model,
					       et->sort_info,
					       0);
		e_canvas_vbox_add_item(E_CANVAS_VBOX(et->canvas_vbox), GNOME_CANVAS_ITEM(et->group));
		gnome_canvas_item_set(GNOME_CANVAS_ITEM(et->group),
				      "drawgrid", et->draw_grid,
				      "drawfocus", et->draw_focus,
				      "cursor_mode", et->cursor_mode,
				      "length_threshold", et->length_threshold,
				      "table_selection_model", et->selection,
				      NULL);
		gtk_signal_connect (GTK_OBJECT (et->group), "cursor_change",
				    GTK_SIGNAL_FUNC (group_cursor_change), et);
		gtk_signal_connect (GTK_OBJECT (et->group), "double_click",
				    GTK_SIGNAL_FUNC (group_double_click), et);
		gtk_signal_connect (GTK_OBJECT (et->group), "right_click",
				    GTK_SIGNAL_FUNC (group_right_click), et);
		gtk_signal_connect (GTK_OBJECT (et->group), "click",
				    GTK_SIGNAL_FUNC (group_click), et);
		gtk_signal_connect (GTK_OBJECT (et->group), "key_press",
				    GTK_SIGNAL_FUNC (group_key_press), et);
		e_table_fill_table (et, et->model);
		
		gtk_object_set (GTK_OBJECT (et->canvas_vbox),
				"width", (double) GTK_WIDGET (et->table_canvas)->allocation.width,
				NULL);
	}

	et->need_rebuild = 0;
	et->rebuild_idle_id = 0;

	return FALSE;
}

static void
et_table_model_changed (ETableModel *model, ETable *et)
{
	et->need_rebuild = TRUE;
	if (!et->rebuild_idle_id)
		et->rebuild_idle_id = g_idle_add_full (20, changed_idle, et, NULL);
}

static void
et_table_row_changed (ETableModel *table_model, int row, ETable *et)
{
	if (!et->need_rebuild) {
		if (e_table_group_remove (et->group, row))
			e_table_group_add (et->group, row);
	}
}

static void
et_table_cell_changed (ETableModel *table_model, int view_col, int row, ETable *et)
{
	et_table_row_changed (table_model, row, et);
}

static void
et_table_row_inserted (ETableModel *table_model, int row, ETable *et)
{
	/* This number has already been decremented. */
	int row_count = e_table_model_row_count(table_model);
	if (!et->need_rebuild) {
		if (row != row_count - 1)
			e_table_group_increment(et->group, row, 1);
		e_table_group_add (et->group, row);
	}
}

static void
et_table_row_deleted (ETableModel *table_model, int row, ETable *et)
{
	int row_count = e_table_model_row_count(table_model);
	if (!et->need_rebuild) {
		e_table_group_remove (et->group, row);
		if (row != row_count)
			e_table_group_decrement(et->group, row, 1);
	}
}

static void
e_table_setup_table (ETable *e_table, ETableHeader *full_header, ETableHeader *header,
		     ETableModel *model)
{
	e_table->table_canvas = GNOME_CANVAS (e_canvas_new ());
	gtk_signal_connect (
		GTK_OBJECT (e_table->table_canvas), "size_allocate",
		GTK_SIGNAL_FUNC (table_canvas_size_allocate), e_table);

	gtk_signal_connect (
		GTK_OBJECT (e_table), "drag_begin",
		GTK_SIGNAL_FUNC (et_drag_begin), e_table);
	gtk_signal_connect (
		GTK_OBJECT (e_table), "drag_end",
		GTK_SIGNAL_FUNC (et_drag_end), e_table);
	gtk_signal_connect (
		GTK_OBJECT (e_table), "drag_data_get",
		GTK_SIGNAL_FUNC (et_drag_data_get), e_table);
	gtk_signal_connect (
		GTK_OBJECT (e_table), "drag_data_delete",
		GTK_SIGNAL_FUNC (et_drag_data_delete), e_table);
	gtk_signal_connect (
		GTK_OBJECT (e_table), "drag_motion",
		GTK_SIGNAL_FUNC (et_drag_motion), e_table);
	gtk_signal_connect (
		GTK_OBJECT (e_table), "drag_leave",
		GTK_SIGNAL_FUNC (et_drag_leave), e_table);
	gtk_signal_connect (
		GTK_OBJECT (e_table), "drag_drop",
		GTK_SIGNAL_FUNC (et_drag_drop), e_table);
	gtk_signal_connect (
		GTK_OBJECT (e_table), "drag_data_received",
		GTK_SIGNAL_FUNC (et_drag_data_received), e_table);
	
	gtk_signal_connect (GTK_OBJECT(e_table->table_canvas), "reflow",
			    GTK_SIGNAL_FUNC (table_canvas_reflow), e_table);
				 
	gtk_widget_show (GTK_WIDGET (e_table->table_canvas));


	e_table->canvas_vbox = gnome_canvas_item_new(gnome_canvas_root(e_table->table_canvas),
						     e_canvas_vbox_get_type(),
						     "spacing", 10.0,
						     NULL);

	if (e_table->use_click_to_add) {
		e_table->click_to_add = gnome_canvas_item_new (GNOME_CANVAS_GROUP(e_table->canvas_vbox),
							       e_table_click_to_add_get_type (),
							       "header", e_table->header,
							       "model", e_table->model,
							       "message", e_table->click_to_add_message,
							       NULL);
		
		e_canvas_vbox_add_item(E_CANVAS_VBOX(e_table->canvas_vbox), e_table->click_to_add);
		gtk_signal_connect(GTK_OBJECT (e_table->click_to_add), "cursor_change",
				   GTK_SIGNAL_FUNC(click_to_add_cursor_change), e_table);
	}

	e_table->group = e_table_group_new (
		GNOME_CANVAS_GROUP (e_table->canvas_vbox),
		full_header, header,
		model, e_table->sort_info, 0);
	e_canvas_vbox_add_item(E_CANVAS_VBOX(e_table->canvas_vbox), GNOME_CANVAS_ITEM(e_table->group));

	gnome_canvas_item_set(GNOME_CANVAS_ITEM(e_table->group),
			      "drawgrid", e_table->draw_grid,
			      "drawfocus", e_table->draw_focus,
			      "cursor_mode", e_table->cursor_mode,
			      "length_threshold", e_table->length_threshold,
			      "table_selection_model", e_table->selection,
			      NULL);

	gtk_signal_connect (GTK_OBJECT (e_table->group), "cursor_change",
			    GTK_SIGNAL_FUNC(group_cursor_change), e_table);
	gtk_signal_connect (GTK_OBJECT (e_table->group), "double_click",
			    GTK_SIGNAL_FUNC(group_double_click), e_table);
	gtk_signal_connect (GTK_OBJECT (e_table->group), "right_click",
			    GTK_SIGNAL_FUNC(group_right_click), e_table);
	gtk_signal_connect (GTK_OBJECT (e_table->group), "click",
			    GTK_SIGNAL_FUNC(group_click), e_table);
	gtk_signal_connect (GTK_OBJECT (e_table->group), "key_press",
			    GTK_SIGNAL_FUNC(group_key_press), e_table);
	
	e_table->table_model_change_id = gtk_signal_connect (
		GTK_OBJECT (model), "model_changed",
		GTK_SIGNAL_FUNC (et_table_model_changed), e_table);

	e_table->table_row_change_id = gtk_signal_connect (
		GTK_OBJECT (model), "model_row_changed",
		GTK_SIGNAL_FUNC (et_table_row_changed), e_table);

	e_table->table_cell_change_id = gtk_signal_connect (
		GTK_OBJECT (model), "model_cell_changed",
		GTK_SIGNAL_FUNC (et_table_cell_changed), e_table);
	
	e_table->table_row_inserted_id = gtk_signal_connect (
		GTK_OBJECT (model), "model_row_inserted",
		GTK_SIGNAL_FUNC (et_table_row_inserted), e_table);
	
	e_table->table_row_deleted_id = gtk_signal_connect (
		GTK_OBJECT (model), "model_row_deleted",
		GTK_SIGNAL_FUNC (et_table_row_deleted), e_table);

}

static void
e_table_fill_table (ETable *e_table, ETableModel *model)
{
	e_table_group_add_all (e_table->group);
}

static ETableCol *
et_col_spec_to_col (ETable *e_table, ETableColumnSpecification *col_spec, ETableExtras *ete)
{
	ETableCol *col = NULL;
	ECell *cell;
	GCompareFunc compare;

	cell = e_table_extras_get_cell(ete, col_spec->cell);
	compare = e_table_extras_get_compare(ete, col_spec->compare);

	if (cell && compare) {
		if (col_spec->title_ && *col_spec->title_) {
			col = e_table_col_new (col_spec->model_col, col_spec->title_,
					       col_spec->expansion, col_spec->minimum_width,
					       cell, compare, col_spec->resizable);
		} else if (col_spec->pixbuf && *col_spec->pixbuf) {
			GdkPixbuf *pixbuf;

			pixbuf = e_table_extras_get_pixbuf(ete, col_spec->pixbuf);
			if (pixbuf) {
				col = e_table_col_new_with_pixbuf (col_spec->model_col, pixbuf,
								   col_spec->expansion, col_spec->minimum_width,
								   cell, compare, col_spec->resizable);
			}
		}
	}
	return col;
}

static ETableHeader *
et_spec_to_full_header (ETable *e_table, ETableSpecification *spec, ETableExtras *ete)
{
	ETableHeader *nh;
	int column;

	g_return_val_if_fail (e_table, NULL);
	g_return_val_if_fail (spec, NULL);
	g_return_val_if_fail (ete, NULL);
	
	nh = e_table_header_new ();

	for (column = 0; spec->columns[column]; column++) {
		ETableCol *col = et_col_spec_to_col(e_table, spec->columns[column], ete);

		if (col)
			e_table_header_add_column (nh, col, -1);
	}

	return nh;
}

static ETableHeader *
et_state_to_header (ETable *e_table, ETableHeader *full_header, ETableState *state)
{
	ETableHeader *nh;
	const int max_cols = e_table_header_count (full_header);
	int column;

	g_return_val_if_fail (e_table, NULL);
	g_return_val_if_fail (full_header, NULL);
	g_return_val_if_fail (state, NULL);
	
	nh = e_table_header_new ();

	for (column = 0; column < state->col_count; column++) {
		int col;

		col = state->columns[column];

		if (col >= max_cols)
			continue;

		e_table_header_add_column (nh, e_table_header_get_column (full_header, col), -1);
	}

	return nh;
}

static void
et_set_state(ETable *e_table, ETableState *state)
{
	if (e_table->header)
		gtk_object_unref(GTK_OBJECT(e_table->header));
	e_table->header = et_state_to_header (e_table, e_table->full_header, state);
	if (e_table->header)
		gtk_object_ref(GTK_OBJECT(e_table->header));

	if (e_table->sort_info)
		gtk_object_unref(GTK_OBJECT(e_table->sort_info));
	e_table->sort_info = state->sort_info;
	if (e_table->sort_info)
		gtk_object_ref(GTK_OBJECT(e_table->sort_info));

	if (e_table->header_item)
		gtk_object_set(GTK_OBJECT(e_table->header_item),
			       "ETableHeader", e_table->header,
			       "sort_info", e_table->sort_info,
			       NULL);
	if (e_table->click_to_add)
		gtk_object_set(GTK_OBJECT(e_table->click_to_add),
			       "header", e_table->header,
			       NULL);

	e_table->need_rebuild = TRUE;
	if (!e_table->rebuild_idle_id)
		e_table->rebuild_idle_id = g_idle_add_full (20, changed_idle, e_table, NULL);
}

void            e_table_set_state                 (ETable               *e_table,
						   const gchar          *state_str)
{
	ETableState *state;

	g_return_if_fail(e_table != NULL);
	g_return_if_fail(E_IS_TABLE(e_table));
	g_return_if_fail(state_str != NULL);

	state = e_table_state_new();
	e_table_state_load_from_string(state, state_str);

	et_set_state(e_table, state);

	gtk_object_sink(GTK_OBJECT(state));
}

void            e_table_load_state                (ETable               *e_table,
						   const gchar          *filename)
{
	ETableState *state;

	g_return_if_fail(e_table != NULL);
	g_return_if_fail(E_IS_TABLE(e_table));
	g_return_if_fail(filename != NULL);

	state = e_table_state_new();
	e_table_state_load_from_file(state, filename);

	et_set_state(e_table, state);

	gtk_object_sink(GTK_OBJECT(state));
}

gchar          *e_table_get_state                 (ETable               *e_table)
{
	return NULL;
}

void            e_table_save_state                (ETable               *e_table,
						   const gchar          *filename)
{
}


static ETable *
et_real_construct (ETable *e_table, ETableModel *etm, ETableExtras *ete,
		   ETableSpecification *specification, ETableState *state)
{
	int row = 0;

	if (ete)
		gtk_object_ref(GTK_OBJECT(ete));
	else
		ete = e_table_extras_new();

	e_table->use_click_to_add = specification->click_to_add;
	e_table->click_to_add_message = g_strdup(specification->click_to_add_message_);
	e_table->draw_grid = specification->draw_grid;
	e_table->cursor_mode = specification->cursor_mode;
	e_table->full_header = et_spec_to_full_header(e_table, specification, ete);

	e_table->model = etm;
	gtk_object_ref (GTK_OBJECT (etm));

	gtk_widget_push_visual (gdk_rgb_get_visual ());
	gtk_widget_push_colormap (gdk_rgb_get_cmap ());

	e_table->header = et_state_to_header (e_table, e_table->full_header, state);
	e_table->sort_info = state->sort_info;

	e_table->group_info_change_id =
		gtk_signal_connect (GTK_OBJECT (e_table->sort_info), "group_info_changed",
				    GTK_SIGNAL_FUNC (sort_info_changed), e_table);

	
	gtk_object_set(GTK_OBJECT(e_table->header),
		       "sort_info", e_table->sort_info,
		       NULL);

	e_table->sorter = e_table_sorter_new(etm, e_table->full_header, e_table->sort_info);

	gtk_object_set (GTK_OBJECT (e_table->selection),
			"model", etm,
			"sorter", e_table->sorter,
			NULL);

	if (!specification->no_headers) {
		e_table_setup_header (e_table);
	}
	e_table_setup_table (e_table, e_table->full_header, e_table->header, etm);
	e_table_fill_table (e_table, etm);

	gtk_layout_get_vadjustment (GTK_LAYOUT (e_table->table_canvas))->step_increment = 20;
	gtk_adjustment_changed(gtk_layout_get_vadjustment (GTK_LAYOUT (e_table->table_canvas)));

	if (!specification->no_headers) {
		/*
		 * The header
		 */
		gtk_table_attach (GTK_TABLE (e_table), GTK_WIDGET (e_table->header_canvas),
				  0, 1, 0 + row, 1 + row,
				  GTK_FILL | GTK_EXPAND,
				  GTK_FILL, 0, 0);
		row ++;
	}
	gtk_table_attach (GTK_TABLE (e_table), GTK_WIDGET (e_table->table_canvas),
			  0, 1, 0 + row, 1 + row,
			  GTK_FILL | GTK_EXPAND,
			  GTK_FILL | GTK_EXPAND,
			  0, 0);
	
	gtk_widget_pop_colormap ();
	gtk_widget_pop_visual ();

	gtk_object_unref(GTK_OBJECT(ete));

	return e_table;
}

ETable *
e_table_construct (ETable *e_table, ETableModel *etm, ETableExtras *ete,
		   const char *spec_str, const char *state_str)
{
	ETableSpecification *specification;
	ETableState *state;

	g_return_val_if_fail(e_table != NULL, NULL);
	g_return_val_if_fail(E_IS_TABLE(e_table), NULL);
	g_return_val_if_fail(etm != NULL, NULL);
	g_return_val_if_fail(E_IS_TABLE_MODEL(etm), NULL);
	g_return_val_if_fail(ete == NULL || E_IS_TABLE_EXTRAS(ete), NULL);
	g_return_val_if_fail(spec_str != NULL, NULL);

	specification = e_table_specification_new();
	e_table_specification_load_from_string(specification, spec_str);
	if (state_str) {
		state = e_table_state_new();
		e_table_state_load_from_string(state, state_str);
	} else {
		state = specification->state;
		gtk_object_ref(GTK_OBJECT(state));
	}

	e_table = et_real_construct (e_table, etm, ete, specification, state);

	gtk_object_unref(GTK_OBJECT(specification));
	gtk_object_unref(GTK_OBJECT(state));

	return e_table;
}

ETable *
e_table_construct_from_spec_file (ETable *e_table, ETableModel *etm, ETableExtras *ete,
				  const char *spec_fn, const char *state_fn)
{
	ETableSpecification *specification;
	ETableState *state;

	g_return_val_if_fail(e_table != NULL, NULL);
	g_return_val_if_fail(E_IS_TABLE(e_table), NULL);
	g_return_val_if_fail(etm != NULL, NULL);
	g_return_val_if_fail(E_IS_TABLE_MODEL(etm), NULL);
	g_return_val_if_fail(ete == NULL || E_IS_TABLE_EXTRAS(ete), NULL);
	g_return_val_if_fail(spec_fn != NULL, NULL);

	specification = e_table_specification_new();
	if (!e_table_specification_load_from_file(specification, spec_fn)) {
		gtk_object_unref(GTK_OBJECT(specification));
		return NULL;
	}

	if (state_fn) {
		state = e_table_state_new();
		if (!e_table_state_load_from_file(state, state_fn)) {
			gtk_object_unref(GTK_OBJECT(state));
			state = specification->state;
			gtk_object_ref(GTK_OBJECT(state));
		}
	} else {
		state = specification->state;
		gtk_object_ref(GTK_OBJECT(state));
	}

	e_table = et_real_construct (e_table, etm, ete, specification, state);

	gtk_object_unref(GTK_OBJECT(specification));
	gtk_object_unref(GTK_OBJECT(state));

	return e_table;
}

GtkWidget *
e_table_new (ETableModel *etm, ETableExtras *ete, const char *spec, const char *state)
{
	ETable *e_table;

	g_return_val_if_fail(etm != NULL, NULL);
	g_return_val_if_fail(E_IS_TABLE_MODEL(etm), NULL);
	g_return_val_if_fail(ete == NULL || E_IS_TABLE_EXTRAS(ete), NULL);
	g_return_val_if_fail(spec != NULL, NULL);

	e_table = gtk_type_new (e_table_get_type ());

	e_table = e_table_construct (e_table, etm, ete, spec, state);
		
	return GTK_WIDGET (e_table);
}

GtkWidget *
e_table_new_from_spec_file (ETableModel *etm, ETableExtras *ete, const char *spec_fn, const char *state_fn)
{
	ETable *e_table;

	g_return_val_if_fail(etm != NULL, NULL);
	g_return_val_if_fail(E_IS_TABLE_MODEL(etm), NULL);
	g_return_val_if_fail(ete == NULL || E_IS_TABLE_EXTRAS(ete), NULL);
	g_return_val_if_fail(spec_fn != NULL, NULL);

	e_table = gtk_type_new (e_table_get_type ());

	e_table = e_table_construct_from_spec_file (e_table, etm, ete, spec_fn, state_fn);
		
	return GTK_WIDGET (e_table);
}



#if 0
static xmlNode *
et_build_column_spec (ETable *e_table)
{
	xmlNode *columns_shown;
	gint i;
	gint col_count;

	columns_shown = xmlNewNode (NULL, "columns-shown");

	col_count = e_table_header_count (e_table->header);
	for (i = 0; i < col_count; i++){
		gchar *text = g_strdup_printf ("%d", e_table_header_index(e_table->header, i));
		xmlNewChild (columns_shown, NULL, "column", text);
		g_free (text);
	}

	return columns_shown;
}

static xmlNode *
et_build_grouping_spec (ETable *e_table)
{
	xmlNode *node;
	xmlNode *grouping;
	int i;
	const int sort_count = e_table_sort_info_sorting_get_count (e_table->sort_info);
	const int group_count = e_table_sort_info_grouping_get_count (e_table->sort_info);
	
	grouping = xmlNewNode (NULL, "grouping");
	node = grouping;

	for (i = 0; i < group_count; i++) {
		ETableSortColumn column = e_table_sort_info_grouping_get_nth(e_table->sort_info, i);
		xmlNode *new_node = xmlNewChild(node, NULL, "group", NULL);

		e_xml_set_integer_prop_by_name (new_node, "column", column.column);
		e_xml_set_integer_prop_by_name (new_node, "ascending", column.ascending);
		node = new_node;
	}

	for (i = 0; i < sort_count; i++) {
		ETableSortColumn column = e_table_sort_info_sorting_get_nth(e_table->sort_info, i);
		xmlNode *new_node = xmlNewChild(node, NULL, "leaf", NULL);
		
		e_xml_set_integer_prop_by_name (new_node, "column", column.column);
		e_xml_set_integer_prop_by_name (new_node, "ascending", column.ascending);
		node = new_node;
	}

	return grouping;
}

static xmlDoc *
et_build_tree (ETable *e_table)
{
	xmlDoc *doc;
	xmlNode *root;

	doc = xmlNewDoc ("1.0");
	if (doc == NULL)
		return NULL;
	
	root = xmlNewDocNode (doc, NULL, "ETableSpecification", NULL);
	xmlDocSetRootElement (doc, root);
	xmlAddChild (root, et_build_column_spec (e_table));
	xmlAddChild (root, et_build_grouping_spec (e_table));

	return doc;
}

gchar *
e_table_get_specification (ETable *e_table)
{
	xmlDoc *doc;
	xmlChar *buffer;
	gint size;

	g_return_val_if_fail(e_table != NULL, NULL);
	g_return_val_if_fail(E_IS_TABLE(e_table), NULL);

	doc = et_build_tree (e_table);
	xmlDocDumpMemory (doc, &buffer, &size);
	xmlFreeDoc (doc);

	return buffer;
}

int
e_table_set_specification (ETable *e_table, const char *spec)
{
	xmlDoc *xmlSpec;
	int ret;

	g_return_val_if_fail(e_table != NULL, -1);
	g_return_val_if_fail(E_IS_TABLE(e_table), -1);
	g_return_val_if_fail(spec != NULL, -1);

	/* doesn't work yet, sigh */
	xmlSpec = xmlParseMemory ((char *)spec, strlen(spec));
	ret = et_real_set_specification(e_table, xmlSpec);
	xmlFreeDoc (xmlSpec);

	return ret;
}

void
e_table_save_specification (ETable *e_table, gchar *filename)
{
	xmlDoc *doc = et_build_tree (e_table);

	g_return_if_fail(e_table != NULL);
	g_return_if_fail(E_IS_TABLE(e_table));
	g_return_if_fail(filename != NULL);

	xmlSaveFile (filename, doc);
	xmlFreeDoc (doc);
}

int
e_table_load_specification (ETable *e_table, gchar *filename)
{
	xmlDoc *xmlSpec;
	int ret;

	g_return_val_if_fail(e_table != NULL, -1);
	g_return_val_if_fail(E_IS_TABLE(e_table), -1);
	g_return_val_if_fail(filename != NULL, -1);

	/* doesn't work yet, yay */
	xmlSpec = xmlParseFile (filename);
	ret = et_real_set_specification(e_table, xmlSpec);
	xmlFreeDoc (xmlSpec);

	return ret;
}
#endif

void
e_table_set_cursor_row (ETable *e_table, int row)
{
	g_return_if_fail(e_table != NULL);
	g_return_if_fail(E_IS_TABLE(e_table));
	g_return_if_fail(row >= 0);

	row = e_table_sorter_model_to_sorted(e_table->sorter, row);
	if (row != -1)
		e_table_group_set_cursor_row(e_table->group, row);
}

int
e_table_get_cursor_row (ETable *e_table)
{
	int row;
	g_return_val_if_fail(e_table != NULL, -1);
	g_return_val_if_fail(E_IS_TABLE(e_table), -1);

	row = e_table_group_get_cursor_row(e_table->group);
	row = e_table_sorter_sorted_to_model(e_table->sorter, row);
	return row;
}

void
e_table_selected_row_foreach     (ETable *e_table,
				  ETableForeachFunc callback,
				  gpointer closure)
{
	g_return_if_fail(e_table != NULL);
	g_return_if_fail(E_IS_TABLE(e_table));

	e_table_selection_model_foreach(e_table->selection,
					callback,
					closure);
}

void
e_table_select_all (ETable *table)
{
	g_return_if_fail (table != NULL);
	g_return_if_fail (E_IS_TABLE (table));
	
	e_table_selection_model_select_all (table->selection);
}

void
e_table_invert_selection (ETable *table)
{
	g_return_if_fail (table != NULL);
	g_return_if_fail (E_IS_TABLE (table));
	
	e_table_selection_model_invert_selection (table->selection);
}


EPrintable *
e_table_get_printable (ETable *e_table)
{
	g_return_val_if_fail(e_table != NULL, NULL);
	g_return_val_if_fail(E_IS_TABLE(e_table), NULL);

	return e_table_group_get_printable(e_table->group);
}

static void
et_get_arg (GtkObject *o, GtkArg *arg, guint arg_id)
{
	ETable *etable = E_TABLE (o);

	switch (arg_id){
	case ARG_TABLE_DRAW_FOCUS:
		GTK_VALUE_BOOL (*arg) = etable->draw_focus;
		break;
	}
}

typedef struct {
	char     *arg;
	gboolean  setting;
} bool_closure;

static void
et_set_arg (GtkObject *o, GtkArg *arg, guint arg_id)
{
	ETable *etable = E_TABLE (o);
	
	switch (arg_id){
	case ARG_LENGTH_THRESHOLD:
		etable->length_threshold = GTK_VALUE_INT (*arg);
		if (etable->group) {
			gnome_canvas_item_set (GNOME_CANVAS_ITEM(etable->group),
					       "length_threshold", GTK_VALUE_INT (*arg),
					       NULL);
		}
		break;
		
	case ARG_TABLE_DRAW_FOCUS:
		etable->draw_focus = GTK_VALUE_BOOL (*arg);
		if (etable->group) {
			gnome_canvas_item_set (GNOME_CANVAS_ITEM(etable->group),
					"drawfocus", GTK_VALUE_BOOL (*arg),
					NULL);
		}
		break;
	}
}
	
static void
set_scroll_adjustments   (ETable *table,
			  GtkAdjustment *hadjustment,
			  GtkAdjustment *vadjustment)
{
	if (vadjustment != NULL) {
		vadjustment->step_increment = 20;
		gtk_adjustment_changed(vadjustment);
	}

	gtk_layout_set_hadjustment (GTK_LAYOUT(table->table_canvas),
				    hadjustment);
	gtk_layout_set_vadjustment (GTK_LAYOUT(table->table_canvas),
				    vadjustment);

	if (table->header_canvas != NULL)
		gtk_layout_set_hadjustment (GTK_LAYOUT(table->header_canvas),
					    hadjustment);
}

gint 
e_table_get_next_row      (ETable *e_table,
			   gint    model_row)
{
	g_return_val_if_fail(e_table != NULL, -1);
	g_return_val_if_fail(E_IS_TABLE(e_table), -1);

	if (e_table->sorter) {
		int i;
		i = e_table_sorter_model_to_sorted(e_table->sorter, model_row);
		i++;
		if (i < e_table_model_row_count(e_table->model)) {
			return e_table_sorter_sorted_to_model(e_table->sorter, i);
		} else
			return -1;
	} else
		if (model_row < e_table_model_row_count(e_table->model) - 1)
			return model_row + 1;
		else
			return -1;
}

gint 
e_table_get_prev_row      (ETable *e_table,
			   gint    model_row)
{
	g_return_val_if_fail(e_table != NULL, -1);
	g_return_val_if_fail(E_IS_TABLE(e_table), -1);

	if (e_table->sorter) {
		int i;
		i = e_table_sorter_model_to_sorted(e_table->sorter, model_row);
		i--;
		if (i >= 0)
			return e_table_sorter_sorted_to_model(e_table->sorter, i);
		else
			return -1;
	} else
		return model_row - 1;
}

gint
e_table_model_to_view_row        (ETable *e_table,
				  gint    model_row)
{
	g_return_val_if_fail(e_table != NULL, -1);
	g_return_val_if_fail(E_IS_TABLE(e_table), -1);

	if (e_table->sorter)
		return e_table_sorter_model_to_sorted(e_table->sorter, model_row);
	else
		return model_row;
}

gint
e_table_view_to_model_row        (ETable *e_table,
				  gint    view_row)
{
	g_return_val_if_fail(e_table != NULL, -1);
	g_return_val_if_fail(E_IS_TABLE(e_table), -1);

	if (e_table->sorter)
		return e_table_sorter_sorted_to_model(e_table->sorter, view_row);
	else
		return view_row;
}

struct _ETableDragSourceSite 
{
	GdkModifierType    start_button_mask;
	GtkTargetList     *target_list;        /* Targets for drag data */
	GdkDragAction      actions;            /* Possible actions */
	GdkColormap       *colormap;	         /* Colormap for drag icon */
	GdkPixmap         *pixmap;             /* Icon for drag data */
	GdkBitmap         *mask;
	
	/* Stored button press information to detect drag beginning */
	gint               state;
	gint               x, y;
	gint               row, col;
};

typedef enum 
{
  GTK_DRAG_STATUS_DRAG,
  GTK_DRAG_STATUS_WAIT,
  GTK_DRAG_STATUS_DROP
} GtkDragStatus;

typedef struct _GtkDragDestInfo GtkDragDestInfo;  
typedef struct _GtkDragSourceInfo GtkDragSourceInfo;

struct _GtkDragDestInfo 
{
  GtkWidget         *widget;	   /* Widget in which drag is in */
  GdkDragContext    *context;	   /* Drag context */
  GtkDragSourceInfo *proxy_source; /* Set if this is a proxy drag */
  GtkSelectionData  *proxy_data;   /* Set while retrieving proxied data */
  gboolean           dropped : 1;     /* Set after we receive a drop */
  guint32            proxy_drop_time; /* Timestamp for proxied drop */
  gboolean           proxy_drop_wait : 1; /* Set if we are waiting for a
					   * status reply before sending
					   * a proxied drop on.
					   */
  gint               drop_x, drop_y; /* Position of drop */
};

struct _GtkDragSourceInfo 
{
  GtkWidget         *widget;
  GtkTargetList     *target_list; /* Targets for drag data */
  GdkDragAction      possible_actions; /* Actions allowed by source */
  GdkDragContext    *context;	  /* drag context */
  GtkWidget         *icon_window; /* Window for drag */
  GtkWidget         *ipc_widget;  /* GtkInvisible for grab, message passing */
  GdkCursor         *cursor;	  /* Cursor for drag */
  gint hot_x, hot_y;		  /* Hot spot for drag */
  gint button;			  /* mouse button starting drag */

  GtkDragStatus      status;	  /* drag status */
  GdkEvent          *last_event;  /* motion event waiting for response */

  gint               start_x, start_y; /* Initial position */
  gint               cur_x, cur_y;     /* Current Position */

  GList             *selections;  /* selections we've claimed */
  
  GtkDragDestInfo   *proxy_dest;  /* Set if this is a proxy drag */

  guint              drop_timeout;     /* Timeout for aborting drop */
  guint              destroy_icon : 1; /* If true, destroy icon_window
					*/
};

/* Drag & drop stuff. */
/* Target */
void e_table_drag_get_data (ETable         *table,
			    int             row,
			    int             col,
			    GdkDragContext *context,
			    GdkAtom         target,
			    guint32         time)
{
	g_return_if_fail(table != NULL);
	g_return_if_fail(E_IS_TABLE(table));

	table->drag_get_data_row = row;
	table->drag_get_data_col = col;
	gtk_drag_get_data(GTK_WIDGET(table),
			  context,
			  target,
			  time);
	
}

void e_table_drag_highlight   (ETable     *table,
			       int         row,
			       int         col) /* col == -1 to highlight entire row. */
{
	g_return_if_fail(table != NULL);
	g_return_if_fail(E_IS_TABLE(table));
}

void e_table_drag_unhighlight (ETable     *table)
{
	g_return_if_fail(table != NULL);
	g_return_if_fail(E_IS_TABLE(table));
}

void e_table_drag_dest_set   (ETable               *table,
			      GtkDestDefaults       flags,
			      const GtkTargetEntry *targets,
			      gint                  n_targets,
			      GdkDragAction         actions)
{
	g_return_if_fail(table != NULL);
	g_return_if_fail(E_IS_TABLE(table));

	gtk_drag_dest_set(GTK_WIDGET(table),
			  flags,
			  targets,
			  n_targets,
			  actions);
}

void e_table_drag_dest_set_proxy (ETable         *table,
				  GdkWindow      *proxy_window,
				  GdkDragProtocol protocol,
				  gboolean        use_coordinates)
{
	g_return_if_fail(table != NULL);
	g_return_if_fail(E_IS_TABLE(table));

	gtk_drag_dest_set_proxy(GTK_WIDGET(table),
				proxy_window,
				protocol,
				use_coordinates);
}

/* There probably should be functions for setting the targets
 * as a GtkTargetList
 */

void e_table_drag_dest_unset (GtkWidget          *widget)
{
	g_return_if_fail(widget != NULL);
	g_return_if_fail(E_IS_TABLE(widget));

	gtk_drag_dest_unset(widget);
}

/* Source side */

void e_table_drag_source_set  (ETable               *table,
			       GdkModifierType       start_button_mask,
			       const GtkTargetEntry *targets,
			       gint                  n_targets,
			       GdkDragAction         actions)
{
	ETableDragSourceSite *site;
	GtkWidget *canvas;

	g_return_if_fail(table != NULL);
	g_return_if_fail(E_IS_TABLE(table));

	canvas = GTK_WIDGET(table->table_canvas);
	site = table->site;

	gtk_widget_add_events (canvas,
			       gtk_widget_get_events (canvas) |
			       GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
			       GDK_BUTTON_MOTION_MASK);

	if (site) {
		if (site->target_list)
			gtk_target_list_unref (site->target_list);
	} else {
		site = g_new0 (ETableDragSourceSite, 1);

		table->drag_source_button_press_event_id =
			gtk_signal_connect (GTK_OBJECT (canvas), "button_press_event",
					    GTK_SIGNAL_FUNC (e_table_drag_source_event_cb),
					    table);
		table->drag_source_motion_notify_event_id =
			gtk_signal_connect (GTK_OBJECT (canvas), "motion_notify_event",
					    GTK_SIGNAL_FUNC (e_table_drag_source_event_cb),
					    table);

		table->site = site;
	}

	site->start_button_mask = start_button_mask;

	if (targets)
		site->target_list = gtk_target_list_new (targets, n_targets);
	else
		site->target_list = NULL;

	site->actions = actions;
}

void e_table_drag_source_unset (ETable        *table)
{
	ETableDragSourceSite *site;

	g_return_if_fail (table != NULL);
	g_return_if_fail (E_IS_TABLE(table));

	site = table->site;

	if (site) {
		gtk_signal_disconnect (GTK_OBJECT (table->table_canvas), table->drag_source_button_press_event_id);
		gtk_signal_disconnect (GTK_OBJECT (table->table_canvas), table->drag_source_motion_notify_event_id);
		g_free(site);
		table->site = NULL;
	}
}

/* There probably should be functions for setting the targets
 * as a GtkTargetList
 */

GdkDragContext *
e_table_drag_begin (ETable            *table,
		    int     	       row,
		    int     	       col,
		    GtkTargetList     *targets,
		    GdkDragAction      actions,
		    gint               button,
		    GdkEvent          *event)
{
	g_return_val_if_fail (table != NULL, NULL);
	g_return_val_if_fail (E_IS_TABLE(table), NULL);

	table->drag_row = row;
	table->drag_col = col;

	return gtk_drag_begin(GTK_WIDGET(table),
			      targets,
			      actions,
			      button,
			      event);
}

static void
e_table_compute_location(ETable *table,
			 GtkWidget *widget,
			 int x,
			 int y,
			 int *row,
			 int *col)
{
	if (!(row || col))
		return;
	e_table_group_compute_location(table->group, &x, &y, row, col);
}

static void
et_drag_begin (GtkWidget *widget,
	       GdkDragContext *context,
	       ETable *et)
{
	gtk_signal_emit (GTK_OBJECT (et),
			 et_signals [TABLE_DRAG_BEGIN],
			 et->drag_row,
			 et->drag_col,
			 context);
}

static void
et_drag_end (GtkWidget *widget,
	     GdkDragContext *context,
	     ETable *et)
{
	gtk_signal_emit (GTK_OBJECT (et),
			 et_signals [TABLE_DRAG_END],
			 et->drag_row,
			 et->drag_col,
			 context);
}

static void 
et_drag_data_get(GtkWidget *widget,
		 GdkDragContext *context,
		 GtkSelectionData *selection_data,
		 guint info,
		 guint time,
		 ETable *et)
{
	gtk_signal_emit (GTK_OBJECT (et),
			 et_signals [TABLE_DRAG_DATA_GET],
			 et->drag_row,
			 et->drag_col,
			 context,
			 selection_data,
			 info,
			 time);
}

static void 
et_drag_data_delete(GtkWidget *widget,
		    GdkDragContext *context,
		    ETable *et)
{
	gtk_signal_emit (GTK_OBJECT (et),
			 et_signals [TABLE_DRAG_DATA_DELETE],
			 et->drag_row,
			 et->drag_col,
			 context);
}

static void 
et_drag_leave(GtkWidget *widget,
	      GdkDragContext *context,
	      guint time,
	      ETable *et)
{
	gtk_signal_emit (GTK_OBJECT (et),
			 et_signals [TABLE_DRAG_LEAVE],
			 et->drop_row,
			 et->drop_col,
			 context,
			 time);
	et->drop_row = -1;
	et->drop_col = -1;
}

static gboolean
et_drag_motion(GtkWidget *widget,
	       GdkDragContext *context,
	       gint x,
	       gint y,
	       guint time,
	       ETable *et)
{
	gboolean ret_val;
	int row, col;
	e_table_compute_location(et,
				 widget,
				 x,
				 y,
				 &row,
				 &col);
	if (et->drop_row >= 0 && et->drop_col >= 0 &&
	    row != et->drop_row && col != et->drop_row) {
		gtk_signal_emit (GTK_OBJECT (et),
				 et_signals [TABLE_DRAG_LEAVE],
				 et->drop_row,
				 et->drop_col,
				 context,
				 time);
	}
	et->drop_row = row;
	et->drop_col = col;
	if (row >= 0 && col >= 0)
		gtk_signal_emit (GTK_OBJECT (et),
				 et_signals [TABLE_DRAG_MOTION],
				 et->drop_row,
				 et->drop_col,
				 context,
				 x,
				 y,
				 time,
				 &ret_val);
	return ret_val;
}

static gboolean
et_drag_drop(GtkWidget *widget,
	     GdkDragContext *context,
	     gint x,
	     gint y,
	     guint time,
	     ETable *et)
{
	gboolean ret_val;
	int row, col;
	e_table_compute_location(et,
				 widget,
				 x,
				 y,
				 &row,
				 &col);
	if (et->drop_row >= 0 && et->drop_col >= 0 &&
	    row != et->drop_row && col != et->drop_row) {
		gtk_signal_emit (GTK_OBJECT (et),
				 et_signals [TABLE_DRAG_LEAVE],
				 et->drop_row,
				 et->drop_col,
				 context,
				 time);
		if (row >= 0 && col >= 0)
			gtk_signal_emit (GTK_OBJECT (et),
					 et_signals [TABLE_DRAG_MOTION],
					 row,
					 col,
					 context,
					 x,
					 y,
					 time,
					 &ret_val);
	}
	et->drop_row = row;
	et->drop_col = col;
	if (row >= 0 && col >= 0)
		gtk_signal_emit (GTK_OBJECT (et),
				 et_signals [TABLE_DRAG_DROP],
				 et->drop_row,
				 et->drop_col,
				 context,
				 x,
				 y,
				 time,
				 &ret_val);
	et->drop_row = -1;
	et->drop_col = -1;
	return ret_val;
}

static void
et_drag_data_received(GtkWidget *widget,
		      GdkDragContext *context,
		      gint x,
		      gint y,
		      GtkSelectionData *selection_data,
		      guint info,
		      guint time,
		      ETable *et)
{
	int row, col;
	e_table_compute_location(et,
				 widget,
				 x,
				 y,
				 &row,
				 &col);
	gtk_signal_emit (GTK_OBJECT (et),
			 et_signals [TABLE_DRAG_DATA_RECEIVED],
			 row,
			 col,
			 context,
			 x,
			 y,
			 selection_data,
			 info,
			 time);
}

static gint
e_table_drag_source_event_cb (GtkWidget      *widget,
			      GdkEvent       *event,
			      ETable         *table)
{
	ETableDragSourceSite *site;
	site = table->site;

	switch (event->type) {
	case GDK_BUTTON_PRESS:
		if ((GDK_BUTTON1_MASK << (event->button.button - 1)) & site->start_button_mask) {
			int row, col;
			e_table_compute_location(table, widget, event->button.x, event->button.y, &row, &col);
			if (row >= 0 && col >= 0) {
				site->state |= (GDK_BUTTON1_MASK << (event->button.button - 1));
				site->x = event->button.x;
				site->y = event->button.y;
				site->row = row;
				site->col = col;
			}
		}
		break;
      
	case GDK_BUTTON_RELEASE:
		if ((GDK_BUTTON1_MASK << (event->button.button - 1)) & site->start_button_mask) {
			site->state &= ~(GDK_BUTTON1_MASK << (event->button.button - 1));
		}
		break;
	  
	case GDK_MOTION_NOTIFY:
		if (site->state & event->motion.state & site->start_button_mask) {
			/* FIXME: This is really broken and can leave us
			 * with a stuck grab
			 */
			int i;
			for (i=1; i<6; i++) {
				if (site->state & event->motion.state & 
				    GDK_BUTTON1_MASK << (i - 1))
					break;
			}
		  
			if (MAX (abs (site->x - event->motion.x),
				 abs (site->y - event->motion.y)) > 3) {
				GtkDragSourceInfo *info;
				GdkDragContext *context;
			  
				site->state = 0;
				context = e_table_drag_begin (table, site->row, site->col,
							      site->target_list,
							      site->actions, 
							      i, event);


				info = g_dataset_get_data (context, "gtk-info");

				if (!info->icon_window) {
					if (site->pixmap)
						gtk_drag_set_icon_pixmap (context,
									  site->colormap,
									  site->pixmap,
									  site->mask, -2, -2);
					else
						gtk_drag_set_icon_default (context);
				}
			  
				return TRUE;
			}
		}
		break;

	default:			/* hit for 2/3BUTTON_PRESS */
		break;
	}
	return FALSE;
}

static void
e_table_class_init (GtkObjectClass *object_class)
{
	ETableClass *klass = E_TABLE_CLASS(object_class);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(object_class);

	e_table_parent_class = gtk_type_class (PARENT_TYPE);

	object_class->destroy   	 = et_destroy;
	object_class->set_arg   	 = et_set_arg;
	object_class->get_arg   	 = et_get_arg;

	klass->cursor_change    	 = NULL;
	klass->double_click     	 = NULL;
	klass->right_click      	 = NULL;
	klass->click            	 = NULL;
	klass->key_press        	 = NULL;

	klass->table_drag_begin           	 = NULL;
	klass->table_drag_end             	 = NULL;
	klass->table_drag_data_get        	 = NULL;
	klass->table_drag_data_delete     	 = NULL;

	klass->table_drag_leave                = NULL;
	klass->table_drag_motion               = NULL;
	klass->table_drag_drop                 = NULL;
	klass->table_drag_data_received        = NULL;

	et_signals [CURSOR_CHANGE] =
		gtk_signal_new ("cursor_change",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (ETableClass, cursor_change),
				gtk_marshal_NONE__INT,
				GTK_TYPE_NONE, 1, GTK_TYPE_INT);

	et_signals [DOUBLE_CLICK] =
		gtk_signal_new ("double_click",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (ETableClass, double_click),
				gtk_marshal_NONE__INT,
				GTK_TYPE_NONE, 1, GTK_TYPE_INT);

	et_signals [RIGHT_CLICK] =
		gtk_signal_new ("right_click",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (ETableClass, right_click),
				e_marshal_INT__INT_INT_POINTER,
				GTK_TYPE_INT, 3, GTK_TYPE_INT, GTK_TYPE_INT, GTK_TYPE_POINTER);

	et_signals [CLICK] =
		gtk_signal_new ("click",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (ETableClass, click),
				e_marshal_INT__INT_INT_POINTER,
				GTK_TYPE_INT, 3, GTK_TYPE_INT, GTK_TYPE_INT, GTK_TYPE_POINTER);

	et_signals [KEY_PRESS] =
		gtk_signal_new ("key_press",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (ETableClass, key_press),
				e_marshal_INT__INT_INT_POINTER,
				GTK_TYPE_INT, 3, GTK_TYPE_INT, GTK_TYPE_INT, GTK_TYPE_POINTER);

	et_signals[TABLE_DRAG_BEGIN] =
		gtk_signal_new ("table_drag_begin",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (ETableClass, table_drag_begin),
				gtk_marshal_NONE__INT_INT_POINTER,
				GTK_TYPE_NONE, 3,
				GTK_TYPE_INT,
				GTK_TYPE_INT,
				GTK_TYPE_GDK_DRAG_CONTEXT);
	et_signals[TABLE_DRAG_END] =
		gtk_signal_new ("table_drag_end",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (ETableClass, table_drag_end),
				gtk_marshal_NONE__INT_INT_POINTER,
				GTK_TYPE_NONE, 3,
				GTK_TYPE_INT,
				GTK_TYPE_INT,
				GTK_TYPE_GDK_DRAG_CONTEXT);
	et_signals[TABLE_DRAG_DATA_GET] =
		gtk_signal_new ("table_drag_data_get",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (ETableClass, table_drag_data_get),
				e_marshal_NONE__INT_INT_POINTER_POINTER_UINT_UINT,
				GTK_TYPE_NONE, 6,
				GTK_TYPE_INT,
				GTK_TYPE_INT,
				GTK_TYPE_GDK_DRAG_CONTEXT,
				GTK_TYPE_SELECTION_DATA,
				GTK_TYPE_UINT,
				GTK_TYPE_UINT);
	et_signals[TABLE_DRAG_DATA_DELETE] =
		gtk_signal_new ("table_drag_data_delete",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (ETableClass, table_drag_data_delete),
				gtk_marshal_NONE__INT_INT_POINTER,
				GTK_TYPE_NONE, 3,
				GTK_TYPE_INT,
				GTK_TYPE_INT,
				GTK_TYPE_GDK_DRAG_CONTEXT);

	et_signals[TABLE_DRAG_LEAVE] =
		gtk_signal_new ("table_drag_leave",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (ETableClass, table_drag_leave),
				e_marshal_NONE__INT_INT_POINTER_UINT,
				GTK_TYPE_NONE, 4,
				GTK_TYPE_INT,
				GTK_TYPE_INT,
				GTK_TYPE_GDK_DRAG_CONTEXT,
				GTK_TYPE_UINT);
	et_signals[TABLE_DRAG_MOTION] =
		gtk_signal_new ("table_drag_motion",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (ETableClass, table_drag_motion),
				e_marshal_BOOL__INT_INT_POINTER_INT_INT_UINT,
				GTK_TYPE_BOOL, 6,
				GTK_TYPE_INT,
				GTK_TYPE_INT,
				GTK_TYPE_GDK_DRAG_CONTEXT,
				GTK_TYPE_INT,
				GTK_TYPE_INT,
				GTK_TYPE_UINT);
	et_signals[TABLE_DRAG_DROP] =
		gtk_signal_new ("table_drag_drop",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (ETableClass, table_drag_drop),
				e_marshal_BOOL__INT_INT_POINTER_INT_INT_UINT,
				GTK_TYPE_BOOL, 6,
				GTK_TYPE_INT,
				GTK_TYPE_INT,
				GTK_TYPE_GDK_DRAG_CONTEXT,
				GTK_TYPE_INT,
				GTK_TYPE_INT,
				GTK_TYPE_UINT);
	et_signals[TABLE_DRAG_DATA_RECEIVED] =
		gtk_signal_new ("table_drag_data_received",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (ETableClass, table_drag_data_received),
				e_marshal_NONE__INT_INT_POINTER_INT_INT_POINTER_UINT_UINT,
				GTK_TYPE_NONE, 8,
				GTK_TYPE_INT,
				GTK_TYPE_INT,
				GTK_TYPE_GDK_DRAG_CONTEXT,
				GTK_TYPE_INT,
				GTK_TYPE_INT,
				GTK_TYPE_SELECTION_DATA,
				GTK_TYPE_UINT,
				GTK_TYPE_UINT);
	
	gtk_object_class_add_signals (object_class, et_signals, LAST_SIGNAL);

	klass->set_scroll_adjustments = set_scroll_adjustments;
	
	widget_class->set_scroll_adjustments_signal =
		gtk_signal_new ("set_scroll_adjustments",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (ETableClass, set_scroll_adjustments),
				gtk_marshal_NONE__POINTER_POINTER,
				GTK_TYPE_NONE, 2, GTK_TYPE_ADJUSTMENT, GTK_TYPE_ADJUSTMENT);
	
	gtk_object_add_arg_type ("ETable::drawfocus", GTK_TYPE_BOOL,
				 GTK_ARG_READWRITE, ARG_TABLE_DRAW_FOCUS);
	gtk_object_add_arg_type ("ETable::length_threshold", GTK_TYPE_INT,
				 GTK_ARG_WRITABLE, ARG_LENGTH_THRESHOLD);
}

E_MAKE_TYPE(e_table, "ETable", ETable, e_table_class_init, e_table_init, PARENT_TYPE);

