/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the program; if not, see <http://www.gnu.org/licenses/>
 *
 *
 * Authors:
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "e-charset-picker.h"
#include "e-util/e-dialog-utils.h"

#include <string.h>
#include <iconv.h>

#include <glib/gi18n-lib.h>

typedef enum {
	E_CHARSET_UNKNOWN,
	E_CHARSET_ARABIC,
	E_CHARSET_BALTIC,
	E_CHARSET_CENTRAL_EUROPEAN,
	E_CHARSET_CHINESE,
	E_CHARSET_CYRILLIC,
	E_CHARSET_GREEK,
	E_CHARSET_HEBREW,
	E_CHARSET_JAPANESE,
	E_CHARSET_KOREAN,
	E_CHARSET_THAI,
	E_CHARSET_TURKISH,
	E_CHARSET_UNICODE,
	E_CHARSET_WESTERN_EUROPEAN,
	E_CHARSET_WESTERN_EUROPEAN_NEW
} ECharsetClass;

static const gchar *classnames[] = {
	N_("Unknown"),
	N_("Arabic"),
	N_("Baltic"),
	N_("Central European"),
	N_("Chinese"),
	N_("Cyrillic"),
	N_("Greek"),
	N_("Hebrew"),
	N_("Japanese"),
	N_("Korean"),
	N_("Thai"),
	N_("Turkish"),
	N_("Unicode"),
	N_("Western European"),
	N_("Western European, New"),
};

typedef struct {
	const gchar *name;
	ECharsetClass class;
	const gchar *subclass;
} ECharset;

/* This list is based on what other mailers/browsers support. There's
 * not a lot of point in using, say, ISO-8859-3, if anything that can
 * read that can read UTF8 too.
 */
/* To Translators: Character set "Logical Hebrew" */
static ECharset charsets[] = {
	{ "ISO-8859-6", E_CHARSET_ARABIC, NULL },
	{ "ISO-8859-13", E_CHARSET_BALTIC, NULL },
	{ "ISO-8859-4", E_CHARSET_BALTIC, NULL },
	{ "ISO-8859-2", E_CHARSET_CENTRAL_EUROPEAN, NULL },
	{ "Big5", E_CHARSET_CHINESE, N_("Traditional") },
	{ "BIG5HKSCS", E_CHARSET_CHINESE, N_("Traditional") },
	{ "EUC-TW", E_CHARSET_CHINESE, N_("Traditional") },
	{ "GB18030", E_CHARSET_CHINESE, N_("Simplified") },
	{ "GB2312", E_CHARSET_CHINESE, N_("Simplified") },
	{ "HZ", E_CHARSET_CHINESE, N_("Simplified") },
	{ "ISO-2022-CN", E_CHARSET_CHINESE, N_("Simplified") },
	{ "KOI8-R", E_CHARSET_CYRILLIC, NULL },
	{ "Windows-1251", E_CHARSET_CYRILLIC, NULL },
	{ "KOI8-U", E_CHARSET_CYRILLIC, N_("Ukrainian") },
	{ "ISO-8859-5", E_CHARSET_CYRILLIC, NULL },
	{ "ISO-8859-7", E_CHARSET_GREEK, NULL },
	{ "ISO-8859-8", E_CHARSET_HEBREW, N_("Visual") },
	{ "ISO-2022-JP", E_CHARSET_JAPANESE, NULL },
	{ "EUC-JP", E_CHARSET_JAPANESE, NULL },
	{ "Shift_JIS", E_CHARSET_JAPANESE, NULL },
	{ "EUC-KR", E_CHARSET_KOREAN, NULL },
	{ "TIS-620", E_CHARSET_THAI, NULL },
	{ "ISO-8859-9", E_CHARSET_TURKISH, NULL },
	{ "UTF-8", E_CHARSET_UNICODE, NULL },
	{ "UTF-7", E_CHARSET_UNICODE, NULL },
	{ "ISO-8859-1", E_CHARSET_WESTERN_EUROPEAN, NULL },
	{ "ISO-8859-15", E_CHARSET_WESTERN_EUROPEAN_NEW, NULL },
};

static void
select_item (GtkMenuShell *menu_shell, GtkWidget *item)
{
	gtk_menu_shell_select_item (menu_shell, item);
	gtk_menu_shell_deactivate (menu_shell);
}

static void
activate (GtkWidget *item, gpointer menu)
{
	g_object_set_data ((GObject *) menu, "activated_item", item);
}

static GtkWidget *
add_charset (GtkWidget *menu, ECharset *charset, gboolean free_name)
{
	GtkWidget *item;
	gchar *label;

	if (charset->subclass) {
		label = g_strdup_printf ("%s, %s (%s)",
					 _(classnames[charset->class]),
					 _(charset->subclass),
					 charset->name);
	} else if (charset->class) {
		label = g_strdup_printf ("%s (%s)",
					 _(classnames[charset->class]),
					 charset->name);
	} else {
		label = g_strdup (charset->name);
	}

	item = gtk_menu_item_new_with_label (label);
	g_object_set_data_full ((GObject *) item, "charset",
				(gpointer) charset->name, free_name ? g_free : NULL);
	g_free (label);

	gtk_widget_show (item);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	g_signal_connect (item, "activate", G_CALLBACK (activate), menu);

	return item;
}

static gboolean
add_other_charset (GtkWidget *menu, GtkWidget *other, const gchar *new_charset)
{
	ECharset charset = { NULL, E_CHARSET_UNKNOWN, NULL };
	GtkWidget *item;
	iconv_t ic;

	ic = iconv_open ("UTF-8", new_charset);
	if (ic == (iconv_t)-1) {
		GtkWidget *window = gtk_widget_get_ancestor (other, GTK_TYPE_WINDOW);
		e_notice (window, GTK_MESSAGE_ERROR,
			  _("Unknown character set: %s"), new_charset);
		return FALSE;
	}
	iconv_close (ic);

	/* Temporarily remove the "Other..." item */
	g_object_ref (other);
	gtk_container_remove (GTK_CONTAINER (menu), other);

	/* Create new menu item */
	charset.name = g_strdup (new_charset);
	item = add_charset (menu, &charset, TRUE);

	/* And re-add "Other..." */
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), other);
	g_object_unref (other);

	g_object_set_data_full ((GObject *) menu, "other_charset",
				g_strdup (new_charset), g_free);

	g_object_set_data ((GObject *) menu, "activated_item", item);
	select_item (GTK_MENU_SHELL (menu), item);

	return TRUE;
}

static void
activate_entry (GtkWidget *entry, GtkDialog *dialog)
{
	gtk_dialog_response (dialog, GTK_RESPONSE_OK);
}

static void
activate_other (GtkWidget *item, gpointer menu)
{
	GtkWidget *window, *entry, *label, *vbox, *hbox;
	gchar *old_charset, *new_charset;
	GtkDialog *dialog;

	window = gtk_widget_get_toplevel (menu);
	if (!GTK_WIDGET_TOPLEVEL (window))
		window = gtk_widget_get_ancestor (item, GTK_TYPE_WINDOW);

	old_charset = g_object_get_data(menu, "other_charset");

	dialog = GTK_DIALOG (gtk_dialog_new_with_buttons (_("Character Encoding"),
							  GTK_WINDOW (window),
							  GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
							  GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
							  GTK_STOCK_OK, GTK_RESPONSE_OK,
							  NULL));

	gtk_dialog_set_has_separator (dialog, FALSE);
	gtk_dialog_set_default_response (dialog, GTK_RESPONSE_OK);

	vbox = gtk_vbox_new (FALSE, 6);
	gtk_container_set_border_width (GTK_CONTAINER (vbox), 12);
	gtk_box_pack_start (GTK_BOX (dialog->vbox), vbox, TRUE, TRUE, 0);
	gtk_widget_show (vbox);

	label = gtk_label_new (_("Enter the character set to use"));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
	gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);
	gtk_widget_show (label);

	hbox = gtk_hbox_new (FALSE, 12);
	gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
	gtk_widget_show (hbox);

	label = gtk_label_new ("");
	gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
	gtk_widget_show (label);

	entry = gtk_entry_new ();
	gtk_box_pack_start (GTK_BOX (hbox), entry, TRUE, TRUE, 0);
	gtk_widget_show (entry);

	if (old_charset)
		gtk_entry_set_text (GTK_ENTRY (entry), old_charset);
	g_signal_connect (entry, "activate",
			  G_CALLBACK (activate_entry), dialog);

	gtk_container_set_border_width (GTK_CONTAINER (dialog->vbox), 0);
	gtk_container_set_border_width (GTK_CONTAINER (dialog->action_area), 12);

	gtk_widget_show_all (GTK_WIDGET (dialog));

	g_object_ref (dialog);
	if (gtk_dialog_run (dialog) == GTK_RESPONSE_OK) {
		new_charset = (gchar *)gtk_entry_get_text (GTK_ENTRY (entry));

		if (*new_charset) {
			if (add_other_charset (menu, item, new_charset)) {
				gtk_widget_destroy (GTK_WIDGET (dialog));
				g_object_unref (dialog);
				return;
			}
		}
	}
	gtk_widget_destroy (GTK_WIDGET (dialog));
	g_object_unref (dialog);

	/* Revert to previous selection */
	select_item (GTK_MENU_SHELL (menu), g_object_get_data(G_OBJECT(menu), "activated_item"));
}

/**
 * e_charset_picker_new:
 * @default_charset: the default character set, or %NULL to use the
 * locale character set.
 *
 * This creates an option menu widget and fills it in with a selection
 * of available character sets. The @default_charset (or locale character
 * set if @default_charset is %NULL) will be listed first, and selected
 * by default (except that iso-8859-1 will always be used instead of
 * US-ASCII). Any other character sets of the same language class as
 * the default will be listed next, followed by the remaining character
 * sets, a separator, and an "Other..." menu item, which can be used to
 * select other charsets.
 *
 * Return value: an option menu widget, filled in and with signals
 * attached.
 */
GtkWidget *
e_charset_picker_new (const gchar *default_charset)
{
	GtkWidget *menu, *item;
	gint def, i;
	const gchar *locale_charset;

	g_get_charset (&locale_charset);
	if (!g_ascii_strcasecmp (locale_charset, "US-ASCII"))
		locale_charset = "iso-8859-1";

	if (!default_charset)
		default_charset = locale_charset;
	for (def = 0; def < G_N_ELEMENTS (charsets); def++) {
		if (!g_ascii_strcasecmp (charsets[def].name, default_charset))
			break;
	}

	menu = gtk_menu_new ();
	for (i = 0; i < G_N_ELEMENTS (charsets); i++) {
		item = add_charset (menu, &charsets[i], FALSE);
		if (i == def) {
			activate (item, menu);
			select_item (GTK_MENU_SHELL (menu), item);
		}
	}

	/* do the Unknown/Other section */
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_menu_item_new ());

	if (def == G_N_ELEMENTS (charsets)) {
		ECharset other = { NULL, E_CHARSET_UNKNOWN, NULL };

		/* Add an entry for @default_charset */
		other.name = g_strdup (default_charset);
		item = add_charset (menu, &other, TRUE);
		activate (item, menu);
		select_item (GTK_MENU_SHELL (menu), item);
		g_object_set_data_full ((GObject *) menu, "other_charset",
					g_strdup (default_charset), g_free);
		def++;
	}

	item = gtk_menu_item_new_with_label (_("Other..."));
	g_signal_connect (item, "activate", G_CALLBACK (activate_other), menu);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

	gtk_widget_show_all (menu);

	return menu;
}

/**
 * e_charset_picker_get_charset:
 * @menu: a character set menu from e_charset_picker_new()
 *
 * Return value: the currently-selected character set in @picker,
 * which must be freed with g_free().
 **/
gchar *
e_charset_picker_get_charset (GtkWidget *menu)
{
	GtkWidget *item;
	gchar *charset;

	g_return_val_if_fail (GTK_IS_MENU (menu), NULL);

	item = gtk_menu_get_active (GTK_MENU (menu));
	charset = g_object_get_data ((GObject *) item, "charset");

	return g_strdup (charset);
}

/**
 * e_charset_add_radio_actions:
 * @action_group: a #GtkActionGroup
 * @action_prefix: a prefix for action names, or %NULL
 * @default_charset: the default character set, or %NULL to use the
 *                   locale character set
 * @callback: a callback function for actions in the group, or %NULL
 * @user_data: user data to be passed to @callback, or %NULL
 *
 * Adds a set of #GtkRadioActions for available character sets to
 * @action_group.  The @default_charset (or locale character set if
 * @default_charset is %NULL) will be added first, and selected by
 * default (except that iso-8859-1 will always be used instead of
 * US-ASCII).  Any other character sets of the same language class as
 * the default will be added next, followed by the remaining character
 * sets.
 **/
GSList *
e_charset_add_radio_actions (GtkActionGroup *action_group,
                             const gchar *action_prefix,
                             const gchar *default_charset,
                             GCallback callback,
                             gpointer user_data)
{
	GtkRadioAction *action = NULL;
	GSList *group = NULL;
	const gchar *locale_charset;
	gint def, ii;

	g_return_val_if_fail (GTK_IS_ACTION_GROUP (action_group), NULL);

	if (action_prefix == NULL)
		action_prefix = "";

	g_get_charset (&locale_charset);
	if (!g_ascii_strcasecmp (locale_charset, "US-ASCII"))
		locale_charset = "iso-8859-1";

	if (default_charset == NULL)
		default_charset = locale_charset;
	for (def = 0; def < G_N_ELEMENTS (charsets); def++)
		if (!g_ascii_strcasecmp (charsets[def].name, default_charset))
			break;

	for (ii = 0; ii < G_N_ELEMENTS (charsets); ii++) {
		const gchar *charset_name;
		gchar *action_name;
		gchar *escaped_name;
		gchar *charset_label;
		gchar **str_array;

		charset_name = charsets[ii].name;
		action_name = g_strconcat (action_prefix, charset_name, NULL);

		/* Escape underlines in the character set name so
		 * they're not treated as GtkLabel mnemonics. */
		str_array = g_strsplit (charset_name, "_", -1);
		escaped_name = g_strjoinv ("__", str_array);
		g_strfreev (str_array);

		if (charsets[ii].subclass != NULL)
			charset_label = g_strdup_printf (
				"%s, %s (%s)",
				gettext (classnames[charsets[ii].class]),
				gettext (charsets[ii].subclass),
				escaped_name);
		else if (charsets[ii].class != E_CHARSET_UNKNOWN)
			charset_label = g_strdup_printf (
				"%s (%s)",
				gettext (classnames[charsets[ii].class]),
				escaped_name);
		else
			charset_label = g_strdup (escaped_name);

		/* XXX Add a tooltip! */
		action = gtk_radio_action_new (
			action_name, charset_label, NULL, NULL, ii);

		/* Character set name is static so no need to free it. */
		g_object_set_data (
			G_OBJECT (action), "charset",
			(gpointer) charset_name);

		gtk_radio_action_set_group (action, group);
		group = gtk_radio_action_get_group (action);

		if (callback != NULL)
			g_signal_connect (
				action, "changed", callback, user_data);

		gtk_action_group_add_action (
			action_group, GTK_ACTION (action));

		g_object_unref (action);

		g_free (action_name);
		g_free (escaped_name);
		g_free (charset_label);
	}

	if (def == G_N_ELEMENTS (charsets)) {
		const gchar *charset_name;
		gchar *action_name;
		gchar *charset_label;
		gchar **str_array;

		charset_name = default_charset;
		action_name = g_strconcat (action_prefix, charset_name, NULL);

		/* Escape underlines in the character set name so
		 * they're not treated as GtkLabel mnemonics. */
		str_array = g_strsplit (charset_name, "_", -1);
		charset_label = g_strjoinv ("__", str_array);
		g_strfreev (str_array);

		/* XXX Add a tooltip! */
		action = gtk_radio_action_new (
			action_name, charset_label, NULL, NULL, def);

		/* Character set name is static so no need to free it. */
		g_object_set_data (
			G_OBJECT (action), "charset",
			(gpointer) charset_name);

		gtk_radio_action_set_group (action, group);
		group = gtk_radio_action_get_group (action);

		if (callback != NULL)
			g_signal_connect (
				action, "changed", callback, user_data);

		gtk_action_group_add_action (
			action_group, GTK_ACTION (action));

		g_object_unref (action);

		g_free (action_name);
		g_free (charset_label);
	}

	/* Any of the actions in the action group will do. */
	if (action != NULL)
		gtk_radio_action_set_current_value (action, def);

	return group;
}
